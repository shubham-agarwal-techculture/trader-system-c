#include "trading/venue.h"

#include "trading/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* MockVenue: deterministic paper venue used for Phase 1-2 development and
 * tests. It is NOT a realistic market simulator — fills occur only against
 * the most recent quote it has seen, and there is no queue position or
 * adverse selection modeling. Treat paper P&L as plumbing verification,
 * not performance evidence. */

#define MOCK_MAX_ORDERS 512
#define MOCK_MAX_EVENTS 4096
#define MOCK_MAX_INSTRUMENTS 64

typedef struct {
    char instrument[TS_MAX_CODE];
    double bid;
    double ask;
    double last;
    ts_ts_ns quote_ns;
    bool has_quote;
} mock_quote;

typedef struct {
    ts_order ord;
    ts_ts_ns submit_ns;
} mock_working;

typedef struct {
    ts_venue_event ev;
} mock_event_slot;

struct mock_venue_impl {
    mock_quote quotes[MOCK_MAX_INSTRUMENTS];
    size_t quote_count;
    mock_working working[MOCK_MAX_ORDERS];
    size_t working_count;
    mock_event_slot events[MOCK_MAX_EVENTS];
    size_t ev_head;
    size_t ev_tail;
    size_t ev_count;
    bool reject_all;
    ts_ts_ns latency_ns;
    bool connected;
    uint64_t exec_seq;
    ts_metrics *metrics;
    ts_clock clock;
};

static ts_ts_ns now_ns(struct mock_venue_impl *m)
{
    return m->clock.now_ns(m->clock.ctx);
}

static mock_quote *find_quote(struct mock_venue_impl *m, const char *instr)
{
    for (size_t i = 0; i < m->quote_count; ++i) {
        if (strcmp(m->quotes[i].instrument, instr) == 0) {
            return &m->quotes[i];
        }
    }
    return NULL;
}

static mock_quote *get_quote(struct mock_venue_impl *m, const char *instr)
{
    mock_quote *q = find_quote(m, instr);
    if (q) {
        return q;
    }
    if (m->quote_count >= MOCK_MAX_INSTRUMENTS) {
        return NULL;
    }
    q = &m->quotes[m->quote_count++];
    memset(q, 0, sizeof(*q));
    ts_copy_str(q->instrument, sizeof(q->instrument), instr);
    return q;
}

static bool push_event(struct mock_venue_impl *m, const ts_venue_event *ev)
{
    if (m->ev_count >= MOCK_MAX_EVENTS) {
        ts_metrics_inc(m->metrics, TS_MET_RECON_BREAKS, 1);
        return false;
    }
    m->events[m->ev_tail].ev = *ev;
    m->ev_tail = (m->ev_tail + 1) % MOCK_MAX_EVENTS;
    m->ev_count++;
    return true;
}

static void push_ack(struct mock_venue_impl *m, const ts_order *ord,
                     const char *venue_id)
{
    ts_venue_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = TS_VEV_ACK;
    ts_copy_str(ev.client_id, sizeof(ev.client_id), ord->client_id);
    ts_copy_str(ev.venue_order_id, sizeof(ev.venue_order_id), venue_id);
    ts_copy_str(ev.instrument, sizeof(ev.instrument), ord->instrument);
    ev.ts_ns = now_ns(m);
    push_event(m, &ev);
}

static void push_exec(struct mock_venue_impl *m, const mock_working *w,
                      double qty, double price)
{
    ts_venue_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = TS_VEV_EXEC;
    ts_copy_str(ev.client_id, sizeof(ev.client_id), w->ord.client_id);
    ts_copy_str(ev.venue_order_id, sizeof(ev.venue_order_id),
                w->ord.venue_order_id);
    ts_copy_str(ev.exec.client_id, sizeof(ev.exec.client_id),
                w->ord.client_id);
    ts_copy_str(ev.exec.venue_order_id, sizeof(ev.exec.venue_order_id),
                w->ord.venue_order_id);
    snprintf(ev.exec.venue_exec_id, sizeof(ev.exec.venue_exec_id),
             "X%llu", (unsigned long long)++m->exec_seq);
    ts_copy_str(ev.exec.instrument, sizeof(ev.exec.instrument),
                w->ord.instrument);
    ev.exec.side = w->ord.side;
    ev.exec.qty = qty;
    ev.exec.price = price;
    ev.exec.fee = 0.0;
    ev.exec.ts_ns = now_ns(m);
    ev.ts_ns = ev.exec.ts_ns;
    push_event(m, &ev);
    ts_metrics_inc(m->metrics, TS_MET_EXECUTIONS, 0); /* counted at OMS */
}

static void push_reject(struct mock_venue_impl *m, const ts_order *ord,
                        const char *reason)
{
    ts_venue_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = TS_VEV_REJECT;
    ts_copy_str(ev.client_id, sizeof(ev.client_id), ord->client_id);
    ts_copy_str(ev.instrument, sizeof(ev.instrument), ord->instrument);
    ts_copy_str(ev.reason, sizeof(ev.reason), reason);
    ev.ts_ns = now_ns(m);
    push_event(m, &ev);
}

/* Attempt to fill a working order against the current quote. */
static void try_fill(struct mock_venue_impl *m, mock_working *w)
{
    mock_quote *q = find_quote(m, w->ord.instrument);
    if (!q || !q->has_quote) {
        return;
    }
    double remaining = w->ord.qty - w->ord.filled_qty;
    if (remaining <= 1e-12) {
        return;
    }

    bool marketable = false;
    double fill_px = 0.0;
    if (w->ord.type == TS_ORD_MARKET) {
        marketable = true;
        fill_px = w->ord.side == TS_SIDE_BUY ? q->ask : q->bid;
        if (fill_px <= 0.0) {
            fill_px = q->last;
        }
    } else {
        /* LIMIT / STOP_LIMIT: fill if price crosses. */
        if (w->ord.side == TS_SIDE_BUY && w->ord.price >= q->ask) {
            marketable = true;
            fill_px = q->ask;
        } else if (w->ord.side == TS_SIDE_SELL && w->ord.price <= q->bid) {
            marketable = true;
            fill_px = q->bid;
        }
    }
    if (!marketable || fill_px <= 0.0) {
        return;
    }

    double fill_qty = remaining;
    push_exec(m, w, fill_qty, fill_px);
    w->ord.filled_qty += fill_qty;
}

/* True if the order can fill completely right now. */
static bool marketable_now(struct mock_venue_impl *m, const ts_order *ord)
{
    mock_quote *q = find_quote(m, ord->instrument);
    if (!q || !q->has_quote) {
        return false;
    }
    if (ord->type == TS_ORD_MARKET) {
        return true;
    }
    if (ord->side == TS_SIDE_BUY && ord->price >= q->ask) {
        return true;
    }
    if (ord->side == TS_SIDE_SELL && ord->price <= q->bid) {
        return true;
    }
    return false;
}

static ts_status mock_submit(venue_t *v, const ts_order *ord)
{
    struct mock_venue_impl *m = v->impl;
    if (!m->connected) {
        return TS_ERR_BAD_STATE;
    }

    /* Idempotency: already-known client order. */
    for (size_t i = 0; i < m->working_count; ++i) {
        if (strcmp(m->working[i].ord.client_id, ord->client_id) == 0) {
            push_ack(m, ord, m->working[i].ord.venue_order_id);
            return TS_OK;
        }
    }

    if (m->reject_all) {
        push_reject(m, ord, "mock venue reject_all enabled");
        return TS_OK;
    }
    if (ord->qty <= 0.0 || ord->instrument[0] == '\0') {
        push_reject(m, ord, "invalid order fields");
        return TS_OK;
    }

    if (m->working_count >= MOCK_MAX_ORDERS) {
        push_reject(m, ord, "venue order capacity reached");
        return TS_OK;
    }

    mock_working *w = &m->working[m->working_count++];
    memset(w, 0, sizeof(*w));
    w->ord = *ord;
    w->submit_ns = now_ns(m);

    /* Venue assigns its own id if the EMS didn't. */
    if (w->ord.venue_order_id[0] == '\0') {
        snprintf(w->ord.venue_order_id, sizeof(w->ord.venue_order_id),
                 "MV%llu", (unsigned long long)++m->exec_seq);
    }

    push_ack(m, ord, w->ord.venue_order_id);

    /* FOK: reject if not fully fillable now. IOC: expire if nothing fills. */
    if ((w->ord.tif == TS_TIF_FOK || w->ord.tif == TS_TIF_IOC) &&
        w->ord.filled_qty <= 0.0 && !marketable_now(m, &w->ord)) {
        if (w->ord.tif == TS_TIF_FOK) {
            push_reject(m, &w->ord, "FOK not fillable");
        } else {
            ts_venue_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = TS_VEV_EXPIRED;
            ts_copy_str(ev.client_id, sizeof(ev.client_id), w->ord.client_id);
            ts_copy_str(ev.instrument, sizeof(ev.instrument), w->ord.instrument);
            ts_copy_str(ev.reason, sizeof(ev.reason), "IOC no fill");
            ev.ts_ns = now_ns(m);
            push_event(m, &ev);
        }
        m->working_count--; /* remove the just-added order */
        return TS_OK;
    }

    /* Immediate match attempt against latest quote. */
    try_fill(m, w);

    /* IOC that partially filled: expire the remainder. */
    if (w->ord.tif == TS_TIF_IOC && w->ord.filled_qty > 0.0 &&
        w->ord.filled_qty < w->ord.qty - 1e-12) {
        ts_venue_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = TS_VEV_EXPIRED;
        ts_copy_str(ev.client_id, sizeof(ev.client_id), w->ord.client_id);
        ts_copy_str(ev.instrument, sizeof(ev.instrument), w->ord.instrument);
        ts_copy_str(ev.reason, sizeof(ev.reason), "IOC remainder expired");
        ev.ts_ns = now_ns(m);
        push_event(m, &ev);
        m->working_count--;
        return TS_OK;
    }
    return TS_OK;
}

static ts_status mock_cancel(venue_t *v, const char *client_id)
{
    struct mock_venue_impl *m = v->impl;
    if (!m->connected) {
        return TS_ERR_BAD_STATE;
    }
    for (size_t i = 0; i < m->working_count; ++i) {
        if (strcmp(m->working[i].ord.client_id, client_id) == 0) {
            ts_venue_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = TS_VEV_CANCEL_ACK;
            ts_copy_str(ev.client_id, sizeof(ev.client_id), client_id);
            ts_copy_str(ev.instrument, sizeof(ev.instrument),
                        m->working[i].ord.instrument);
            ev.ts_ns = now_ns(m);
            push_event(m, &ev);
            /* Remove from working set. */
            m->working[i] = m->working[--m->working_count];
            return TS_OK;
        }
    }
    return TS_ERR_NOT_FOUND;
}

static void mock_on_market_event(venue_t *v, const ts_mkt_event *ev)
{
    struct mock_venue_impl *m = v->impl;
    if (!ev) {
        return;
    }
    mock_quote *q = get_quote(m, ev->instrument);
    if (!q) {
        return;
    }
    q->bid = ev->bid;
    q->ask = ev->ask;
    q->last = ev->last;
    q->quote_ns = ev->ts_ns;
    q->has_quote = true;

    /* Re-evaluate resting orders against the new quote. */
    for (size_t i = 0; i < m->working_count;) {
        double before = m->working[i].ord.filled_qty;
        try_fill(m, &m->working[i]);
        if (m->working[i].ord.filled_qty >= m->working[i].ord.qty - 1e-12) {
            m->working[i] = m->working[--m->working_count];
        } else {
            (void)before;
            i++;
        }
    }
}

static ts_status mock_poll(venue_t *v, ts_venue_event *out)
{
    struct mock_venue_impl *m = v->impl;
    if (m->ev_count == 0) {
        return TS_ERR_NOT_FOUND;
    }
    *out = m->events[m->ev_head].ev;
    m->ev_head = (m->ev_head + 1) % MOCK_MAX_EVENTS;
    m->ev_count--;
    return TS_OK;
}

static bool mock_connected(venue_t *v)
{
    struct mock_venue_impl *m = v->impl;
    return m->connected;
}

static void mock_destroy(venue_t *v)
{
    if (!v) {
        return;
    }
    free(v->impl);
    free(v);
}

static const venue_ops_t g_mock_ops = {
    .name = "mock",
    .submit = mock_submit,
    .cancel = mock_cancel,
    .on_market_event = mock_on_market_event,
    .poll = mock_poll,
    .connected = mock_connected,
    .destroy = mock_destroy,
};

venue_t *venue_mock_create(ts_metrics *metrics, ts_clock clock)
{
    venue_t *v = calloc(1, sizeof(*v));
    if (!v) {
        return NULL;
    }
    struct mock_venue_impl *m = calloc(1, sizeof(*m));
    if (!m) {
        free(v);
        return NULL;
    }
    m->connected = true;
    m->metrics = metrics;
    m->clock = clock;
    v->ops = &g_mock_ops;
    v->impl = m;
    return v;
}

void venue_mock_set_reject_all(venue_t *v, bool reject)
{
    if (v && v->impl) {
        ((struct mock_venue_impl *)v->impl)->reject_all = reject;
    }
}

void venue_mock_set_latency_ns(venue_t *v, ts_ts_ns latency_ns)
{
    if (v && v->impl) {
        ((struct mock_venue_impl *)v->impl)->latency_ns = latency_ns;
    }
}
