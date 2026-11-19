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
            printf("  %-20s n=%llu avg=%.0fns max=%lluns\n",
                   m->latency[i].name,
                   (unsigned long long)m->latency[i].count,
                   (double)m->latency[i].sum_ns / (double)m->latency[i].count,
                   (unsigned long long)m->latency[i].max_ns);
        }
    }
}
