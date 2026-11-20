#ifndef TRADING_OMS_H
#define TRADING_OMS_H

#include "trading/events.h"
#include "trading/journal.h"
#include "trading/metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Order Management System. Owns the authoritative order lifecycle.
 * All mutations are journaled so state can be rebuilt on restart. */

typedef struct oms oms_t;

typedef struct {
    oms_t *oms;
    journal_t *journal;   /* may be NULL: in-memory only */
    ts_metrics *metrics;  /* may be NULL */
    ts_clock clock;
} oms_config;

oms_t *oms_create(const oms_config *cfg, size_t capacity);
void oms_destroy(oms_t *o);

/* Submit a new order request. Idempotent on client_id: resubmitting the
 * same client_id returns the existing order and TS_OK. */
ts_status oms_submit(oms_t *o, const ts_order_request *req, ts_order *out);

const ts_order *oms_find(const oms_t *o, const char *client_id);
const ts_order *oms_find_venue(const oms_t *o, const char *venue_order_id);
const ts_order *oms_at(const oms_t *o, size_t idx);
size_t oms_count(const oms_t *o);
size_t oms_open_count(const oms_t *o);

/* Risk gate result, called by the engine after rms_check_order. */
ts_status oms_mark_risk(oms_t *o, const char *client_id, bool approved,
                        const char *rule, const char *msg);

/* Venue-facing transitions (called by EMS). */
ts_status oms_on_ack(oms_t *o, const char *client_id,
                     const char *venue_order_id);
ts_status oms_on_execution(oms_t *o, const ts_execution *exec);
ts_status oms_on_reject(oms_t *o, const char *client_id, const char *reason);
ts_status oms_on_cancel(oms_t *o, const char *client_id);
/* Amend price/qty of a non-terminal order (venue-confirmed). */
ts_status oms_on_amend(oms_t *o, const char *client_id, double new_price,
                       double new_qty);
ts_status oms_on_expire(oms_t *o, const char *client_id, const char *reason);
ts_status oms_on_unknown(oms_t *o, const char *client_id, const char *reason);

/* Recovery: install an order snapshot read from the journal. */
ts_status oms_restore_order(oms_t *o, const ts_order *snapshot);

/* Self-trade check: true if submitting req would cross an existing
 * non-terminal order on the opposite side of the same instrument.
 * Market orders cross any opposite-side working order; limit orders
 * cross when buy.price >= resting sell.price (or vice versa). */
bool oms_would_self_trade(const oms_t *o, const ts_order_request *req);

/* Find orders in a given state (for reconciliation reporting). */
size_t oms_orders_in_state(const oms_t *o, ts_order_state state,
                           ts_order *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* TRADING_OMS_H */
