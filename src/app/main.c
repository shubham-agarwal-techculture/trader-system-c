#include "trading/config.h"
#include "trading/core.h"
#include "trading/ems.h"
#include "trading/events.h"
#include "trading/journal.h"
#include "trading/kraken_ws.h"
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
#if defined(_WIN32)
#  include <windows.h>
#else
#  include <time.h>
#endif

/* Pipeline context shared by the strategy order sink. */
typedef struct {
    oms_t *oms;
    rms_t *rms;
    ems_t *ems;
    ts_metrics *metrics;
    ts_vclock *vclock;
} pipeline_t;

static void order_sink(void *ctx, const ts_order_request *req)
{
    pipeline_t *p = ctx;
    ts_ts_ns t0 = p->vclock->now;

    ts_order ord;
    ts_status st = oms_submit(p->oms, req, &ord);
    if (st != TS_OK) {
        ts_log(TS_LOG_WARN, "pipeline: oms_submit failed: %s",
               ts_status_str(st));
        return;
    }

    ts_risk_result risk;
    if (oms_would_self_trade(p->oms, req)) {
        risk.approved = false;
        risk.code = TS_RISK_DUPLICATE;
        risk.rule = "SELF_TRADE";
        snprintf(risk.msg, sizeof(risk.msg),
                 "would cross own working order");
    } else {
        risk = rms_check_order(p->rms, &ord);
    }
    oms_mark_risk(p->oms, req->client_id, risk.approved,
                  risk.rule, risk.msg);
    if (!risk.approved) {
        ts_log(TS_LOG_INFO, "pipeline: risk reject %s: %s (%s)",
               req->client_id, risk.rule, risk.msg);
        return;
    }

    const ts_order *approved = oms_find(p->oms, req->client_id);
    if (!approved) {
        return;
    }
    ts_order dispatch = *approved;
    ems_submit(p->ems, &dispatch);
    ts_metrics_latency(p->metrics, 0, "decision_ns", p->vclock->now - t0);
}

/* Journal replay callback: rebuild OMS order snapshots, RMS positions/PnL,
 * kill-switch state. */
typedef struct {
    oms_t *oms;
    rms_t *rms;
} recover_ctx_t;

static ts_status recover_event(const journal_event *ev, void *vctx)
{
    recover_ctx_t *ctx = vctx;
    switch (ev->type) {
    case JE_SNAP:
        return oms_restore_order(ctx->oms, &ev->order);
    case JE_EXEC:
        return rms_on_execution(ctx->rms, &ev->exec);
    case JE_KILL:
        if (ev->killed) {
            rms_kill(ctx->rms);
        } else {
            rms_resume(ctx->rms);
        }
        return TS_OK;
    case JE_ACCT:
        return rms_on_account(ctx->rms, ev->currency, ev->amount);
    case JE_RISK:
    default:
        return TS_OK;
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s run --journal FILE --replay FILE [--instrument CODE] [--config FILE] [--venue-script FILE]\n"
        "  %s live --journal FILE --config FILE [--instrument CODE] [--duration-s N] [--interval-ms N]\n"
        "  %s recover --journal FILE\n",
        prog, prog, prog);
}

static int cmd_run(const char *journal_path, const char *replay_path,
                   const char *instrument_filter, const char *config_path,
                   const char *venue_script)
{
    ts_metrics metrics;
    ts_metrics_init(&metrics);

    /* Optional config file overrides defaults. */
    ts_config *cfg = NULL;
    if (config_path) {
        cfg = ts_config_load(config_path);
        if (!cfg) {
            ts_log(TS_LOG_ERROR, "cannot load config %s", config_path);
            return 1;
        }
        ts_log(TS_LOG_INFO, "loaded config %s", config_path);
    }

    journal_t *journal = journal_open_append(journal_path);
    if (!journal) {
        ts_log(TS_LOG_ERROR, "cannot open journal %s", journal_path);
        return 1;
    }
    if (cfg) {
        journal_set_fsync(journal,
            ts_config_get_bool(cfg, "journal", "fsync", false));
        journal_set_flush(journal,
            ts_config_get_bool(cfg, "journal", "flush", true));
        journal_set_max_bytes(journal,
            ts_config_get_i64(cfg, "journal", "max_size_mb", 0) * 1048576LL);
    }

    ts_vclock vclock = {0};
    ts_clock clock = ts_clock_virtual(&vclock);

    oms_config ocfg = {.journal = journal, .metrics = &metrics, .clock = clock};
    oms_t *oms = oms_create(&ocfg, 4096);

    rms_config rcfg = {.journal = journal, .metrics = &metrics, .clock = clock};
    rms_t *rms = rms_create(&rcfg);

    ts_risk_limits lim;
    ts_risk_limits_default(&lim);
    if (cfg) {
        ts_config_apply_risk(cfg, &lim);
    }
    rms_set_limits(rms, &lim);

    /* Venue selection: replay script takes precedence, else mock. */
    const char *vtype = cfg ? ts_config_get(cfg, "venue", "type", "mock")
                            : "mock";
    venue_t *venue = NULL;
    if (venue_script || strcmp(vtype, "replay") == 0) {
        const char *script = venue_script
            ? venue_script
            : ts_config_get(cfg, "venue", "script", NULL);
        if (script) {
            venue = venue_replay_create(script, &metrics, clock);
        }
    }
    if (!venue) {
        if (strcmp(vtype, "kraken") == 0) {
            /* Credentials come from environment only — never config
             * files, journal, or logs. */
            const char *kkey = getenv("KRAKEN_API_KEY");
            const char *ksec = getenv("KRAKEN_API_SECRET");
            if (kkey && ksec && kkey[0] && ksec[0]) {
                venue = venue_kraken_create(kkey, ksec, &metrics, clock);
            } else {
                ts_log(TS_LOG_ERROR,
                       "venue=kraken requires KRAKEN_API_KEY and "
                       "KRAKEN_API_SECRET env vars");
                return 1;
            }
        } else if (strcmp(vtype, "dummy") == 0) {
            venue = venue_dummy_create(&metrics, clock);
            if (venue && cfg) {
                venue_dummy_set_latency_ns(
                    venue, ts_config_get_i64(cfg, "venue", "latency_ns", 0));
                venue_dummy_set_fill_fraction(
                    venue, ts_config_get_double(cfg, "venue",
                                                "fill_fraction", 1.0));
                venue_dummy_set_reject_every(
                    venue, (int)ts_config_get_i64(cfg, "venue",
                                                  "reject_every", 0));
                venue_dummy_set_max_orders_per_sec(
                    venue, ts_config_get_double(cfg, "venue",
                                                "max_orders_per_sec", 0.0));
            }
        } else {
            venue = venue_mock_create(&metrics, clock);
            if (cfg && venue) {
                bool reject = ts_config_get_bool(cfg, "venue", "reject_all",
                                                 false);
                venue_mock_set_reject_all(venue, reject);
            }
        }
    }
    if (!venue) {
        ts_log(TS_LOG_ERROR, "failed to create venue");
        return 1;
    }
    /* Live-enablement gate: only simulated venues (mock/replay) run by
     * default. Any real adapter must be explicitly enabled via
     * [live] enabled = true in config. */
    bool live_enabled = cfg &&
        ts_config_get_bool(cfg, "live", "enabled", false);
    const char *vn = venue_name(venue);
    if (strcmp(vn, "mock") != 0 && strcmp(vn, "replay") != 0 &&
        strcmp(vn, "dummy") != 0 && !live_enabled) {
        ts_log(TS_LOG_ERROR,
               "venue '%s' is not a simulated venue; set [live] enabled = "
               "true to proceed", vn);
        return 1;
    }
    ts_log(TS_LOG_INFO, "venue: %s (live=%s)", vn,
           live_enabled ? "enabled" : "simulated");
    ems_config ecfg = {
        .venue = venue, .oms = oms, .rms = rms,
        .metrics = &metrics, .journal = journal, .clock = clock,
    };
    ems_t *ems = ems_create(&ecfg);

    replay_t *replay = replay_open(replay_path);
    if (!replay) {
        ts_log(TS_LOG_ERROR, "cannot open replay %s", replay_path);
        return 1;
    }

    pipeline_t pipe = {.oms = oms, .rms = rms, .ems = ems,
                       .metrics = &metrics, .vclock = &vclock};

    ts_seqwatch *seqwatch = ts_seqwatch_create(&metrics);

    momentum_config mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    ts_copy_str(mcfg.instrument, sizeof(mcfg.instrument),
                instrument_filter ? instrument_filter : "BTCUSDT");
    mcfg.qty = 0.5;
    mcfg.move_threshold = 0.5;
    if (cfg) {
        ts_copy_str(mcfg.instrument, sizeof(mcfg.instrument),
                    ts_config_get(cfg, "strategy", "instrument",
                                  mcfg.instrument));
        mcfg.qty = ts_config_get_double(cfg, "strategy", "qty", 0.5);
        mcfg.move_threshold =
            ts_config_get_double(cfg, "strategy", "move_threshold", 0.5);
        const char *sname = ts_config_get(cfg, "strategy", "name", "momentum");
        ts_copy_str(mcfg.strategy_name, sizeof(mcfg.strategy_name), sname);
    }
    mcfg.sink = order_sink;
    mcfg.sink_ctx = &pipe;
    const char *sname = cfg
        ? ts_config_get(cfg, "strategy", "name", "momentum")
        : "momentum";
    strategy_t *strat = NULL;
    if (strcmp(sname, "book_imbalance") == 0) {
        bookimb_config bcfg;
        memset(&bcfg, 0, sizeof(bcfg));
        ts_copy_str(bcfg.instrument, sizeof(bcfg.instrument), mcfg.instrument);
        ts_copy_str(bcfg.strategy_name, sizeof(bcfg.strategy_name),
                    mcfg.strategy_name[0] ? mcfg.strategy_name : "book_imbalance");
        bcfg.qty = mcfg.qty;
        bcfg.imbalance_threshold =
            ts_config_get_double(cfg, "strategy", "imbalance_threshold", 0.3);
        bcfg.sink = order_sink;
        bcfg.sink_ctx = &pipe;
        strat = strategy_book_imbalance_create(&bcfg);
    } else {
        strat = strategy_momentum_create(&mcfg);
    }

    ts_mkt_event ev;
    size_t events = 0, orders_before = oms_count(oms);
    while (replay_next(replay, &ev) == TS_OK) {
        ts_vclock_set(&vclock, ev.ts_ns);
        ts_metrics_inc(&metrics, TS_MET_MKT_EVENTS, 1);
        ts_seqwatch_on_event(seqwatch, &ev);

        rms_on_quote(rms, ev.instrument, ev.bid, ev.ask, ev.last, ev.ts_ns);
        ems_on_market_event(ems, &ev);
        if (strat && strat->on_event) {
            strat->on_event(strat, &ev);
        }
        ems_poll(ems);
        events++;
    }

    /* Final reconciliation against venue open-order state. */
    ts_recon_report rep;
    if (ems_reconcile(ems, &rep) == TS_OK) {
        printf("reconcile: oms_open=%zu venue_open=%zu matched=%zu "
               "oms_only=%zu venue_only=%zu\n",
               rep.oms_open, rep.venue_open, rep.matched,
               rep.oms_only, rep.venue_only);
    }

    printf("replay complete: %zu market events, %zu orders in OMS\n",
           events, oms_count(oms) - orders_before);
    printf("open orders: %zu\n", oms_open_count(oms));
    printf("positions: %zu, realized pnl: %.2f, unrealized: %.2f\n",
           rms_position_count(rms), rms_realized_pnl(rms),
           rms_unrealized_pnl(rms));
    ts_metrics_dump(&metrics);

    if (strat) strat->destroy(strat);
    ts_seqwatch_destroy(seqwatch);
    replay_close(replay);
    ems_destroy(ems);
    venue->ops->destroy(venue);
    rms_destroy(rms);
    oms_destroy(oms);
    journal_close(journal);
    if (cfg) ts_config_free(cfg);
    return 0;
}

static void sleep_ms(int ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts = {.tv_sec = ms / 1000,
                          .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
#endif
}

/* Live mode: pull market data from the configured venue (kraken) and
 * run the same OMS/RMS/EMS pipeline as replay. Real orders reach the
 * exchange — [live] enabled + credentials are mandatory. */
static int cmd_live(const char *journal_path, const char *instrument,
                    const char *config_path, int duration_s, int interval_ms)
{
    ts_metrics metrics;
    ts_metrics_init(&metrics);

    ts_config *cfg = NULL;
    if (config_path) {
        cfg = ts_config_load(config_path);
        if (!cfg) {
            ts_log(TS_LOG_ERROR, "cannot load config %s", config_path);
            return 1;
        }
        ts_log(TS_LOG_INFO, "loaded config %s", config_path);
    }

    /* Live mode requires the live gate explicitly. */
    if (!cfg || !ts_config_get_bool(cfg, "live", "enabled", false)) {
        ts_log(TS_LOG_ERROR,
               "live mode requires [live] enabled = true in config");
        return 1;
    }

    journal_t *journal = journal_open_append(journal_path);
    if (!journal) {
        ts_log(TS_LOG_ERROR, "cannot open journal %s", journal_path);
        return 1;
    }
    if (cfg) {
        journal_set_fsync(journal,
            ts_config_get_bool(cfg, "journal", "fsync", false));
        journal_set_flush(journal,
            ts_config_get_bool(cfg, "journal", "flush", true));
        journal_set_max_bytes(journal,
            ts_config_get_i64(cfg, "journal", "max_size_mb", 0) * 1048576LL);
    }

    ts_clock clock = ts_clock_mono();
    ts_vclock vclock = {clock.now_ns(clock.ctx)};

    oms_config ocfg = {.journal = journal, .metrics = &metrics, .clock = clock};
    oms_t *oms = oms_create(&ocfg, 4096);
    rms_config rcfg = {.journal = journal, .metrics = &metrics, .clock = clock};
    rms_t *rms = rms_create(&rcfg);
    ts_risk_limits lim;
    ts_risk_limits_default(&lim);
    if (cfg) ts_config_apply_risk(cfg, &lim);
    rms_set_limits(rms, &lim);

    /* Live mode currently supports the kraken venue only. */
    venue_t *venue = NULL;
    const char *kkey = getenv("KRAKEN_API_KEY");
    const char *ksec = getenv("KRAKEN_API_SECRET");
    if (!kkey || !ksec || !kkey[0] || !ksec[0]) {
        ts_log(TS_LOG_ERROR,
               "live mode requires KRAKEN_API_KEY and KRAKEN_API_SECRET");
        return 1;
    }
    venue = venue_kraken_create(kkey, ksec, &metrics, clock);
    if (!venue) {
        ts_log(TS_LOG_ERROR, "failed to create kraken venue");
        return 1;
    }
    /* Prefer the WebSocket ticker feed; REST ticker is the fallback. */
    const char *live_instr = instrument;
    if (cfg) {
        live_instr = ts_config_get(cfg, "strategy", "instrument",
                                   live_instr);
    }
    ts_status ws_st = venue_kraken_ws_start(venue, live_instr);
    bool use_ws = (ws_st == TS_OK);
    ts_log(TS_LOG_INFO, "live mode: venue=kraken instrument=%s "
           "feed=%s interval=%dms duration=%ds", live_instr,
           use_ws ? "websocket" : "rest-poll", interval_ms, duration_s);

    ems_config ecfg = {.venue = venue, .oms = oms, .rms = rms,
                       .metrics = &metrics, .clock = clock};
    ems_t *ems = ems_create(&ecfg);
    ts_seqwatch *seqwatch = ts_seqwatch_create(&metrics);

    pipeline_t pipe = {.oms = oms, .rms = rms, .ems = ems,
                       .metrics = &metrics, .vclock = &vclock};
    momentum_config mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    ts_copy_str(mcfg.instrument, sizeof(mcfg.instrument), instrument);
    mcfg.qty = 0.5;
    mcfg.move_threshold = 0.5;
    if (cfg) {
        ts_copy_str(mcfg.instrument, sizeof(mcfg.instrument),
                    ts_config_get(cfg, "strategy", "instrument",
                                  mcfg.instrument));
        mcfg.qty = ts_config_get_double(cfg, "strategy", "qty", 0.5);
        mcfg.move_threshold =
            ts_config_get_double(cfg, "strategy", "move_threshold", 0.5);
    }
    mcfg.sink = order_sink;
    mcfg.sink_ctx = &pipe;
    strategy_t *strat = strategy_momentum_create(&mcfg);

    ts_mkt_event ev;
    size_t events = 0;
    int elapsed = 0;
    while (duration_s <= 0 || elapsed < duration_s * 1000) {
        ts_status st;
        if (use_ws && venue_kraken_ws_running(venue)) {
            st = venue_kraken_next_market_event(venue, &ev);
            if (st != TS_OK) {
                st = TS_ERR_NOT_FOUND; /* queue empty: idle this tick */
            }
        } else {
            st = venue_kraken_get_ticker(venue,
                mcfg.instrument[0] ? mcfg.instrument : live_instr,
                &ev, clock);
        }
        if (st == TS_OK) {
            events++;
            vclock.now = ev.ts_ns;
            ts_metrics_inc(&metrics, TS_MET_MKT_EVENTS, 1);
            ts_seqwatch_on_event(seqwatch, &ev);
            rms_on_quote(rms, ev.instrument, ev.bid, ev.ask, ev.last,
                         ev.ts_ns);
            ems_on_market_event(ems, &ev);
            if (strat && strat->on_event) {
                strat->on_event(strat, &ev);
            }
        } else {
            ts_metrics_inc(&metrics, TS_MET_MKT_STALE, 1);
        }
        ems_poll(ems);
        /* WS feed updates faster than REST cadence — poll the queue
         * more frequently when it's the active source. */
        int sleep_ms_now = use_ws ? (interval_ms < 50 ? interval_ms : 50)
                                  : interval_ms;
        sleep_ms(sleep_ms_now);
        elapsed += sleep_ms_now;
    }

    ts_recon_report rep;
    if (ems_reconcile(ems, &rep) == TS_OK) {
        printf("reconcile: oms_open=%zu venue_open=%zu matched=%zu "
               "oms_only=%zu venue_only=%zu\n",
               rep.oms_open, rep.venue_open, rep.matched,
               rep.oms_only, rep.venue_only);
    }
    printf("live session: %zu market events\n", events);
    ts_metrics_dump(&metrics);

    if (strat) strat->destroy(strat);
    ts_seqwatch_destroy(seqwatch);
    ems_destroy(ems);
    venue->ops->destroy(venue);
    rms_destroy(rms);
    oms_destroy(oms);
    journal_close(journal);
    if (cfg) ts_config_free(cfg);
    return 0;
}

static int cmd_recover(const char *journal_path)
{
    ts_metrics metrics;
    ts_metrics_init(&metrics);
    ts_vclock vclock = {0};
    ts_clock clock = ts_clock_virtual(&vclock);

    oms_config ocfg = {.journal = NULL, .metrics = &metrics, .clock = clock};
    oms_t *oms = oms_create(&ocfg, 8192);
    rms_config rcfg = {.journal = NULL, .metrics = &metrics, .clock = clock};
    rms_t *rms = rms_create(&rcfg);

    recover_ctx_t ctx = {.oms = oms, .rms = rms};
    size_t skipped = 0;
    ts_status st = journal_replay(journal_path, recover_event, &ctx, &skipped);
    if (st != TS_OK) {
        ts_log(TS_LOG_ERROR, "journal replay failed: %s",
               ts_status_str(st));
        return 1;
    }

    printf("recovered from %s (skipped %zu malformed lines)\n",
           journal_path, skipped);
    printf("orders: %zu total, %zu open\n", oms_count(oms),
           oms_open_count(oms));
    printf("positions: %zu, realized pnl: %.2f\n",
           rms_position_count(rms), rms_realized_pnl(rms));
    for (size_t i = 0; i < rms_position_count(rms); ++i) {
        /* positions are internal; print count only for now */
        (void)i;
    }
    ts_metrics_dump(&metrics);

    rms_destroy(rms);
    oms_destroy(oms);
    return 0;
}

int main(int argc, char **argv)
{
    ts_log_set_level(TS_LOG_INFO);
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    const char *cmd = argv[1];
    const char *journal_path = NULL;
    const char *replay_path = NULL;
    const char *instrument = NULL;
    const char *config_path = NULL;
    const char *venue_script = NULL;
    int duration_s = 0;
    int interval_ms = 1000;

    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--journal") == 0 && i + 1 < argc) {
            journal_path = argv[++i];
        } else if (strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            replay_path = argv[++i];
        } else if (strcmp(argv[i], "--instrument") == 0 && i + 1 < argc) {
            instrument = argv[++i];
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--venue-script") == 0 && i + 1 < argc) {
            venue_script = argv[++i];
        } else if (strcmp(argv[i], "--duration-s") == 0 && i + 1 < argc) {
            duration_s = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--interval-ms") == 0 && i + 1 < argc) {
            interval_ms = atoi(argv[++i]);
        }
    }

    if (!journal_path) {
        journal_path = "trading_journal.log";
    }

    if (strcmp(cmd, "run") == 0) {
        if (!replay_path) {
            usage(argv[0]);
            return 2;
        }
        return cmd_run(journal_path, replay_path, instrument, config_path,
                       venue_script);
    }
    if (strcmp(cmd, "live") == 0) {
        if (!config_path) {
            usage(argv[0]);
            return 2;
        }
        return cmd_live(journal_path,
                        instrument ? instrument : "BTCUSDT",
                        config_path, duration_s, interval_ms);
    }
    if (strcmp(cmd, "recover") == 0) {
        return cmd_recover(journal_path);
    }

    usage(argv[0]);
    return 2;
}
