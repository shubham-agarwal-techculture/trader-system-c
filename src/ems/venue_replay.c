#include "trading/venue.h"

#include "trading/log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ReplayVenue: a deterministic venue that replays a scripted event file.
 * Used for contract tests and fault injection (duplicate execs,
 * out-of-order events, UNKNOWN states, rejects).
 *
 * Script format (one event per line, '#' comments, blank lines ok):
 *   ACK      <client_id> <venue_order_id>
 *   EXEC     <client_id> <venue_exec_id> <instrument> <BUY|SELL> <qty> <price> [fee]
 *   REJECT   <client_id> <reason words...>
 *   CANCEL   <client_id>
 *   EXPIRED  <client_id> [reason words...]
 *   UNKNOWN  <client_id> [reason words...]
 *
 * Unlike MockVenue, submit() does NOT auto-ack — scripts must include
 * ACK lines explicitly so tests control exact event ordering. */

#define REPLAY_MAX_EVENTS 8192
#define REPLAY_MAX_ORDERS 1024

typedef struct {
    ts_venue_event ev;
} replay_slot;

typedef struct {
    char client_id[TS_MAX_ID];
    char venue_order_id[TS_MAX_ID];
    char instrument[TS_MAX_CODE];
    ts_side side;
    double qty;
    double filled;
    bool closed; /* terminal event drained */
} replay_order;

struct replay_venue_impl {
    replay_slot events[REPLAY_MAX_EVENTS];
    size_t ev_head;
    size_t ev_tail;
    size_t ev_count;
    size_t ev_capacity;
    replay_order orders[REPLAY_MAX_ORDERS];
    size_t order_count;
    uint64_t venue_seq;
    bool connected;
    ts_metrics *metrics;
    ts_clock clock;
};

static ts_ts_ns now_ns(struct replay_venue_impl *m)
{
    return m->clock.now_ns(m->clock.ctx);
}

static bool push_event(struct replay_venue_impl *m, const ts_venue_event *ev)
{
    if (m->ev_count >= m->ev_capacity) {
        ts_metrics_inc(m->metrics, TS_MET_RECON_BREAKS, 1);
        return false;
    }
    m->events[m->ev_tail].ev = *ev;
    m->ev_tail = (m->ev_tail + 1) % m->ev_capacity;
    m->ev_count++;
    return true;
}

static void make_event(struct replay_venue_impl *m, ts_venue_event *ev,
                       ts_venue_event_type type, const char *client_id)
{
    memset(ev, 0, sizeof(*ev));
    ev->type = type;
    ts_copy_str(ev->client_id, sizeof(ev->client_id), client_id);
    ev->ts_ns = now_ns(m);
}

/* ---------- script parsing ---------- */

static char *skip_ws(char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static char *next_token(char **pp)
{
    char *p = skip_ws(*pp);
    if (*p == '\0' || *p == '\n' || *p == '\r') {
        *pp = p;
        return NULL;
    }
    char *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    if (*p) {
        *p++ = '\0';
    }
    *pp = p;
    return start;
}

static char *rest_of_line(char **pp)
{
    char *p = skip_ws(*pp);
    char *start = p;
    while (*p && *p != '\n' && *p != '\r') p++;
    if (*p) {
        *p = '\0';
    }
    *pp = p;
    return start;
}

static void parse_exec_line(struct replay_venue_impl *m, char *rest,
                            ts_venue_event *ev)
{
    /* rest: <client_id> <exec_id> <instr> <side> <qty> <price> [fee] */
    char *save = rest;
    char *cid = next_token(&save);
    char *eid = next_token(&save);
    char *instr = next_token(&save);
    char *side = next_token(&save);
    char *qty = next_token(&save);
    char *price = next_token(&save);
    char *fee = next_token(&save);
    if (!cid || !instr || !qty || !price) {
        return;
    }
    ev->exec.side = (side && strcmp(side, "SELL") == 0) ? TS_SIDE_SELL
                                                      : TS_SIDE_BUY;
    ev->exec.qty = strtod(qty, NULL);
    ev->exec.price = strtod(price, NULL);
    ev->exec.fee = fee ? strtod(fee, NULL) : 0.0;
    ev->exec.ts_ns = ev->ts_ns;
    ts_copy_str(ev->exec.client_id, sizeof(ev->exec.client_id), cid);
    ts_copy_str(ev->exec.instrument, sizeof(ev->exec.instrument), instr);
    if (eid) {
        ts_copy_str(ev->exec.venue_exec_id, sizeof(ev->exec.venue_exec_id), eid);
    } else {
        snprintf(ev->exec.venue_exec_id, sizeof(ev->exec.venue_exec_id),
                 "RE%llu", (unsigned long long)++m->venue_seq);
    }
    ts_copy_str(ev->client_id, sizeof(ev->client_id), cid);
    ts_copy_str(ev->instrument, sizeof(ev->instrument), instr);
}

static bool load_script(struct replay_venue_impl *m, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return false;
    }
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#' || *p == '\n') {
            continue;
        }
        char *tag = next_token(&p);
        if (!tag) {
            continue;
        }
        ts_venue_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.ts_ns = now_ns(m);

        if (strcmp(tag, "ACK") == 0) {
            char *rest = p;
            char *cid = next_token(&rest);
            char *vid = next_token(&rest);
            if (!cid) continue;
            make_event(m, &ev, TS_VEV_ACK, cid);
            if (vid) {
                ts_copy_str(ev.venue_order_id, sizeof(ev.venue_order_id), vid);
            }
        } else if (strcmp(tag, "EXEC") == 0) {
            make_event(m, &ev, TS_VEV_EXEC, "");
            parse_exec_line(m, p, &ev);
            if (ev.client_id[0] == '\0') continue;
        } else if (strcmp(tag, "REJECT") == 0) {
            char *rest = p;
            char *cid = next_token(&rest);
            if (!cid) continue;
            make_event(m, &ev, TS_VEV_REJECT, cid);
            ts_copy_str(ev.reason, sizeof(ev.reason), rest_of_line(&rest));
        } else if (strcmp(tag, "CANCEL") == 0) {
            char *rest = p;
            char *cid = next_token(&rest);
            if (!cid) continue;
            make_event(m, &ev, TS_VEV_CANCEL_ACK, cid);
        } else if (strcmp(tag, "EXPIRED") == 0) {
            char *rest = p;
            char *cid = next_token(&rest);
            if (!cid) continue;
            make_event(m, &ev, TS_VEV_EXPIRED, cid);
            ts_copy_str(ev.reason, sizeof(ev.reason), rest_of_line(&rest));
        } else if (strcmp(tag, "UNKNOWN") == 0) {
            char *rest = p;
            char *cid = next_token(&rest);
            if (!cid) continue;
            make_event(m, &ev, TS_VEV_UNKNOWN, cid);
            ts_copy_str(ev.reason, sizeof(ev.reason), rest_of_line(&rest));
        } else {
            continue; /* unknown tag: skip */
        }
        push_event(m, &ev);
    }
    fclose(fp);
    return true;
}

/* ---------- venue_ops ---------- */

static replay_order *find_order(struct replay_venue_impl *m,
                                const char *client_id)
{
    for (size_t i = 0; i < m->order_count; ++i) {
        if (strcmp(m->orders[i].client_id, client_id) == 0) {
            return &m->orders[i];
        }
    }
    return NULL;
}

static ts_status replay_submit(venue_t *v, const ts_order *ord)
{
    struct replay_venue_impl *m = v->impl;
    if (!m->connected) {
        return TS_ERR_BAD_STATE;
    }
    if (find_order(m, ord->client_id)) {
        return TS_OK; /* idempotent */
    }
    if (m->order_count >= REPLAY_MAX_ORDERS) {
        return TS_ERR_FULL;
    }
    replay_order *ro = &m->orders[m->order_count++];
    memset(ro, 0, sizeof(*ro));
    ts_copy_str(ro->client_id, sizeof(ro->client_id), ord->client_id);
    ts_copy_str(ro->venue_order_id, sizeof(ro->venue_order_id),
                ord->venue_order_id);
    ts_copy_str(ro->instrument, sizeof(ro->instrument), ord->instrument);
    ro->side = ord->side;
    ro->qty = ord->qty;
    /* No auto-ACK: the script controls all outbound events. */
    return TS_OK;
}

static ts_status replay_cancel(venue_t *v, const char *client_id)
{
    struct replay_venue_impl *m = v->impl;
    replay_order *ro = find_order(m, client_id);
    if (!ro) {
        return TS_ERR_NOT_FOUND;
    }
    ro->closed = true;
    /* Script supplies the CANCEL_ACK; nothing auto-emitted. */
    return TS_OK;
}

static void replay_on_market_event(venue_t *v, const ts_mkt_event *ev)
{
    (void)v; (void)ev; /* replay venue ignores market data */
}

static void note_drained(struct replay_venue_impl *m, const ts_venue_event *ev)
{
    /* Track order closure/fills so open_orders stays meaningful. */
    replay_order *ro = find_order(m, ev->client_id);
    if (!ro) {
        return;
    }
    switch (ev->type) {
    case TS_VEV_REJECT:
    case TS_VEV_CANCEL_ACK:
    case TS_VEV_EXPIRED:
        ro->closed = true;
        break;
    case TS_VEV_EXEC:
        ro->filled += ev->exec.qty;
        if (ro->filled >= ro->qty - 1e-12) {
            ro->closed = true;
        }
        break;
    default:
        break;
    }
}

static ts_status replay_poll(venue_t *v, ts_venue_event *out)
{
    struct replay_venue_impl *m = v->impl;
    if (m->ev_count == 0) {
        return TS_ERR_NOT_FOUND;
    }
    *out = m->events[m->ev_head].ev;
    m->ev_head = (m->ev_head + 1) % m->ev_capacity;
    m->ev_count--;
    note_drained(m, out);
    return TS_OK;
}

static bool replay_connected(venue_t *v)
{
    return ((struct replay_venue_impl *)v->impl)->connected;
}

static ts_status replay_open_orders(venue_t *v, ts_venue_open_order *out,
                                    size_t cap, size_t *count)
{
    struct replay_venue_impl *m = v->impl;
    size_t n = 0;
    for (size_t i = 0; i < m->order_count && n < cap; ++i) {
        replay_order *ro = &m->orders[i];
        if (ro->closed || ro->filled >= ro->qty - 1e-12) {
            continue;
        }
        ts_venue_open_order *oo = &out[n++];
        memset(oo, 0, sizeof(*oo));
        ts_copy_str(oo->client_id, sizeof(oo->client_id), ro->client_id);
        ts_copy_str(oo->venue_order_id, sizeof(oo->venue_order_id),
                    ro->venue_order_id);
        ts_copy_str(oo->instrument, sizeof(oo->instrument), ro->instrument);
        oo->side = ro->side;
        oo->qty = ro->qty;
        oo->filled_qty = ro->filled;
        oo->price = 0.0;
    }
    if (count) {
        *count = n;
    }
    return TS_OK;
}

static void replay_destroy(venue_t *v)
{
    if (!v) {
        return;
    }
    free(v->impl);
    free(v);
}

static const venue_ops_t g_replay_ops = {
    .name = "replay",
    .submit = replay_submit,
    .cancel = replay_cancel,
    .on_market_event = replay_on_market_event,
    .poll = replay_poll,
    .connected = replay_connected,
    .open_orders = replay_open_orders,
    .destroy = replay_destroy,
};

venue_t *venue_replay_create(const char *path, ts_metrics *metrics,
                             ts_clock clock)
{
    if (!path) {
        return NULL;
    }
    venue_t *v = calloc(1, sizeof(*v));
    if (!v) {
        return NULL;
    }
    struct replay_venue_impl *m = calloc(1, sizeof(*m));
    if (!m) {
        free(v);
        return NULL;
    }
    m->ev_capacity = REPLAY_MAX_EVENTS;
    m->connected = true;
    m->metrics = metrics;
    m->clock = clock;
    if (!load_script(m, path)) {
        free(m);
        free(v);
        return NULL;
    }
    v->ops = &g_replay_ops;
    v->impl = m;
    return v;
}
