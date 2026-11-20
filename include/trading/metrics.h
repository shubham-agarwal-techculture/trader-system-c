#ifndef TRADING_METRICS_H
#define TRADING_METRICS_H

#include "trading/core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed set of counters covering the pipeline. Indices are stable so they
 * can be logged/journaled without string lookups on the hot path. */
typedef enum {
    TS_MET_MKT_EVENTS = 0,
    TS_MET_MKT_SEQ_GAPS,
    TS_MET_MKT_STALE,
    TS_MET_ORDERS_SUBMITTED,
    TS_MET_ORDERS_DUPLICATE,
    TS_MET_RISK_APPROVED,
    TS_MET_RISK_REJECTED,
    TS_MET_VENUE_ACKS,
    TS_MET_VENUE_REJECTS,
    TS_MET_EXECUTIONS,
    TS_MET_EXEC_DUPLICATE,
    TS_MET_CANCELS,
    TS_MET_RECON_BREAKS,
    TS_MET_COUNT
} ts_metric_id;

typedef struct {
    const char *name;
    uint64_t value;
} ts_metric;

typedef struct {
    ts_metric counters[TS_MET_COUNT];
    /* Simple latency record: count/sum/max plus a log2 histogram for
     * percentile estimation. hist[i] counts durations in
     * [2^(i-1), 2^i) ns for i>=1; hist[0] counts 0. */
    struct {
        const char *name;
        uint64_t count;
        uint64_t sum_ns;
        uint64_t max_ns;
        uint64_t hist[40];
    } latency[8];
} ts_metrics;

void ts_metrics_init(ts_metrics *m);
void ts_metrics_inc(ts_metrics *m, ts_metric_id id, uint64_t n);
uint64_t ts_metrics_get(const ts_metrics *m, ts_metric_id id);
void ts_metrics_latency(ts_metrics *m, int slot, const char *name, ts_ts_ns dur_ns);
/* Estimate the p-th percentile (0-100) in ns; returns 0 if no samples.
 * Value is the upper bound of the containing histogram bucket. */
double ts_metrics_latency_pct(const ts_metrics *m, int slot, double pct);
void ts_metrics_dump(const ts_metrics *m);

#ifdef __cplusplus
}
#endif

#endif /* TRADING_METRICS_H */
