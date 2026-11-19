#include "testutil.h"

#include "trading/ems.h"
#include "trading/journal.h"
#include "trading/metrics.h"
#include "trading/oms.h"
#include "trading/rms.h"
#include "trading/strategy.h"
#include "trading/venue.h"

#include <stdio.h>
#include <string.h>

ts_status pipe_rec_cb(const journal_event *ev, void *ctx);

typedef struct {
    oms_t *oms;
    rms_t *rms;
    ems_t *ems;
    ts_metrics *metrics;
    ts_vclock *vclock;
    journal_t *journal;
} pipe_t;

static void sink(void *ctx, const ts_order_request *req)
{
    pipe_t *p = ctx;
    ts_order ord;
    if (oms_submit(p->oms, req, &ord) != TS_OK) {
        return;
    }
    ts_risk_result risk = rms_check_order(p->rms, &ord);
    oms_mark_risk(p->oms, req->client_id, risk.approved, risk.rule, risk.msg);
    if (!risk.approved) {
        return;
    }
    const ts_order *appr = oms_find(p->oms, req->client_id);
    if (appr) {
        ts_order d = *appr;
        ems_submit(p->ems, &d);
    }
}

static char g_replay_path[512];
static char g_journal_path[512];

static void write_replay_file(void)
{
    snprintf(g_replay_path, sizeof(g_replay_path),
             "test_replay_%lld.txt", (long long)ts_clock_mono_now());
    FILE *f = fopen(g_replay_path, "w");
    ASSERT(f != NULL);
    /* Alternating quotes that move mid by > threshold to trigger trades. */
    double mid = 100.0;
    for (int i = 0; i < 20; ++i) {
        mid += (i % 2 == 0) ? 1.0 : -1.0;
        fprintf(f, "QUOTE BTCUSDT %.2f %.2f %.2f %lld %d\n",
                mid - 0.5, mid + 0.5, mid,
                1000000LL + i * 1000000LL, i);
    }
    fclose(f);
}

static void test_end_to_end_replay(void)
{
    write_replay_file();
    snprintf(g_journal_path, sizeof(g_journal_path),
             "test_pipe_%lld.log", (long long)ts_clock_mono_now());
    remove(g_journal_path);

    ts_metrics metrics;
    ts_metrics_init(&metrics);
    ts_vclock vc = {0};
    ts_clock clock = ts_clock_virtual(&vc);

    journal_t *j = journal_open_append(g_journal_path);
    ASSERT(j != NULL);

    oms_config ocfg = {.journal = j, .metrics = &metrics, .clock = clock};
    oms_t *o = oms_create(&ocfg, 1024);
    rms_config rcfg = {.journal = j, .metrics = &metrics, .clock = clock};
    rms_t *r = rms_create(&rcfg);
    /* conservative limits that still allow qty 0.5 orders */
    ts_risk_limits lim;
    ts_risk_limits_default(&lim);
    lim.max_order_qty = 100.0;
    lim.max_order_notional = 1e9;
    lim.max_position_qty = 1000.0;
    lim.daily_loss_limit = 1e9;
    rms_set_limits(r, &lim);

    venue_t *v = venue_mock_create(&metrics, clock);
    ems_config ecfg = {.venue = v, .oms = o, .rms = r,
                       .metrics = &metrics, .journal = j, .clock = clock};
    ems_t *e = ems_create(&ecfg);

    pipe_t p = {.oms = o, .rms = r, .ems = e, .metrics = &metrics,
                .vclock = &vc, .journal = j};

    momentum_config mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    ts_copy_str(mcfg.instrument, sizeof(mcfg.instrument), "BTCUSDT");
    ts_copy_str(mcfg.strategy_name, sizeof(mcfg.strategy_name), "mom");
    mcfg.qty = 0.5;
    mcfg.move_threshold = 0.5;
    mcfg.sink = sink;
    mcfg.sink_ctx = &p;
    strategy_t *s = strategy_momentum_create(&mcfg);
    ASSERT(s != NULL);

    replay_t *rep = replay_open(g_replay_path);
    ASSERT(rep != NULL);

    ts_mkt_event ev;
    size_t nev = 0;
    while (replay_next(rep, &ev) == TS_OK) {
        ts_vclock_set(&vc, ev.ts_ns);
        ts_metrics_inc(&metrics, TS_MET_MKT_EVENTS, 1);
        rms_on_quote(r, ev.instrument, ev.bid, ev.ask, ev.last, ev.ts_ns);
        ems_on_market_event(e, &ev);
        s->on_event(s, &ev);
        ems_poll(e);
        nev++;
    }
    ASSERT_EQ_I(nev, 20);

    /* momentum should have fired on most moves */
    uint64_t submitted = ts_metrics_get(&metrics, TS_MET_ORDERS_SUBMITTED);
    uint64_t execs = ts_metrics_get(&metrics, TS_MET_EXECUTIONS);
    printf("  pipeline: %llu orders, %llu execs, %zu open\n",
           (unsigned long long)submitted, (unsigned long long)execs,
           oms_open_count(o));
    ASSERT(submitted > 0);
    ASSERT(execs > 0);
    ASSERT(oms_open_count(o) == 0); /* all IOC orders resolve */

    s->destroy(s);
    replay_close(rep);
    ems_destroy(e);
    v->ops->destroy(v);
    rms_destroy(r);
    oms_destroy(o);
    journal_close(j);

    remove(g_replay_path);
    remove(g_journal_path);
}

static void test_journal_recovers_pipeline_state(void)
{
    write_replay_file();
    snprintf(g_journal_path, sizeof(g_journal_path),
             "test_rec_%lld.log", (long long)ts_clock_mono_now());
    remove(g_journal_path);

    size_t expected_orders = 0;
    double expected_qty = 0.0;

    /* run once */
    {
        ts_metrics metrics;
        ts_metrics_init(&metrics);
        ts_vclock vc = {0};
        ts_clock clock = ts_clock_virtual(&vc);
        journal_t *j = journal_open_append(g_journal_path);
        oms_config ocfg = {.journal = j, .metrics = &metrics, .clock = clock};
        oms_t *o = oms_create(&ocfg, 1024);
        rms_config rcfg = {.journal = j, .metrics = &metrics, .clock = clock};
        rms_t *r = rms_create(&rcfg);
        ts_risk_limits lim;
        ts_risk_limits_default(&lim);
        lim.max_order_qty = 100.0;
        lim.max_order_notional = 1e9;
        lim.max_position_qty = 1000.0;
        lim.daily_loss_limit = 1e9;
        rms_set_limits(r, &lim);
        venue_t *v = venue_mock_create(&metrics, clock);
        ems_config ecfg = {.venue = v, .oms = o, .rms = r,
                           .metrics = &metrics, .journal = j, .clock = clock};
        ems_t *e = ems_create(&ecfg);
        pipe_t p = {.oms = o, .rms = r, .ems = e, .metrics = &metrics,
                    .vclock = &vc, .journal = j};
        momentum_config mcfg;
        memset(&mcfg, 0, sizeof(mcfg));
        ts_copy_str(mcfg.instrument, sizeof(mcfg.instrument), "BTCUSDT");
        mcfg.qty = 0.5;
        mcfg.move_threshold = 0.5;
        mcfg.sink = sink;
        mcfg.sink_ctx = &p;
        strategy_t *s = strategy_momentum_create(&mcfg);
        replay_t *rep = replay_open(g_replay_path);
        ts_mkt_event ev;
        while (replay_next(rep, &ev) == TS_OK) {
            ts_vclock_set(&vc, ev.ts_ns);
            rms_on_quote(r, ev.instrument, ev.bid, ev.ask, ev.last, ev.ts_ns);
            ems_on_market_event(e, &ev);
            s->on_event(s, &ev);
            ems_poll(e);
        }
        expected_orders = oms_count(o);
        const ts_rms_position *pos = rms_position(r, "BTCUSDT");
        expected_qty = pos ? pos->qty : 0.0;
        printf("  session1: %zu orders, BTCUSDT qty %.3f\n",
               expected_orders, expected_qty);

        s->destroy(s);
        replay_close(rep);
        ems_destroy(e);
        v->ops->destroy(v);
        rms_destroy(r);
        oms_destroy(o);
        journal_close(j);
    }

    /* recover into fresh OMS/RMS */
    {
        ts_vclock vc = {0};
        ts_clock clock = ts_clock_virtual(&vc);
        oms_config ocfg = {.clock = clock};
        oms_t *o = oms_create(&ocfg, 4096);
        rms_config rcfg = {.clock = clock};
        rms_t *r = rms_create(&rcfg);

        /* reuse recover callback pattern */
        typedef struct { oms_t *o; rms_t *r; } rc2_t;
        rc2_t rc2 = {.o = o, .r = r};
        size_t skipped = 0;
        ASSERT(journal_replay(g_journal_path, pipe_rec_cb, &rc2,
                              &skipped) == TS_OK);

        ASSERT_EQ_I(oms_count(o), expected_orders);
        const ts_rms_position *pos = rms_position(r, "BTCUSDT");
        double qty = pos ? pos->qty : 0.0;
        ASSERT_EQ_D(qty, expected_qty, 1e-9);

        oms_destroy(o);
        rms_destroy(r);
    }

    remove(g_replay_path);
    remove(g_journal_path);
}

ts_status pipe_rec_cb(const journal_event *ev, void *vctx)
{
    struct { oms_t *o; rms_t *r; } *rc = vctx;
    switch (ev->type) {
    case JE_SNAP:
        return oms_restore_order(rc->o, &ev->order);
    case JE_EXEC:
        return rms_on_execution(rc->r, &ev->exec);
    case JE_KILL:
        if (ev->killed) rms_kill(rc->r); else rms_resume(rc->r);
        return TS_OK;
    default:
        return TS_OK;
    }
}

const test_case pipeline_tests[] = {
    {"end_to_end_replay", test_end_to_end_replay},
    {"journal_recovers_pipeline_state", test_journal_recovers_pipeline_state},
    {NULL, NULL},
};
