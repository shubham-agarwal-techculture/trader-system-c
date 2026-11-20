#ifndef TRADING_VENUE_H
#define TRADING_VENUE_H

#include "trading/events.h"
#include "trading/metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Venue adapter contract. A venue may be a direct exchange API, a broker
 * API, or a simulated venue. All venue-specific semantics stay behind this
 * interface; the EMS only sees normalized orders/events. */

typedef struct venue venue_t;

/* Normalized view of an order open at the venue, used for reconciliation. */
typedef struct {
    char client_id[TS_MAX_ID];
    char venue_order_id[TS_MAX_ID];
    char instrument[TS_MAX_CODE];
    ts_side side;
    double qty;
    double filled_qty;
    double price;
} ts_venue_open_order;

typedef struct venue_ops {
    const char *name;
    /* Submit an approved order. Must be idempotent on client_id.
     * Returns TS_OK if the request was accepted for processing; the
     * resulting ACK/EXEC/REJECT events arrive via poll(). */
    ts_status (*submit)(venue_t *v, const ts_order *ord);
    /* Request cancellation. */
    ts_status (*cancel)(venue_t *v, const char *client_id);
    /* Feed a normalized market event (venue may use it for matching). */
    void (*on_market_event)(venue_t *v, const ts_mkt_event *ev);
    /* Drain one pending venue event. TS_ERR_NOT_FOUND when empty. */
    ts_status (*poll)(venue_t *v, ts_venue_event *out);
    /* Session/health status. */
    bool (*connected)(venue_t *v);
    /* Optional: amend/replace an open order. new_qty <= 0 keeps qty. */
    ts_status (*amend)(venue_t *v, const char *client_id, double new_price,
                       double new_qty);
    /* Optional: cancel all open orders; venue emits per-order
     * CANCEL_ACK events via poll(). */
    ts_status (*cancel_all)(venue_t *v);
    /* Optional: list orders the venue believes are open/working.
     * Returns TS_OK and fills out/count. NULL if unsupported — callers
     * must treat that as "reconciliation unavailable", not "no orders". */
    ts_status (*open_orders)(venue_t *v, ts_venue_open_order *out,
                             size_t cap, size_t *count);
    void (*destroy)(venue_t *v);
} venue_ops_t;

struct venue {
    const venue_ops_t *ops;
    void *impl;
};

static inline const char *venue_name(const venue_t *v)
{
    return v && v->ops ? v->ops->name : "?";
}

/* Mock/paper venue constructor (src/ems/venue_mock.c). */
venue_t *venue_mock_create(ts_metrics *metrics, ts_clock clock);
void venue_mock_set_reject_all(venue_t *v, bool reject);
void venue_mock_set_latency_ns(venue_t *v, ts_ts_ns latency_ns);
void venue_mock_set_connected(venue_t *v, bool connected);

/* Replay venue constructor (src/ems/venue_replay.c): replays scripted
 * venue events from a file for deterministic contract/fault tests. */
venue_t *venue_replay_create(const char *path, ts_metrics *metrics,
                             ts_clock clock);

/* Dummy venue (src/ems/venue_dummy.c): realistic simulated venue with
 * configurable latency, partial fills, deterministic rejects, and rate
 * limits — for exercising the live-order path pre-real-adapter. */
venue_t *venue_dummy_create(ts_metrics *metrics, ts_clock clock);
void venue_dummy_set_latency_ns(venue_t *v, ts_ts_ns ns);
void venue_dummy_set_fill_fraction(venue_t *v, double frac);
void venue_dummy_set_reject_every(venue_t *v, int n);
void venue_dummy_set_max_orders_per_sec(venue_t *v, double n);
void venue_dummy_set_connected(venue_t *v, bool connected);

#ifdef __cplusplus
}
#endif

#endif /* TRADING_VENUE_H */
