#include "trading/venue.h"

#include "trading/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DummyVenue: a realistic simulated venue adapter for exercising the
 * live-order path before a real exchange/broker adapter exists.
 *
 * Differences from MockVenue (which is a minimal paper matcher):
 *  - simulated one-way latency: events become visible only after
 *    latency_ns has elapsed on the venue clock
 *  - partial fills: each match attempt fills fill_fraction of the
 *    remainder; orders can rest across multiple market events
 *  - deterministic rejects: every reject_every-th order is rejected
 *  - order rate limit: max_orders_per_sec submissions
 *
 * Still NOT a realistic market simulator: no queue position, no adverse
 * selection, fills always at top-of-book. */

#define DUMMY_MAX_ORDERS 512
#define DUMMY_MAX_EVENTS 4096
#define DUMMY_MAX_QUOTES 64

typedef struct {
    char instrument[TS_MAX_CODE];
    double bid, ask, last;
    ts_ts_ns quote_ns;
    bool has_quote;
} dummy_quote;

typedef struct {
    ts_order ord;
    ts_ts_ns submit_ns;
} dummy_working;

typedef struct {
    ts_venue_event ev;
    ts_ts_ns ready_ns; /* event visible to poll() at/after this time */
} dummy_slot;

struct dummy_venue_impl {
    dummy_quote quotes[DUMMY_MAX_QUOTES];
    size_t quote_count;
    dummy_working working[DUMMY_MAX_ORDERS];
    size_t working_count;
    dummy_slot events[DUMMY_MAX_EVENTS];
    size_t ev_head, ev_tail, ev_count;
    bool connected;
    ts_ts_ns latency_ns;
    double fill_fraction;      /* (0,1]; 1.0 = full fill per attempt */
    int reject_every;          /* 0 = never; N = reject every Nth submit */
    uint64_t submit_count;
    double max_orders_per_sec; /* 0 = unlimited */
    ts_ts_ns window_start_ns;
    uint32_t window_count;
    uint64_t exec_seq;
    ts_metrics *metrics;
    ts_clock clock;
};

static ts_ts_ns now_ns(struct dummy_venue_impl *d)
{
    return d->clock.now_ns(d->clock.ctx);
}

static dummy_quote *find_quote(struct dummy_venue_impl *d, const char *instr)
{
    for (size_t i = 0; i < d->quote_count; ++i) {
        if (strcmp(d->quotes[i].instrument, instr) == 0) {
            return &d->quotes[i];
        }
    }
    return NULL;
}

static dummy_quote *get_quote(struct dummy_venue_impl *d, const char *instr)
{
    dummy_quote *q = find_quote(d, instr);
    if (q) return q;
    if (d->quote_count >= DUMMY_MAX_QUOTES) return NULL;
    q = &d->quotes[d->quote_count++];
    memset(q, 0, sizeof(*q));
    ts_copy_str(q->instrument, sizeof(q->instrument), instr);
    return q;
}

static bool push_event(struct dummy_venue_impl *d, const ts_venue_event *ev)
{
    if (d->ev_count >= DUMMY_MAX_EVENTS) {
        ts_metrics_inc(d->metrics, TS_MET_RECON_BREAKS, 1);
        return false;
    }
    dummy_slot *s = &d->events[d->ev_tail];
    s->ev = *ev;
    s->ready_ns = now_ns(d) + d->latency_ns;
    d->ev_tail = (d->ev_tail + 1) % DUMMY_MAX_EVENTS;
    d->ev_count++;
    return true;
}

static void ev_ack(struct dummy_venue_impl *d, const ts_order *ord,
                   const char *venue_id)
{
    ts_venue_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = TS_VEV_ACK;
    ts_copy_str(ev.client_id, sizeof(ev.client_id), ord->client_id);
    ts_copy_str(ev.venue_order_id, sizeof(ev.venue_order_id), venue_id);
    ts_copy_str(ev.instrument, sizeof(ev.instrument), ord->instrument);
    ev.ts_ns = now_ns(d);
    push_event(d, &ev);
}

static void ev_exec(struct dummy_venue_impl *d, const dummy_working *w,
                    double qty, double price)
{
    ts_venue_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = TS_VEV_EXEC;
    ts_copy_str(ev.client_id, sizeof(ev.client_id), w->ord.client_id);
    ts_copy_str(ev.venue_order_id, sizeof(ev.venue_order_id),
                w->ord.venue_order_id);
    ts_copy_str(ev.exec.client_id, sizeof(ev.exec.client_id), w->ord.client_id);
    ts_copy_str(ev.exec.venue_order_id, sizeof(ev.exec.venue_order_id),
                w->ord.venue_order_id);
    snprintf(ev.exec.venue_exec_id, sizeof(ev.exec.venue_exec_id),
             "DX%llu", (unsigned long long)++d->exec_seq);
    ts_copy_str(ev.exec.instrument, sizeof(ev.exec.instrument), w->ord.instrument);
    ev.exec.side = w->ord.side;
    ev.exec.qty = qty;
    ev.exec.price = price;
    ev.exec.ts_ns = now_ns(d);
    ev.ts_ns = ev.exec.ts_ns;
    push_event(d, &ev);
}

static void ev_simple(struct dummy_venue_impl *d, ts_venue_event_type t,
                      const char *client_id, const char *instr,
                      const char *reason)
{
    ts_venue_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = t;
    ts_copy_str(ev.client_id, sizeof(ev.client_id), client_id);
    if (instr) ts_copy_str(ev.instrument, sizeof(ev.instrument), instr);
    if (reason) ts_copy_str(ev.reason, sizeof(ev.reason), reason);
    ev.ts_ns = now_ns(d);
    push_event(d, &ev);
}

static bool marketable(struct dummy_venue_impl *d, const ts_order *ord,
                       double *fill_px)
{
    dummy_quote *q = find_quote(d, ord->instrument);
    if (!q || !q->has_quote) return false;
    if (ord->type == TS_ORD_MARKET) {
        *fill_px = ord->side == TS_SIDE_BUY ? q->ask : q->bid;
        if (*fill_px <= 0.0) *fill_px = q->last;
        return *fill_px > 0.0;
    }
    if (ord->side == TS_SIDE_BUY && ord->price >= q->ask) {
        *fill_px = q->ask;
        return true;
    }
    if (ord->side == TS_SIDE_SELL && ord->price <= q->bid) {
        *fill_px = q->bid;
        return true;
    }
    return false;
}

static void try_fill(struct dummy_venue_impl *d, dummy_working *w)
{
    double remaining = w->ord.qty - w->ord.filled_qty;
    if (remaining <= 1e-12) return;
    double px;
    if (!marketable(d, &w->ord, &px)) return;

    double chunk = remaining;
    if (d->fill_fraction < 1.0) {
        chunk = remaining * d->fill_fraction;
        if (chunk <= 1e-12) chunk = remaining; /* avoid stall */
    }
    ev_exec(d, w, chunk, px);
    w->ord.filled_qty += chunk;

    /* IOC/FOK/DAY: expire unfilled remainder immediately after attempt */
    double left = w->ord.qty - w->ord.filled_qty;
    if (left > 1e-12 &&
        (w->ord.tif == TS_TIF_IOC || w->ord.tif == TS_TIF_FOK ||
         w->ord.tif == TS_TIF_DAY)) {
        ev_simple(d, TS_VEV_EXPIRED, w->ord.client_id, w->ord.instrument,
                  w->ord.tif == TS_TIF_FOK ? "FOK incomplete" : "remainder expired");
        w->ord.filled_qty = w->ord.qty; /* mark done; caller removes */
    }
}

static ts_status dummy_submit(venue_t *v, const ts_order *ord)
{
    struct dummy_venue_impl *d = v->impl;
    if (!d->connected) return TS_ERR_BAD_STATE;

    for (size_t i = 0; i < d->working_count; ++i) {
        if (strcmp(d->working[i].ord.client_id, ord->client_id) == 0) {
            return TS_OK; /* idempotent */
        }
    }
    if (ord->qty <= 0.0 || ord->instrument[0] == '\0') {
        ev_simple(d, TS_VEV_REJECT, ord->client_id, ord->instrument,
                  "invalid order fields");
        return TS_OK;
    }
    /* rate limit */
    if (d->max_orders_per_sec > 0.0) {
        ts_ts_ns now = now_ns(d);
        if (now - d->window_start_ns >= 1000000000LL) {
            d->window_start_ns = now;
            d->window_count = 0;
        }
        if (d->window_count >= (uint32_t)d->max_orders_per_sec) {
            ev_simple(d, TS_VEV_REJECT, ord->client_id, ord->instrument,
                      "rate limit exceeded");
            return TS_OK;
        }
        d->window_count++;
    }
    /* deterministic reject-every-N */
    d->submit_count++;
    if (d->reject_every > 0 &&
        (int)(d->submit_count % (uint64_t)d->reject_every) == 0) {
        ev_simple(d, TS_VEV_REJECT, ord->client_id, ord->instrument,
                  "dummy deterministic reject");
        return TS_OK;
    }
    if (d->working_count >= DUMMY_MAX_ORDERS) {
        ev_simple(d, TS_VEV_REJECT, ord->client_id, ord->instrument,
                  "venue capacity");
        return TS_OK;
    }

    dummy_working *w = &d->working[d->working_count++];
    memset(w, 0, sizeof(*w));
    w->ord = *ord;
    w->submit_ns = now_ns(d);
    if (w->ord.venue_order_id[0] == '\0') {
        snprintf(w->ord.venue_order_id, sizeof(w->ord.venue_order_id),
                 "DV%llu", (unsigned long long)++d->exec_seq);
    }
    ev_ack(d, ord, w->ord.venue_order_id);
    try_fill(d, w);
    return TS_OK;
}

static ts_status dummy_cancel(venue_t *v, const char *client_id)
{
    struct dummy_venue_impl *d = v->impl;
    if (!d->connected) return TS_ERR_BAD_STATE;
    for (size_t i = 0; i < d->working_count; ++i) {
        if (strcmp(d->working[i].ord.client_id, client_id) == 0) {
            ev_simple(d, TS_VEV_CANCEL_ACK, client_id,
                      d->working[i].ord.instrument, NULL);
            d->working[i] = d->working[--d->working_count];
            return TS_OK;
        }
    }
    return TS_ERR_NOT_FOUND;
}

static ts_status dummy_amend(venue_t *v, const char *client_id,
                             double new_price, double new_qty)
{
    struct dummy_venue_impl *d = v->impl;
    if (!d->connected) return TS_ERR_BAD_STATE;
    for (size_t i = 0; i < d->working_count; ++i) {
        dummy_working *w = &d->working[i];
        if (strcmp(w->ord.client_id, client_id) != 0) continue;
        if (new_price > 0.0) w->ord.price = new_price;
        if (new_qty > w->ord.filled_qty) w->ord.qty = new_qty;
        ts_venue_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = TS_VEV_AMENDED;
        ts_copy_str(ev.client_id, sizeof(ev.client_id), client_id);
        ts_copy_str(ev.instrument, sizeof(ev.instrument), w->ord.instrument);
        ev.exec.price = w->ord.price;
        ev.exec.qty = w->ord.qty;
        ev.ts_ns = now_ns(d);
        push_event(d, &ev);
        return TS_OK;
    }
    return TS_ERR_NOT_FOUND;
}

static ts_status dummy_cancel_all(venue_t *v)
{
    struct dummy_venue_impl *d = v->impl;
    if (!d->connected) return TS_ERR_BAD_STATE;
    while (d->working_count > 0) {
        dummy_working *w = &d->working[0];
        ev_simple(d, TS_VEV_CANCEL_ACK, w->ord.client_id, w->ord.instrument,
                  NULL);
        d->working[0] = d->working[--d->working_count];
    }
    return TS_OK;
}

static void dummy_on_market_event(venue_t *v, const ts_mkt_event *ev)
{
    struct dummy_venue_impl *d = v->impl;
    if (!ev) return;
    dummy_quote *q = get_quote(d, ev->instrument);
    if (!q) return;
    if (ev->type == TS_MKT_BOOK && ev->book_depth > 0) {
        q->bid = ev->bid_px[0];
        q->ask = ev->ask_px[0];
        q->last = (q->bid + q->ask) * 0.5;
    } else {
        q->bid = ev->bid;
        q->ask = ev->ask;
        q->last = ev->last;
    }
    q->quote_ns = ev->ts_ns;
    q->has_quote = true;

    for (size_t i = 0; i < d->working_count;) {
        try_fill(d, &d->working[i]);
        double remaining = d->working[i].ord.qty - d->working[i].ord.filled_qty;
        bool done = remaining <= 1e-12;
        if (!done && d->working[i].ord.filled_qty >= d->working[i].ord.qty - 1e-12) {
            done = true;
        }
        /* IOC/FOK/DAY orders that attempted a fill are expired by
         * try_fill marking filled_qty=qty */
        if (done || (d->working[i].ord.tif != TS_TIF_GTC &&
                     d->working[i].ord.filled_qty > 0.0 &&
                     remaining <= 1e-12)) {
            d->working[i] = d->working[--d->working_count];
        } else {
            i++;
        }
    }
}

static ts_status dummy_poll(venue_t *v, ts_venue_event *out)
{
    struct dummy_venue_impl *d = v->impl;
    if (d->ev_count == 0) return TS_ERR_NOT_FOUND;
    ts_ts_ns now = now_ns(d);
    if (d->events[d->ev_head].ready_ns > now) {
        return TS_ERR_NOT_FOUND; /* head not ready yet (latency) */
    }
    *out = d->events[d->ev_head].ev;
    d->ev_head = (d->ev_head + 1) % DUMMY_MAX_EVENTS;
    d->ev_count--;
    return TS_OK;
}

static bool dummy_connected(venue_t *v)
{
    return ((struct dummy_venue_impl *)v->impl)->connected;
}

static ts_status dummy_open_orders(venue_t *v, ts_venue_open_order *out,
                                   size_t cap, size_t *count)
{
    struct dummy_venue_impl *d = v->impl;
    if (!d->connected) return TS_ERR_BAD_STATE;
    size_t n = 0;
    for (size_t i = 0; i < d->working_count && n < cap; ++i) {
        dummy_working *w = &d->working[i];
        if (w->ord.filled_qty >= w->ord.qty - 1e-12) continue;
        ts_venue_open_order *oo = &out[n++];
        memset(oo, 0, sizeof(*oo));
        ts_copy_str(oo->client_id, sizeof(oo->client_id), w->ord.client_id);
        ts_copy_str(oo->venue_order_id, sizeof(oo->venue_order_id),
                    w->ord.venue_order_id);
        ts_copy_str(oo->instrument, sizeof(oo->instrument), w->ord.instrument);
        oo->side = w->ord.side;
        oo->qty = w->ord.qty;
        oo->filled_qty = w->ord.filled_qty;
        oo->price = w->ord.price;
    }
    if (count) *count = n;
    return TS_OK;
}

static void dummy_destroy(venue_t *v)
{
    if (!v) return;
    free(v->impl);
    free(v);
}

static const venue_ops_t g_dummy_ops = {
    .name = "dummy",
    .submit = dummy_submit,
    .cancel = dummy_cancel,
    .on_market_event = dummy_on_market_event,
    .poll = dummy_poll,
    .connected = dummy_connected,
    .amend = dummy_amend,
    .cancel_all = dummy_cancel_all,
    .open_orders = dummy_open_orders,
    .destroy = dummy_destroy,
};

venue_t *venue_dummy_create(ts_metrics *metrics, ts_clock clock)
{
    venue_t *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    struct dummy_venue_impl *d = calloc(1, sizeof(*d));
    if (!d) { free(v); return NULL; }
    d->connected = true;
    d->fill_fraction = 1.0;
    d->metrics = metrics;
    d->clock = clock;
    v->ops = &g_dummy_ops;
    v->impl = d;
    return v;
}

void venue_dummy_set_latency_ns(venue_t *v, ts_ts_ns ns)
{
    if (v && v->impl)
        ((struct dummy_venue_impl *)v->impl)->latency_ns = ns;
}

void venue_dummy_set_fill_fraction(venue_t *v, double frac)
{
    if (v && v->impl && frac > 0.0 && frac <= 1.0)
        ((struct dummy_venue_impl *)v->impl)->fill_fraction = frac;
}

void venue_dummy_set_reject_every(venue_t *v, int n)
{
    if (v && v->impl)
        ((struct dummy_venue_impl *)v->impl)->reject_every = n;
}

void venue_dummy_set_max_orders_per_sec(venue_t *v, double n)
{
    if (v && v->impl)
        ((struct dummy_venue_impl *)v->impl)->max_orders_per_sec = n;
}

void venue_dummy_set_connected(venue_t *v, bool connected)
{
    if (v && v->impl)
        ((struct dummy_venue_impl *)v->impl)->connected = connected;
}
