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

#ifdef __cplusplus
}
#endif

#endif /* TRADING_VENUE_H */
