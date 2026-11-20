#include "trading/config.h"
#include "trading/core.h"
#include "trading/ems.h"
#include "trading/events.h"
#include "trading/journal.h"
#include "trading/log.h"
#include "trading/metrics.h"
#include "trading/oms.h"
#include "trading/rms.h"
#include "trading/seqwatch.h"
#include "trading/strategy.h"
#include "trading/venue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* trading_bench: deterministic throughput + latency benchmark.
 * Drives N synthetic market events through the full pipeline
 * (seqwatch -> RMS marks -> venue -> strategy -> OMS/RMS/EMS -> poll)
 * against the dummy venue and reports events/sec plus the latency
 * percentile metrics already collected by the pipeline.
 *
 * Usage: trading_bench [--events N] [--config FILE]
 * Default N = 100000. */

typedef struct {
    oms_t *oms;
    rms_t *rms;
    ems_t *ems;
    ts_metrics *metrics;
    ts_vclock *vclock;
} bench_pipe;

static void bench_sink(void *ctx, const ts_order_request *req)
{
    bench_pipe *p = ctx;
    ts_ts_ns t0 = p->vclock->now;
    ts_order ord;
    if (oms_submit(p->oms, req, &ord) != TS_OK) return;
    if (oms_would_self_trade(p->oms, req)) {
        oms_mark_risk(p->oms, req->client_id, false, "SELF_TRADE", "");
        return;
    }
    ts_risk_result risk = rms_check_order(p->rms, &ord);
    oms_mark_risk(p->oms, req->client_id, risk.approved, risk.rule,
                  risk.msg);
    if (!risk.approved) return;
    const ts_order *approved = oms_find(p->oms, req->client_id);
    if (!approved) return;
    ts_order dispatch = *approved;
    ems_submit(p->ems, &dispatch);
    ts_metrics_latency(p->metrics, 0, "decision_ns", p->vclock->now - t0);
}

int main(int argc, char **argv)
{
    ts_log_set_level(TS_LOG_WARN);
    long n_events = 100000;
    const char *config_path = NULL;
    bool no_journal = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--events") == 0 && i + 1 < argc) {
            n_events = atol(argv[++i]);
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--nojournal") == 0) {
            no_journal = true;
        }
    }
    if (n_events <= 0) n_events = 100000;

    ts_metrics metrics;
    ts_metrics_init(&metrics);

    ts_config *cfg = NULL;
    if (config_path) {
        cfg = ts_config_load(config_path);
        if (!cfg) {
            fprintf(stderr, "cannot load config %s\n", config_path);
            return 1;
        }
    }

    /* Virtual clock advanced synthetically; wall time measured
     * separately for throughput. */
    ts_vclock vclock = {1000000000LL};
    ts_clock clock = ts_clock_virtual(&vclock);

    /* journal to a temp file so the full durable path is exercised;
     * --nojournal isolates compute cost from I/O. */
    char jpath[256];
    snprintf(jpath, sizeof(jpath), "bench_journal_%lld.log",
             (long long)ts_clock_mono_now());
    journal_t *journal = NULL;
    if (!no_journal) {
        journal = journal_open_append(jpath);
        if (!journal) {
            fprintf(stderr, "cannot open journal\n");
            return 1;
        }
        journal_set_flush(journal, false); /* bench: durability off */
    }

    oms_config ocfg = {.journal = journal, .metrics = &metrics,
                       .clock = clock};
    oms_t *oms = oms_create(&ocfg, 65536);
    rms_config rcfg = {.journal = journal, .metrics = &metrics,
                       .clock = clock};
    (void)0;
    rms_t *rms = rms_create(&rcfg);
    ts_risk_limits lim;
    ts_risk_limits_default(&lim);
    lim.max_order_qty = 1e9;
    lim.max_order_notional = 1e18;
    lim.max_position_qty = 1e12;
    lim.daily_loss_limit = 1e18;
    lim.max_order_rate = 0;
    lim.require_fresh_market = false;
    if (cfg) ts_config_apply_risk(cfg, &lim);
    rms_set_limits(rms, &lim);

    venue_t *venue = venue_dummy_create(&metrics, clock);
    venue_dummy_set_latency_ns(venue, 0);
    venue_dummy_set_fill_fraction(venue, 1.0);

    ems_config ecfg = {.venue = venue, .oms = oms, .rms = rms,
                       .metrics = &metrics, .clock = clock};
    ems_t *ems = ems_create(&ecfg);
    ts_seqwatch *seqwatch = ts_seqwatch_create(&metrics);

    bench_pipe pipe = {.oms = oms, .rms = rms, .ems = ems,
                       .metrics = &metrics, .vclock = &vclock};
    momentum_config mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    ts_copy_str(mcfg.instrument, sizeof(mcfg.instrument), "BTCUSDT");
    mcfg.qty = 0.001;
    mcfg.move_threshold = 0.0001; /* sensitive: fires often */
    mcfg.sink = bench_sink;
    mcfg.sink_ctx = &pipe;
    strategy_t *strat = strategy_momentum_create(&mcfg);

    /* Wall clock for throughput */
    ts_clock mono = ts_clock_mono();
    ts_ts_ns t_start = mono.now_ns(mono.ctx);

    /* Synthetic feed: alternating quote moves to trigger momentum
     * orders ~every few events, plus periodic trades. Deterministic
     * pseudo-random via LCG. */
    uint64_t rng = 0x9e3779b97f4a7c15ULL;
    ts_mkt_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = TS_MKT_QUOTE;
    ts_copy_str(ev.instrument, sizeof(ev.instrument), "BTCUSDT");
    double mid = 100.0;

    for (long i = 0; i < n_events; ++i) {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        double shock = ((double)(rng >> 33) / 2147483648.0 - 0.5) * 0.02;
        /* trend bias so momentum fires periodically */
        if (i % 7 == 0) shock += 0.01;
        if (i % 11 == 0) shock -= 0.008;
        mid *= (1.0 + shock);
        double spread = mid * 0.0001;
        ev.bid = mid - spread;
        ev.ask = mid + spread;
        ev.last = mid;
        ev.bid_qty = 10.0;
        ev.ask_qty = 10.0;
        ev.seq = (uint64_t)i + 1;
        vclock.now += 1000000; /* 1ms per event */
        ev.ts_ns = vclock.now;

        ts_metrics_inc(&metrics, TS_MET_MKT_EVENTS, 1);
        ts_seqwatch_on_event(seqwatch, &ev);
        rms_on_quote(rms, ev.instrument, ev.bid, ev.ask, ev.last, ev.ts_ns);
        ems_on_market_event(ems, &ev);
        if (strat && strat->on_event) {
            strat->on_event(strat, &ev);
        }
        ems_poll(ems);
    }

    ts_ts_ns t_end = mono.now_ns(mono.ctx);
    double elapsed_s = (double)(t_end - t_start) / 1e9;

    printf("=== trading_bench ===\n");
    printf("events:      %ld\n", n_events);
    printf("orders:      %zu\n", oms_count(oms));
    printf("execs:       %llu\n",
           (unsigned long long)ts_metrics_get(&metrics, TS_MET_EXECUTIONS));
    printf("wall_time:   %.3fs\n", elapsed_s);
    printf("throughput:  %.0f events/sec\n",
           elapsed_s > 0 ? (double)n_events / elapsed_s : 0.0);
    printf("\n--- latency (virtual-clock deltas, ns) ---\n");
    ts_metrics_dump(&metrics);

    if (strat) strat->destroy(strat);
    ts_seqwatch_destroy(seqwatch);
    ems_destroy(ems);
    venue->ops->destroy(venue);
    rms_destroy(rms);
    oms_destroy(oms);
    journal_close(journal);
    remove(jpath);
    if (cfg) ts_config_free(cfg);
    return 0;
}
