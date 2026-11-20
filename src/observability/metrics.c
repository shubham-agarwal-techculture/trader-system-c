#include "trading/metrics.h"

#include <stdio.h>
#include <string.h>

static const char *counter_names[TS_MET_COUNT] = {
    "mkt_events",
    "mkt_seq_gaps",
    "mkt_stale",
    "orders_submitted",
    "orders_duplicate",
    "risk_approved",
    "risk_rejected",
    "venue_acks",
    "venue_rejects",
    "executions",
    "exec_duplicate",
    "cancels",
    "recon_breaks",
};

void ts_metrics_init(ts_metrics *m)
{
    memset(m, 0, sizeof(*m));
    for (int i = 0; i < TS_MET_COUNT; ++i) {
        m->counters[i].name = counter_names[i];
    }
    m->latency[0].name = "decision_ns";
    m->latency[1].name = "risk_ns";
    m->latency[2].name = "dispatch_ns";
    m->latency[3].name = "ack_ns";
    m->latency[4].name = "fill_ns";
}

void ts_metrics_inc(ts_metrics *m, ts_metric_id id, uint64_t n)
{
    if (m && id >= 0 && id < TS_MET_COUNT) {
        m->counters[id].value += n;
    }
}

uint64_t ts_metrics_get(const ts_metrics *m, ts_metric_id id)
{
    if (!m || id < 0 || id >= TS_MET_COUNT) {
        return 0;
    }
    return m->counters[id].value;
}

void ts_metrics_latency(ts_metrics *m, int slot, const char *name, ts_ts_ns dur_ns)
{
    if (!m || slot < 0 || slot >= 8 || dur_ns < 0) {
        return;
    }
    if (m->latency[slot].name == NULL) {
        m->latency[slot].name = name;
    }
    m->latency[slot].count++;
    m->latency[slot].sum_ns += (uint64_t)dur_ns;
    if ((uint64_t)dur_ns > m->latency[slot].max_ns) {
        m->latency[slot].max_ns = (uint64_t)dur_ns;
    }
    /* bucket: 0 -> hist[0]; ns in [2^(i-1), 2^i) -> hist[i] */
    int idx = 0;
    uint64_t v = (uint64_t)dur_ns;
    while (v >>= 1) {
        idx++;
    }
    /* v=1ns -> idx0? dur_ns=1 -> while(1>>=1)=0 -> idx0 -> hist[0] range [1,2) ok-ish.
       dur_ns=0 -> idx0. Acceptable granularity for estimates. */
    if (idx >= 40) {
        idx = 39;
    }
    m->latency[slot].hist[idx]++;
}

double ts_metrics_latency_pct(const ts_metrics *m, int slot, double pct)
{
    if (!m || slot < 0 || slot >= 8 || !m->latency[slot].name ||
        m->latency[slot].count == 0 || pct < 0.0 || pct > 100.0) {
        return 0.0;
    }
    uint64_t target =
        (uint64_t)((m->latency[slot].count * pct) / 100.0 + 0.5);
    if (target == 0) {
        target = 1;
    }
    uint64_t cum = 0;
    for (int i = 0; i < 40; ++i) {
        cum += m->latency[slot].hist[i];
        if (cum >= target) {
            /* upper bound of bucket i is 2^i ns (i=0 -> ~1ns) */
            if (i == 0) {
                return 1.0;
            }
            return (double)(1ULL << i);
        }
    }
    return (double)m->latency[slot].max_ns;
}

void ts_metrics_dump(const ts_metrics *m)
{
    printf("--- metrics ---\n");
    for (int i = 0; i < TS_MET_COUNT; ++i) {
        printf("  %-20s %llu\n", m->counters[i].name,
               (unsigned long long)m->counters[i].value);
    }
    for (int i = 0; i < 8; ++i) {
        if (m->latency[i].name && m->latency[i].count) {
            printf("  %-20s n=%llu avg=%.0fns p50=%.0f p95=%.0f p99=%.0f max=%lluns\n",
                   m->latency[i].name,
                   (unsigned long long)m->latency[i].count,
                   (double)m->latency[i].sum_ns / (double)m->latency[i].count,
                   ts_metrics_latency_pct(m, i, 50.0),
                   ts_metrics_latency_pct(m, i, 95.0),
                   ts_metrics_latency_pct(m, i, 99.0),
                   (unsigned long long)m->latency[i].max_ns);
        }
    }
}
