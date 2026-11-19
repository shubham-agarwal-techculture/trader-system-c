#ifndef TRADING_EMS_H
#define TRADING_EMS_H

#include "trading/oms.h"
#include "trading/rms.h"
#include "trading/venue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Execution Management System. Receives risk-approved orders, dispatches
 * them to a venue adapter, and routes venue events back into the OMS/RMS.
 * This is the only component allowed to talk to venues. */

typedef struct ems ems_t;

typedef struct {
    ems_t *ems;
    venue_t *venue;      /* not owned; must outlive ems */
    oms_t *oms;
    rms_t *rms;
    ts_metrics *metrics; /* may be NULL */
    journal_t *journal;  /* may be NULL; used for dispatch audit */
    ts_clock clock;
} ems_config;

ems_t *ems_create(const ems_config *cfg);
void ems_destroy(ems_t *e);

/* Full order path: OMS-approved order -> EMS -> venue.
 * Returns after the venue has accepted the request for processing;
 * outcomes arrive via ems_poll(). */
ts_status ems_submit(ems_t *e, const ts_order *ord);
ts_status ems_cancel(ems_t *e, const char *client_id);

/* Forward a market event to the venue (needed for paper matching and for
 * venues that consume their own feed). */
void ems_on_market_event(ems_t *e, const ts_mkt_event *ev);

/* Drain and dispatch pending venue events into OMS/RMS. Call in the main
 * loop after market data and order flow. */
ts_status ems_poll(ems_t *e);

bool ems_venue_connected(ems_t *e);

/* Reconciliation support: mark orders whose venue state is uncertain. */
ts_status ems_mark_unknown(ems_t *e, const char *client_id, const char *reason);

#ifdef __cplusplus
}
#endif

#endif /* TRADING_EMS_H */
