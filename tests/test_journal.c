#include "testutil.h"

#include "trading/journal.h"
#include "trading/oms.h"
#include "trading/rms.h"

#include <stdio.h>
#include <string.h>

ts_status jc_cb(const journal_event *ev, void *ctx);
ts_status rec_cb(const journal_event *ev, void *ctx);

static char g_path[512];

static const char *tmp_journal(void)
{
    snprintf(g_path, sizeof(g_path), "test_journal_%lld.log",
             (long long)ts_clock_mono_now());
    remove(g_path);
    return g_path;
}

static ts_order make_order(const char *id, ts_order_state state,
                           double filled)
{
    ts_order o;
    memset(&o, 0, sizeof(o));
    ts_copy_str(o.client_id, sizeof(o.client_id), id);
    ts_copy_str(o.venue_order_id, sizeof(o.venue_order_id), "V1");
    ts_copy_str(o.instrument, sizeof(o.instrument), "BTCUSDT");
    ts_copy_str(o.strategy, sizeof(o.strategy), "t");
    o.side = TS_SIDE_BUY;
    o.type = TS_ORD_LIMIT;
    o.tif = TS_TIF_GTC;
    o.qty = 2.0;
    o.price = 100.0;
    o.filled_qty = filled;
    o.avg_fill_price = filled > 0 ? 100.5 : 0.0;
    o.state = state;
    o.created_ns = 12345;
    o.updated_ns = 12345;
    return o;
}

static void test_journal_write_read(void)
{
    const char *path = tmp_journal();
    journal_t *j = journal_open_append(path);
    ASSERT(j != NULL);

    ts_order o = make_order("j1", TS_ORD_WORKING, 0.0);
    ASSERT(journal_append_snap(j, &o) == TS_OK);
    ASSERT(journal_append_snap(j, &o) == TS_OK);

    ts_execution ex;
    memset(&ex, 0, sizeof(ex));
    ts_copy_str(ex.client_id, sizeof(ex.client_id), "j1");
    ts_copy_str(ex.venue_exec_id, sizeof(ex.venue_exec_id), "X1");
    ts_copy_str(ex.instrument, sizeof(ex.instrument), "BTCUSDT");
    ex.side = TS_SIDE_BUY;
    ex.qty = 1.0;
    ex.price = 100.5;
    ex.ts_ns = 12346;
    ASSERT(journal_append_exec(j, &ex) == TS_OK);

    ASSERT(journal_append_risk(j, 12347, "j1", true, "OK", "") == TS_OK);
    ASSERT(journal_append_risk(j, 12348, "j2", false, "QTY_LIMIT",
                               "too big") == TS_OK);
    ASSERT(journal_append_kill(j, 12349, true) == TS_OK);
    ASSERT(journal_append_acct(j, 12350, "USD", 50000.0) == TS_OK);
    journal_close(j);

    /* replay and count */
    typedef struct { int c[8]; double acct; bool kill; } rc_t;
    rc_t rc;
    memset(&rc, 0, sizeof(rc));

    size_t skipped = 0;
    ASSERT(journal_replay(path, jc_cb, &rc, &skipped) == TS_OK);
    ASSERT_EQ_I(skipped, 0);
    ASSERT_EQ_I(rc.c[JE_SNAP], 2);
    ASSERT_EQ_I(rc.c[JE_EXEC], 1);
    ASSERT_EQ_I(rc.c[JE_RISK], 2);
    ASSERT_EQ_I(rc.c[JE_KILL], 1);
    ASSERT_EQ_I(rc.c[JE_ACCT], 1);
    ASSERT_EQ_D(rc.acct, 50000.0, 1e-9);
    ASSERT(rc.kill);

    remove(path);
}

ts_status jc_cb(const journal_event *ev, void *vctx)
{
    struct { int c[8]; double acct; bool kill; } *rc = vctx;
    if (ev->type <= JE_ACCT) {
        rc->c[ev->type]++;
    }
    if (ev->type == JE_ACCT) {
        rc->acct = ev->amount;
    }
    if (ev->type == JE_KILL) {
        rc->kill = ev->killed;
    }
    return TS_OK;
}

static void test_recovery_rebuilds_state(void)
{
    const char *path = tmp_journal();
    ts_vclock vc = {1000};
    ts_clock clock = ts_clock_virtual(&vc);

    /* session 1: submit, approve, ack, partial fill, kill */
    {
        journal_t *j = journal_open_append(path);
        oms_config ocfg = {.journal = j, .clock = clock};
        oms_t *o = oms_create(&ocfg, 64);
        rms_config rcfg = {.journal = j, .clock = clock};
        rms_t *r = rms_create(&rcfg);

        ts_order_request req;
        memset(&req, 0, sizeof(req));
        ts_copy_str(req.client_id, sizeof(req.client_id), "rec1");
        ts_copy_str(req.instrument, sizeof(req.instrument), "BTCUSDT");
        req.side = TS_SIDE_BUY;
        req.type = TS_ORD_LIMIT;
        req.tif = TS_TIF_GTC;
        req.qty = 2.0;
        req.price = 100.0;
        ASSERT(oms_submit(o, &req, NULL) == TS_OK);
        ASSERT(oms_mark_risk(o, "rec1", true, "OK", "") == TS_OK);
        ASSERT(oms_on_ack(o, "rec1", "V9") == TS_OK);

        ts_execution ex;
        memset(&ex, 0, sizeof(ex));
        ts_copy_str(ex.client_id, sizeof(ex.client_id), "rec1");
        ts_copy_str(ex.instrument, sizeof(ex.instrument), "BTCUSDT");
        ex.side = TS_SIDE_BUY;
        ex.qty = 1.0;
        ex.price = 100.0;
        ex.ts_ns = vc.now;
        ASSERT(oms_on_execution(o, &ex) == TS_OK);
        ASSERT(rms_on_execution(r, &ex) == TS_OK);

        rms_kill(r);

        oms_destroy(o);
        rms_destroy(r);
        journal_close(j);
    }

    /* session 2: recover */
    {
        oms_config ocfg = {.clock = clock};
        oms_t *o = oms_create(&ocfg, 64);
        rms_config rcfg = {.clock = clock};
        rms_t *r = rms_create(&rcfg);

        /* replay via same callback style as app */
        typedef struct { oms_t *o; rms_t *r; } rc2_t;
        rc2_t rc2 = {.o = o, .r = r};
        ASSERT(journal_replay(path, rec_cb, &rc2, NULL) == TS_OK);

        const ts_order *ord = oms_find(o, "rec1");
        ASSERT(ord != NULL);
        ASSERT_EQ_I(ord->state, TS_ORD_PARTIAL);
        ASSERT_EQ_D(ord->filled_qty, 1.0, 1e-9);
        ASSERT_STR(ord->venue_order_id, "V9");

        const ts_rms_position *pos = rms_position(r, "BTCUSDT");
        ASSERT(pos != NULL);
        ASSERT_EQ_D(pos->qty, 1.0, 1e-9);
        ASSERT(rms_is_killed(r));

        oms_destroy(o);
        rms_destroy(r);
    }

    remove(path);
}

ts_status rec_cb(const journal_event *ev, void *vctx)
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

const test_case journal_tests[] = {
    {"journal_write_read", test_journal_write_read},
    {"recovery_rebuilds_state", test_recovery_rebuilds_state},
    {NULL, NULL},
};
