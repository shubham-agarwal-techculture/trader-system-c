#include "trading/core.h"
#include "trading/ems.h"
#include "trading/events.h"
#include "trading/journal.h"
#include "trading/log.h"
#include "trading/metrics.h"
#include "trading/oms.h"
#include "trading/rms.h"
#include "trading/strategy.h"
#include "trading/venue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    ts_risk_result risk = rms_check_order(p->rms, &ord);
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
        "  %s run --journal FILE --replay FILE [--instrument CODE]\n"
        "  %s recover --journal FILE\n"
        "  %s metrics-demo --journal FILE --replay FILE\n",
        prog, prog, prog);
}

static int cmd_run(const char *journal_path, const char *replay_path,
                   const char *instrument_filter)
{
    ts_metrics metrics;
    ts_metrics_init(&metrics);

    journal_t *journal = journal_open_append(journal_path);
    if (!journal) {
        ts_log(TS_LOG_ERROR, "cannot open journal %s", journal_path);
        return 1;
    }

    ts_vclock vclock = {0};
    ts_clock clock = ts_clock_virtual(&vclock);

    oms_config ocfg = {.journal = journal, .metrics = &metrics, .clock = clock};
    oms_t *oms = oms_create(&ocfg, 4096);

    rms_config rcfg = {.journal = journal, .metrics = &metrics, .clock = clock};
    rms_t *rms = rms_create(&rcfg);

    venue_t *venue = venue_mock_create(&metrics, clock);
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

    momentum_config mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    ts_copy_str(mcfg.instrument, sizeof(mcfg.instrument),
                instrument_filter ? instrument_filter : "BTCUSDT");
    mcfg.qty = 0.5;
    mcfg.move_threshold = 0.5;
    mcfg.sink = order_sink;
    mcfg.sink_ctx = &pipe;
    strategy_t *strat = strategy_momentum_create(&mcfg);

    ts_mkt_event ev;
    size_t events = 0, orders_before = oms_count(oms);
    while (replay_next(replay, &ev) == TS_OK) {
        ts_vclock_set(&vclock, ev.ts_ns);
        ts_metrics_inc(&metrics, TS_MET_MKT_EVENTS, 1);

        rms_on_quote(rms, ev.instrument, ev.bid, ev.ask, ev.last, ev.ts_ns);
        ems_on_market_event(ems, &ev);
        if (strat && strat->on_event) {
            strat->on_event(strat, &ev);
        }
        ems_poll(ems);
        events++;
    }

    printf("replay complete: %zu market events, %zu orders in OMS\n",
           events, oms_count(oms) - orders_before);
    printf("open orders: %zu\n", oms_open_count(oms));
    printf("positions: %zu, realized pnl: %.2f, unrealized: %.2f\n",
           rms_position_count(rms), rms_realized_pnl(rms),
           rms_unrealized_pnl(rms));
    ts_metrics_dump(&metrics);

    if (strat) strat->destroy(strat);
    replay_close(replay);
    ems_destroy(ems);
    venue->ops->destroy(venue);
    rms_destroy(rms);
    oms_destroy(oms);
    journal_close(journal);
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

    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--journal") == 0 && i + 1 < argc) {
            journal_path = argv[++i];
        } else if (strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            replay_path = argv[++i];
        } else if (strcmp(argv[i], "--instrument") == 0 && i + 1 < argc) {
            instrument = argv[++i];
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
        return cmd_run(journal_path, replay_path, instrument);
    }
    if (strcmp(cmd, "recover") == 0) {
        return cmd_recover(journal_path);
    }

    usage(argv[0]);
    return 2;
}
