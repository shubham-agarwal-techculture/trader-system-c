#include "testutil.h"

#include "trading/ems.h"
#include "trading/oms.h"
#include "trading/rms.h"
#include "trading/venue.h"

#include <string.h>

static ts_vclock g_vc;
static ts_clock g_clock;

static void setup(ts_metrics *metrics, oms_t **o, rms_t **r, ems_t **e,
                  venue_t **v)
{
    g_vc.now = 5000000000LL;
    g_clock = ts_clock_virtual(&g_vc);

    oms_config ocfg = {.metrics = metrics, .clock = g_clock};
    *o = oms_create(&ocfg, 64);

    rms_config rcfg = {.metrics = metrics, .clock = g_clock};
    *r = rms_create(&rcfg);

    *v = venue_mock_create(metrics, g_clock);

    ems_config ecfg = {
        .venue = *v, .oms = *o, .rms = *r,
        .metrics = metrics, .clock = g_clock,
    };
    *e = ems_create(&ecfg);
}

static void teardown(oms_t *o, rms_t *r, ems_t *e, venue_t *v)
{
    ems_destroy(e);
    v->ops->destroy(v);
    rms_destroy(r);
    oms_destroy(o);
}

static ts_order make_approved(oms_t *o, rms_t *r, const char *id,
                              ts_side side, ts_order_type type,
                              ts_tif tif, double qty, double price)
{
    ts_order_request req;
    memset(&req, 0, sizeof(req));
    ts_copy_str(req.client_id, sizeof(req.client_id), id);
    ts_copy_str(req.instrument, sizeof(req.instrument), "BTCUSDT");
    ts_copy_str(req.strategy, sizeof(req.strategy), "t");
    req.side = side;
    req.type = type;
    req.tif = tif;
    req.qty = qty;
    req.price = price;
    ASSERT(oms_submit(o, &req, NULL) == TS_OK);

    ts_order ord = *oms_find(o, id);
    ts_risk_result risk = rms_check_order(r, &ord);
    ASSERT(risk.approved);
    ASSERT(oms_mark_risk(o, id, true, "OK", "") == TS_OK);
    return *oms_find(o, id);
}

static void feed_quote(ems_t *e, rms_t *r, double bid, double ask)
{
    ts_mkt_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = TS_MKT_QUOTE;
    ts_copy_str(ev.instrument, sizeof(ev.instrument), "BTCUSDT");
    ev.bid = bid;
    ev.ask = ask;
    ev.last = (bid + ask) * 0.5;
    ev.ts_ns = g_vc.now;
    rms_on_quote(r, ev.instrument, ev.bid, ev.ask, ev.last, ev.ts_ns);
    ems_on_market_event(e, &ev);
}

static void test_market_order_fills(void)
{
    ts_metrics metrics;
    ts_metrics_init(&metrics);
    oms_t *o; rms_t *r; ems_t *e; venue_t *v;
    setup(&metrics, &o, &r, &e, &v);

    feed_quote(e, r, 99.0, 101.0);

    ts_order ord = make_approved(o, r, "m1", TS_SIDE_BUY, TS_ORD_MARKET,
                                 TS_TIF_IOC, 1.0, 0.0);
    ASSERT(ems_submit(e, &ord) == TS_OK);
    ASSERT(ems_poll(e) == TS_OK);

    const ts_order *after = oms_find(o, "m1");
    ASSERT_EQ_I(after->state, TS_ORD_FILLED);
    ASSERT_EQ_D(after->avg_fill_price, 101.0, 1e-9); /* lifted the ask */
    ASSERT_EQ_D(rms_position(r, "BTCUSDT")->qty, 1.0, 1e-9);
    /* filled at ask 101, last mark is mid 100 -> unrealized -1 */
    ASSERT_EQ_D(rms_unrealized_pnl(r), -1.0, 1e-9);

    teardown(o, r, e, v);
}

static void test_limit_order_rests_then_fills(void)
{
    ts_metrics metrics;
    ts_metrics_init(&metrics);
    oms_t *o; rms_t *r; ems_t *e; venue_t *v;
    setup(&metrics, &o, &r, &e, &v);

    feed_quote(e, r, 99.0, 101.0);

    /* buy limit at 95: not marketable, rests on book */
    ts_order ord = make_approved(o, r, "l1", TS_SIDE_BUY, TS_ORD_LIMIT,
                                 TS_TIF_GTC, 1.0, 95.0);
    ASSERT(ems_submit(e, &ord) == TS_OK);
    ASSERT(ems_poll(e) == TS_OK);
    ASSERT_EQ_I(oms_find(o, "l1")->state, TS_ORD_WORKING);

    /* price drops: bid 94 / ask 96 -> buy limit 95 >= ask? no: 95 >= 96 false.
     * ask must drop to <=95. Set ask 94.5 -> marketable at ask. */
    feed_quote(e, r, 93.0, 94.5);
    ASSERT(ems_poll(e) == TS_OK);
    const ts_order *after = oms_find(o, "l1");
    ASSERT_EQ_I(after->state, TS_ORD_FILLED);
    ASSERT_EQ_D(after->avg_fill_price, 94.5, 1e-9);

    teardown(o, r, e, v);
}

static void test_ioc_no_fill_expires(void)
{
    ts_metrics metrics;
    ts_metrics_init(&metrics);
    oms_t *o; rms_t *r; ems_t *e; venue_t *v;
    setup(&metrics, &o, &r, &e, &v);

    feed_quote(e, r, 99.0, 101.0);

    /* buy IOC at 90: not marketable -> expires */
    ts_order ord = make_approved(o, r, "i1", TS_SIDE_BUY, TS_ORD_LIMIT,
                                 TS_TIF_IOC, 1.0, 90.0);
    ASSERT(ems_submit(e, &ord) == TS_OK);
    ASSERT(ems_poll(e) == TS_OK);
    ASSERT_EQ_I(oms_find(o, "i1")->state, TS_ORD_EXPIRED);

    teardown(o, r, e, v);
}

static void test_venue_reject_flows_to_oms(void)
{
    ts_metrics metrics;
    ts_metrics_init(&metrics);
    oms_t *o; rms_t *r; ems_t *e; venue_t *v;
    setup(&metrics, &o, &r, &e, &v);

    feed_quote(e, r, 99.0, 101.0);
    venue_mock_set_reject_all(v, true);

    ts_order ord = make_approved(o, r, "r1", TS_SIDE_BUY, TS_ORD_MARKET,
                                 TS_TIF_IOC, 1.0, 0.0);
    ASSERT(ems_submit(e, &ord) == TS_OK);
    ASSERT(ems_poll(e) == TS_OK);
    ASSERT_EQ_I(oms_find(o, "r1")->state, TS_ORD_REJECTED);

    teardown(o, r, e, v);
}

static void test_cancel_working_order(void)
{
    ts_metrics metrics;
    ts_metrics_init(&metrics);
    oms_t *o; rms_t *r; ems_t *e; venue_t *v;
    setup(&metrics, &o, &r, &e, &v);

    feed_quote(e, r, 99.0, 101.0);

    ts_order ord = make_approved(o, r, "cx", TS_SIDE_BUY, TS_ORD_LIMIT,
                                 TS_TIF_GTC, 1.0, 90.0);
    ASSERT(ems_submit(e, &ord) == TS_OK);
    ASSERT(ems_poll(e) == TS_OK);
    ASSERT_EQ_I(oms_find(o, "cx")->state, TS_ORD_WORKING);

    ASSERT(ems_cancel(e, "cx") == TS_OK);
    ASSERT(ems_poll(e) == TS_OK);
    ASSERT_EQ_I(oms_find(o, "cx")->state, TS_ORD_CANCELED);

    teardown(o, r, e, v);
}

static void test_duplicate_submit_idempotent_at_venue(void)
{
    ts_metrics metrics;
    ts_metrics_init(&metrics);
    oms_t *o; rms_t *r; ems_t *e; venue_t *v;
    setup(&metrics, &o, &r, &e, &v);

    feed_quote(e, r, 99.0, 101.0);

    ts_order ord = make_approved(o, r, "d1", TS_SIDE_BUY, TS_ORD_LIMIT,
                                 TS_TIF_GTC, 1.0, 90.0);
    ASSERT(ems_submit(e, &ord) == TS_OK);
    ASSERT(ems_poll(e) == TS_OK);
    /* second dispatch of the same client order: venue dedups */
    ASSERT(ems_submit(e, &ord) == TS_OK);
    ASSERT(ems_poll(e) == TS_OK);
    ASSERT_EQ_I(oms_count(o), 1);

    teardown(o, r, e, v);
}

const test_case ems_tests[] = {
    {"market_order_fills", test_market_order_fills},
    {"limit_order_rests_then_fills", test_limit_order_rests_then_fills},
    {"ioc_no_fill_expires", test_ioc_no_fill_expires},
    {"venue_reject_flows_to_oms", test_venue_reject_flows_to_oms},
    {"cancel_working_order", test_cancel_working_order},
    {"duplicate_submit_idempotent", test_duplicate_submit_idempotent_at_venue},
    {NULL, NULL},
};
