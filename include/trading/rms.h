#ifndef TRADING_RMS_H
#define TRADING_RMS_H

#include "trading/events.h"
#include "trading/journal.h"
#include "trading/metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Risk Management System. Synchronous pre-trade gate plus post-trade
 * position/P&L tracking. All decisions are journaled for audit. */

#define TS_RMS_MAX_POSITIONS 256
#define TS_RMS_RATE_WINDOW 1024

typedef struct {
    double max_order_qty;        /* max quantity per order */
    double max_order_notional;   /* max qty*price per order */
    double max_position_qty;     /* max absolute position per instrument */
    double daily_loss_limit;     /* max allowed loss (positive magnitude) */
    double max_order_rate;       /* max order submissions per second, 0 = off */
    bool require_fresh_market;   /* reject if quote older than max_quote_age_ns */
    int64_t max_quote_age_ns;
} ts_risk_limits;

/* Conservative defaults suitable for first live enablement. */
void ts_risk_limits_default(ts_risk_limits *lim);

typedef enum {
    TS_RISK_OK = 0,
    TS_RISK_KILL_SWITCH,
    TS_RISK_INVALID_ORDER,
    TS_RISK_RATE_LIMIT,
    TS_RISK_QTY_LIMIT,
    TS_RISK_NOTIONAL_LIMIT,
    TS_RISK_POSITION_LIMIT,
    TS_RISK_LOSS_LIMIT,
    TS_RISK_STALE_DATA,
    TS_RISK_NO_MARKET_DATA,
    TS_RISK_DUPLICATE,
} ts_risk_code;

const char *ts_risk_code_str(ts_risk_code c);

typedef struct {
    bool approved;
    ts_risk_code code;
    const char *rule;   /* stable rule name for journals/alerts */
    char msg[TS_MAX_REASON];
} ts_risk_result;

typedef struct {
    char instrument[TS_MAX_CODE];
    double qty;          /* signed: + long, - short */
    double avg_cost;
    double last_price;
    ts_ts_ns last_quote_ns;
} ts_rms_position;

typedef struct rms rms_t;

typedef struct {
    rms_t *rms;
    journal_t *journal;   /* may be NULL */
    ts_metrics *metrics;  /* may be NULL */
    ts_clock clock;
} rms_config;

rms_t *rms_create(const rms_config *cfg);
void rms_destroy(rms_t *r);

void rms_set_limits(rms_t *r, const ts_risk_limits *lim);
const ts_risk_limits *rms_get_limits(const rms_t *r);

void rms_kill(rms_t *r);    /* engage kill switch (journaled) */
rms_t *rms_resume(rms_t *r);/* disengage; returns r for chaining */
bool rms_is_killed(const rms_t *r);

/* Feed market data freshness/marks. */
void rms_on_quote(rms_t *r, const char *instrument, double bid, double ask,
                  double last, ts_ts_ns quote_ns);

/* Pre-trade check. Synchronous; must complete before EMS dispatch. */
ts_risk_result rms_check_order(rms_t *r, const ts_order *ord);

/* Post-trade: apply an execution to positions and P&L. */
ts_status rms_on_execution(rms_t *r, const ts_execution *exec);

/* Account cash update (for future buying-power checks). */
ts_status rms_on_account(rms_t *r, const char *currency, double amount);

/* Inspection. */
const ts_rms_position *rms_position(rms_t *r, const char *instrument);
double rms_realized_pnl(const rms_t *r);
double rms_unrealized_pnl(rms_t *r);
double rms_total_pnl(rms_t *r);
size_t rms_position_count(const rms_t *r);

/* Recovery hooks used by the journal replay path. */
ts_status rms_restore_position(rms_t *r, const char *instrument, double qty,
                               double avg_cost);
ts_status rms_restore_pnl(rms_t *r, double realized_pnl);

#ifdef __cplusplus
}
#endif

#endif /* TRADING_RMS_H */
