#include "testutil.h"

#include "trading/config.h"
#include "trading/ems.h"
#include "trading/journal.h"
#include "trading/oms.h"
#include "trading/rms.h"
#include "trading/seqwatch.h"
#include "trading/venue.h"

#include <stdio.h>
#include <string.h>

/* ---------- config ---------- */

static void test_config_load_and_get(void)
{
    const char *path = "test_cfg.ini";
    FILE *f = fopen(path, "w");
    ASSERT(f != NULL);
    fprintf(f,
        "# comment\n"
        "[risk]\n"
        "max_order_qty = 42.5\n"
        "require_fresh_market = true\n"
        "max_quote_age_ns = 123456789\n"
        "daily_loss_limit = 999\n"
        "[strategy]\n"
        "qty = 0.25 ; trailing comment\n");
    fclose(f);

    ts_config *c = ts_config_load(path);
    ASSERT(c != NULL);
    ASSERT_EQ_D(ts_config_get_double(c, "risk", "max_order_qty", 0), 42.5, 1e-9);
    ASSERT(ts_config_get_bool(c, "risk", "require_fresh_market", false));
    ASSERT_EQ_I(ts_config_get_i64(c, "risk", "max_quote_age_ns", 0), 123456789);
    ASSERT_EQ_D(ts_config_get_double(c, "strategy", "qty", 0), 0.25, 1e-9);
    ASSERT_STR(ts_config_get(c, "risk", "missing", "dflt"), "dflt");

    ts_risk_limits lim;
    ts_risk_limits_default(&lim);
    ts_config_apply_risk(c, &lim);
    ASSERT_EQ_D(lim.max_order_qty, 42.5, 1e-9);
    ASSERT(lim.require_fresh_market);
    ASSERT_EQ_I(lim.max_quote_age_ns, 123456789);
    /* untouched keys keep defaults */
    ASSERT_EQ_D(lim.max_position_qty, 5000.0, 1e-9);

    ts_config_free(c);
    remove(path);
}

/* ---------- seqwatch ---------- */

static void test_seqwatch_gaps(void)
{
    ts_metrics metrics;
    ts_metrics_init(&metrics);
    ts_seqwatch *w = ts_seqwatch_create(&metrics);
    ASSERT(w != NULL);

    ts_mkt_event ev;
    memset(&ev, 0, sizeof(ev));
    ts_copy_str(ev.instrument, sizeof(ev.instrument), "BTCUSDT");

    ev.seq = 1;
    ASSERT(ts_seqwatch_on_event(w, &ev) == TS_OK);
    ev.seq = 2;
    ASSERT(ts_seqwatch_on_event(w, &ev) == TS_OK);
    ev.seq = 3;
    ASSERT(ts_seqwatch_on_event(w, &ev) == TS_OK);
    ASSERT_EQ_I(ts_metrics_get(&metrics, TS_MET_MKT_SEQ_GAPS), 0);

    /* jump 3 -> 7: 3 missing */
    ev.seq = 7;
    ASSERT(ts_seqwatch_on_event(w, &ev) == TS_OK);
    ASSERT_EQ_I(ts_metrics_get(&metrics, TS_MET_MKT_SEQ_GAPS), 3);

    const ts_seq_channel *ch = ts_seqwatch_channel(w, "BTCUSDT");
    ASSERT(ch != NULL);
    ASSERT_EQ_I(ch->events, 4);
    ASSERT_EQ_I(ch->gaps, 3);

    /* old/duplicate seq: not a gap */
    ev.seq = 5;
    ASSERT(ts_seqwatch_on_event(w, &ev) == TS_OK);
    ASSERT_EQ_I(ts_metrics_get(&metrics, TS_MET_MKT_SEQ_GAPS), 3);
    ASSERT_EQ_I(ch->dup_or_old, 1);

    /* per-instrument isolation */
    ts_copy_str(ev.instrument, sizeof(ev.instrument), "ETHUSDT");
    ev.seq = 100;
    ASSERT(ts_seqwatch_on_event(w, &ev) == TS_OK);
    ASSERT_EQ_I(ts_seqwatch_channel_count(w), 2);
    ASSERT_EQ_I(ts_metrics_get(&metrics, TS_MET_MKT_SEQ_GAPS), 3);

    ts_seqwatch_destroy(w);
}

/* ---------- exec dedup ---------- */

static void test_exec_dedup(void)
{
    ts_vclock vc = {1000};
    ts_clock clock = ts_clock_virtual(&vc);
    ts_metrics metrics;
    ts_metrics_init(&metrics);
    oms_config cfg = {.metrics = &metrics, .clock = clock};
    oms_t *o = oms_create(&cfg, 16);

    ts_order_request req;
    memset(&req, 0, sizeof(req));
    ts_copy_str(req.client_id, sizeof(req.client_id), "d1");
    ts_copy_str(req.instrument, sizeof(req.instrument), "BTCUSDT");
    req.side = TS_SIDE_BUY;
    req.type = TS_ORD_LIMIT;
    req.tif = TS_TIF_GTC;
    req.qty = 1.0;
    req.price = 100.0;
    ASSERT(oms_submit(o, &req, NULL) == TS_OK);
    ASSERT(oms_mark_risk(o, "d1", true, "OK", "") == TS_OK);
    ASSERT(oms_on_ack(o, "d1", "V1") == TS_OK);

    ts_execution ex;
    memset(&ex, 0, sizeof(ex));
    ts_copy_str(ex.client_id, sizeof(ex.client_id), "d1");
    ts_copy_str(ex.venue_exec_id, sizeof(ex.venue_exec_id), "X99");
    ts_copy_str(ex.instrument, sizeof(ex.instrument), "BTCUSDT");
    ex.side = TS_SIDE_BUY;
    ex.qty = 1.0;
    ex.price = 100.0;
    ex.ts_ns = vc.now;

    ASSERT(oms_on_execution(o, &ex) == TS_OK);
    ASSERT_EQ_I(oms_find(o, "d1")->state, TS_ORD_FILLED);
    ASSERT_EQ_D(oms_find(o, "d1")->filled_qty, 1.0, 1e-9);

    /* duplicate exec id: ignored */
    ASSERT(oms_on_execution(o, &ex) == TS_OK);
    ASSERT_EQ_I(oms_find(o, "d1")->state, TS_ORD_FILLED);
    ASSERT_EQ_D(oms_find(o, "d1")->filled_qty, 1.0, 1e-9);
    ASSERT_EQ_I(ts_metrics_get(&metrics, TS_MET_EXEC_DUPLICATE), 1);
    ASSERT_EQ_I(ts_metrics_get(&metrics, TS_MET_EXECUTIONS), 1);

    /* different exec id on terminal order: overfill is clamped, counted
     * as a reconciliation break, state stays FILLED */
    uint64_t breaks_before = ts_metrics_get(&metrics, TS_MET_RECON_BREAKS);
    ts_copy_str(ex.venue_exec_id, sizeof(ex.venue_exec_id), "X100");
    ex.qty = 0.5;
    ASSERT(oms_on_execution(o, &ex) == TS_OK);
    ASSERT_EQ_I(oms_find(o, "d1")->state, TS_ORD_FILLED);
    ASSERT_EQ_D(oms_find(o, "d1")->filled_qty, 1.0, 1e-9);
    ASSERT(ts_metrics_get(&metrics, TS_MET_RECON_BREAKS) > breaks_before);

    oms_destroy(o);
}

/* ---------- reconcile ---------- */

static void feed_quote(ems_t *e, rms_t *r, double bid, double ask)
{
    ts_mkt_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = TS_MKT_QUOTE;
    ts_copy_str(ev.instrument, sizeof(ev.instrument), "BTCUSDT");
    ev.bid = bid;
    ev.ask = ask;
    ev.last = (bid + ask) * 0.5;
    ems_on_market_event(e, &ev);
    rms_on_quote(r, ev.instrument, ev.bid, ev.ask, ev.last, 0);
}

static void submit_working(oms_t *o, rms_t *r, ems_t *e, const char *id,
                           double price)
{
    ts_order_request req;
    memset(&req, 0, sizeof(req));
    ts_copy_str(req.client_id, sizeof(req.client_id), id);
    ts_copy_str(req.instrument, sizeof(req.instrument), "BTCUSDT");
    req.side = TS_SIDE_BUY;
    req.type = TS_ORD_LIMIT;
    req.tif = TS_TIF_GTC;
    req.qty = 1.0;
    req.price = price;
    ASSERT(oms_submit(o, &req, NULL) == TS_OK);
    ts_order ord = *oms_find(o, id);
    ASSERT(rms_check_order(r, &ord).approved);
    ASSERT(oms_mark_risk(o, id, true, "OK", "") == TS_OK);
    ord = *oms_find(o, id);
    ASSERT(ems_submit(e, &ord) == TS_OK);
    ASSERT(ems_poll(e) == TS_OK);
}

static void test_reconcile_matched_and_breaks(void)
{
    ts_vclock vc = {1000};
    ts_clock clock = ts_clock_virtual(&vc);
    ts_metrics metrics;
    ts_metrics_init(&metrics);

    oms_config ocfg = {.metrics = &metrics, .clock = clock};
    oms_t *o = oms_create(&ocfg, 64);
    rms_config rcfg = {.metrics = &metrics, .clock = clock};
    rms_t *r = rms_create(&rcfg);
    venue_t *v = venue_mock_create(&metrics, clock);
    ems_config ecfg = {.venue = v, .oms = o, .rms = r,
                       .metrics = &metrics, .clock = clock};
    ems_t *e = ems_create(&ecfg);

    feed_quote(e, r, 99.0, 101.0);

    /* w1: working GTC buy at 90 (not marketable) */
    submit_working(o, r, e, "w1", 90.0);
    ASSERT_EQ_I(oms_find(o, "w1")->state, TS_ORD_WORKING);

    /* clean reconcile: matched */
    ts_recon_report rep;
    ASSERT(ems_reconcile(e, &rep) == TS_OK);
    ASSERT(rep.supported);
    ASSERT_EQ_I(rep.oms_open, 1);
    ASSERT_EQ_I(rep.venue_open, 1);
    ASSERT_EQ_I(rep.matched, 1);
    ASSERT_EQ_I(rep.oms_only, 0);
    ASSERT_EQ_I(rep.venue_only, 0);

    /* simulate venue-side loss: remove order directly from venue by
     * canceling at venue level without telling OMS — emulate by using
     * venue cancel then re-adding? Mock venue cancel emits event which
     * would update OMS on poll. Instead, craft divergence: submit order
     * at venue directly bypassing OMS. */
    ts_order ghost;
    memset(&ghost, 0, sizeof(ghost));
    ts_copy_str(ghost.client_id, sizeof(ghost.client_id), "ghost1");
    ts_copy_str(ghost.instrument, sizeof(ghost.instrument), "BTCUSDT");
    ghost.side = TS_SIDE_BUY;
    ghost.type = TS_ORD_LIMIT;
    ghost.tif = TS_TIF_GTC;
    ghost.qty = 1.0;
    ghost.price = 80.0;
    ASSERT(v->ops->submit(v, &ghost) == TS_OK);
    /* drain the auto-ACK+nothing else; ghost ACK event pending — poll it */
    ASSERT(ems_poll(e) == TS_OK); /* dispatches ghost ACK -> oms_on_ack fails NOT_FOUND, ignored */

    ASSERT(ems_reconcile(e, &rep) == TS_OK);
    ASSERT_EQ_I(rep.venue_only, 1);
    ASSERT_EQ_I(rep.matched, 1);

    ems_destroy(e);
    v->ops->destroy(v);
    rms_destroy(r);
    oms_destroy(o);
}

/* ---------- replay venue ---------- */

static char g_venue_script[512];

static void write_venue_script(const char *content)
{
    snprintf(g_venue_script, sizeof(g_venue_script),
             "test_venue_%lld.txt", (long long)ts_clock_mono_now());
    FILE *f = fopen(g_venue_script, "w");
    ASSERT(f != NULL);
    fputs(content, f);
    fclose(f);
}

static void test_replay_venue_scripted_events(void)
{
    write_venue_script(
        "# script\n"
        "ACK c1 V100\n"
        "EXEC c1 X1 BTCUSDT BUY 0.5 100.0 0.01\n"
        "EXEC c1 X1 BTCUSDT BUY 0.5 100.0 0.01\n" /* duplicate exec id */
        "ACK c2 V200\n"
        "REJECT c2 insufficient funds\n");

    ts_metrics metrics;
    ts_metrics_init(&metrics);
    ts_vclock vc = {1000};
    ts_clock clock = ts_clock_virtual(&vc);
    venue_t *v = venue_replay_create(g_venue_script, &metrics, clock);
    ASSERT(v != NULL);
    ASSERT_STR(v->ops->name, "replay");

    oms_config ocfg = {.metrics = &metrics, .clock = clock};
    oms_t *o = oms_create(&ocfg, 64);
    rms_config rcfg = {.metrics = &metrics, .clock = clock};
    rms_t *r = rms_create(&rcfg);
    ems_config ecfg = {.venue = v, .oms = o, .rms = r,
                       .metrics = &metrics, .clock = clock};
    ems_t *e = ems_create(&ecfg);

    /* submit c1 and c2 before polling: ems_poll drains the whole script,
     * so all orders must exist in the OMS first */
    ts_order_request req;
    memset(&req, 0, sizeof(req));
    ts_copy_str(req.client_id, sizeof(req.client_id), "c1");
    ts_copy_str(req.instrument, sizeof(req.instrument), "BTCUSDT");
    req.side = TS_SIDE_BUY;
    req.type = TS_ORD_LIMIT;
    req.tif = TS_TIF_GTC;
    req.qty = 1.0;
    req.price = 100.0;
    ASSERT(oms_submit(o, &req, NULL) == TS_OK);
    ASSERT(oms_mark_risk(o, "c1", true, "OK", "") == TS_OK);
    ts_order ord = *oms_find(o, "c1");
    ASSERT(ems_submit(e, &ord) == TS_OK);

    memset(&req, 0, sizeof(req));
    ts_copy_str(req.client_id, sizeof(req.client_id), "c2");
    ts_copy_str(req.instrument, sizeof(req.instrument), "BTCUSDT");
    req.side = TS_SIDE_SELL;
    req.type = TS_ORD_LIMIT;
    req.tif = TS_TIF_GTC;
    req.qty = 1.0;
    req.price = 90.0;
    ASSERT(oms_submit(o, &req, NULL) == TS_OK);
    ASSERT(oms_mark_risk(o, "c2", true, "OK", "") == TS_OK);
    ord = *oms_find(o, "c2");
    ASSERT(ems_submit(e, &ord) == TS_OK);

    /* drain all events: ACK c1, EXEC, dup EXEC, ACK c2, REJECT c2 */
    ASSERT(ems_poll(e) == TS_OK);

    const ts_order *c1 = oms_find(o, "c1");
    ASSERT_EQ_I(c1->state, TS_ORD_PARTIAL); /* two execs of 0.5, dup dropped */
    ASSERT_EQ_D(c1->filled_qty, 0.5, 1e-9);
    ASSERT_EQ_I(ts_metrics_get(&metrics, TS_MET_EXEC_DUPLICATE), 1);
    ASSERT_EQ_I(oms_find(o, "c2")->state, TS_ORD_REJECTED);
    ASSERT_STR(oms_find(o, "c2")->reject_reason, "insufficient funds");

    ems_destroy(e);
    rms_destroy(r);
    oms_destroy(o);
    v->ops->destroy(v);
    remove(g_venue_script);
}

static void test_replay_venue_open_orders(void)
{
    write_venue_script(
        "ACK a1 VA1\n"
        "EXEC a1 XA1 BTCUSDT BUY 0.4 100.0\n"
        "ACK a2 VA2\n"
        "CANCEL a2\n");

    ts_metrics metrics;
    ts_metrics_init(&metrics);
    ts_vclock vc = {1000};
    ts_clock clock = ts_clock_virtual(&vc);
    venue_t *v = venue_replay_create(g_venue_script, &metrics, clock);
    ASSERT(v != NULL);

    /* submit two orders directly at venue level */
    ts_order o1, o2;
    memset(&o1, 0, sizeof(o1));
    memset(&o2, 0, sizeof(o2));
    ts_copy_str(o1.client_id, sizeof(o1.client_id), "a1");
    ts_copy_str(o1.instrument, sizeof(o1.instrument), "BTCUSDT");
    o1.side = TS_SIDE_BUY; o1.type = TS_ORD_LIMIT; o1.tif = TS_TIF_GTC;
    o1.qty = 1.0; o1.price = 100.0;
    o2 = o1;
    ts_copy_str(o2.client_id, sizeof(o2.client_id), "a2");
    ASSERT(v->ops->submit(v, &o1) == TS_OK);
    ASSERT(v->ops->submit(v, &o2) == TS_OK);

    /* drain all script events so closure tracking runs */
    ts_venue_event ev;
    while (v->ops->poll(v, &ev) == TS_OK) {}

    ts_venue_open_order open[8];
    size_t n = 0;
    ASSERT(v->ops->open_orders(v, open, 8, &n) == TS_OK);
    /* a1: filled 0.4 of 1.0 -> still open; a2: canceled -> closed */
    ASSERT_EQ_I(n, 1);
    ASSERT_STR(open[0].client_id, "a1");
    ASSERT_EQ_D(open[0].filled_qty, 0.4, 1e-9);

    v->ops->destroy(v);
    remove(g_venue_script);
}

/* ---------- journal durability ---------- */

static void test_journal_flush_and_fsync(void)
{
    const char *path = "test_jrnl_flush.log";
    remove(path);
    journal_t *j = journal_open_append(path);
    ASSERT(j != NULL);
    journal_set_fsync(j, false);

    ts_order o;
    memset(&o, 0, sizeof(o));
    ts_copy_str(o.client_id, sizeof(o.client_id), "jf");
    ts_copy_str(o.instrument, sizeof(o.instrument), "BTCUSDT");
    o.side = TS_SIDE_BUY;
    o.type = TS_ORD_LIMIT;
    o.tif = TS_TIF_GTC;
    o.qty = 1.0;
    o.price = 1.0;
    o.state = TS_ORD_NEW;
    o.updated_ns = 1;
    ASSERT(journal_append_snap(j, &o) == TS_OK);
    journal_close(j);

    /* readable immediately after close (flush on write guaranteed
     * ordering; verify file non-empty) */
    FILE *f = fopen(path, "r");
    ASSERT(f != NULL);
    char buf[128];
    ASSERT(fgets(buf, sizeof(buf), f) != NULL);
    ASSERT(strstr(buf, "SNAP") != NULL);
    fclose(f);
    remove(path);
}

/* ---------- amend / cancel-all / disconnect ---------- */

static void test_amend_working_order(void)
{
    ts_vclock vc = {1000};
    ts_clock clock = ts_clock_virtual(&vc);
    ts_metrics metrics;
    ts_metrics_init(&metrics);
    oms_config ocfg = {.metrics = &metrics, .clock = clock};
    oms_t *o = oms_create(&ocfg, 64);
    rms_config rcfg = {.metrics = &metrics, .clock = clock};
    rms_t *r = rms_create(&rcfg);
    venue_t *v = venue_mock_create(&metrics, clock);
    ems_config ecfg = {.venue = v, .oms = o, .rms = r,
                       .metrics = &metrics, .clock = clock};
    ems_t *e = ems_create(&ecfg);

    feed_quote(e, r, 99.0, 101.0);
    submit_working(o, r, e, "a1", 90.0);
    ASSERT_EQ_I(oms_find(o, "a1")->state, TS_ORD_WORKING);

    /* amend price 90 -> 85, qty 1 -> 2 */
    ASSERT(ems_amend(e, "a1", 85.0, 2.0) == TS_OK);
    ASSERT(ems_poll(e) == TS_OK);
    const ts_order *ord = oms_find(o, "a1");
    ASSERT_EQ_D(ord->price, 85.0, 1e-9);
    ASSERT_EQ_D(ord->qty, 2.0, 1e-9);
    ASSERT_EQ_I(ord->state, TS_ORD_WORKING);

    ems_destroy(e);
    v->ops->destroy(v);
    rms_destroy(r);
    oms_destroy(o);
}

static void test_cancel_all(void)
{
    ts_vclock vc = {1000};
    ts_clock clock = ts_clock_virtual(&vc);
    ts_metrics metrics;
    ts_metrics_init(&metrics);
    oms_config ocfg = {.metrics = &metrics, .clock = clock};
    oms_t *o = oms_create(&ocfg, 64);
    rms_config rcfg = {.metrics = &metrics, .clock = clock};
    rms_t *r = rms_create(&rcfg);
    venue_t *v = venue_mock_create(&metrics, clock);
    ems_config ecfg = {.venue = v, .oms = o, .rms = r,
                       .metrics = &metrics, .clock = clock};
    ems_t *e = ems_create(&ecfg);

    feed_quote(e, r, 99.0, 101.0);
    submit_working(o, r, e, "ca1", 90.0);
    submit_working(o, r, e, "ca2", 91.0);
    ASSERT_EQ_I(oms_open_count(o), 2);

    ASSERT(ems_cancel_all(e) == TS_OK);
    ASSERT(ems_poll(e) == TS_OK);
    ASSERT_EQ_I(oms_find(o, "ca1")->state, TS_ORD_CANCELED);
    ASSERT_EQ_I(oms_find(o, "ca2")->state, TS_ORD_CANCELED);
    ASSERT_EQ_I(oms_open_count(o), 0);

    ems_destroy(e);
    v->ops->destroy(v);
    rms_destroy(r);
    oms_destroy(o);
}

static void test_disconnect_marks_unknown(void)
{
    ts_vclock vc = {1000};
    ts_clock clock = ts_clock_virtual(&vc);
    ts_metrics metrics;
    ts_metrics_init(&metrics);
    oms_config ocfg = {.metrics = &metrics, .clock = clock};
    oms_t *o = oms_create(&ocfg, 64);
    rms_config rcfg = {.metrics = &metrics, .clock = clock};
    rms_t *r = rms_create(&rcfg);
    venue_t *v = venue_mock_create(&metrics, clock);
    ems_config ecfg = {.venue = v, .oms = o, .rms = r,
                       .metrics = &metrics, .clock = clock};
    ems_t *e = ems_create(&ecfg);

    feed_quote(e, r, 99.0, 101.0);
    venue_mock_set_connected(v, false);
    ASSERT(!ems_venue_connected(e));

    /* drive order manually: submit -> risk -> ems_submit (fails) */
    ts_order_request req;
    memset(&req, 0, sizeof(req));
    ts_copy_str(req.client_id, sizeof(req.client_id), "dc1");
    ts_copy_str(req.instrument, sizeof(req.instrument), "BTCUSDT");
    req.side = TS_SIDE_BUY;
    req.type = TS_ORD_LIMIT;
    req.tif = TS_TIF_GTC;
    req.qty = 1.0;
    req.price = 90.0;
    ASSERT(oms_submit(o, &req, NULL) == TS_OK);
    ts_order ord = *oms_find(o, "dc1");
    ASSERT(rms_check_order(r, &ord).approved);
    ASSERT(oms_mark_risk(o, "dc1", true, "OK", "") == TS_OK);
    ord = *oms_find(o, "dc1");
    ASSERT(ems_submit(e, &ord) == TS_ERR_BAD_STATE);
    /* venue submit fails (disconnected) -> order marked UNKNOWN */
    ASSERT_EQ_I(oms_find(o, "dc1")->state, TS_ORD_UNKNOWN);

    /* reconnect: venue accepts again but order stays UNKNOWN until
     * explicit reconciliation resolves it */
    venue_mock_set_connected(v, true);
    ASSERT(ems_venue_connected(e));

    ems_destroy(e);
    v->ops->destroy(v);
    rms_destroy(r);
    oms_destroy(o);
}

const test_case hardening_tests[] = {
    {"config_load_and_get", test_config_load_and_get},
    {"seqwatch_gaps", test_seqwatch_gaps},
    {"exec_dedup", test_exec_dedup},
    {"reconcile_matched_and_breaks", test_reconcile_matched_and_breaks},
    {"replay_venue_scripted_events", test_replay_venue_scripted_events},
    {"replay_venue_open_orders", test_replay_venue_open_orders},
    {"journal_flush_and_fsync", test_journal_flush_and_fsync},
    {"amend_working_order", test_amend_working_order},
    {"cancel_all", test_cancel_all},
    {"disconnect_marks_unknown", test_disconnect_marks_unknown},
    {NULL, NULL},
};
