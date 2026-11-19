#include "testutil.h"

#include "trading/rms.h"

#include <string.h>

static ts_vclock g_vc;
static ts_clock g_clock;

static void setup(void)
{
    g_vc.now = 1000000000LL; /* 1s in ns */
    g_clock = ts_clock_virtual(&g_vc);
}

static rms_t *make_rms(void)
{
    rms_config cfg = {.clock = g_clock};
    return rms_create(&cfg);
}

static ts_order make_order(const char *instr, ts_side side, double qty,
                           double price)
{
    ts_order o;
    memset(&o, 0, sizeof(o));
    ts_copy_str(o.client_id, sizeof(o.client_id), "c1");
    ts_copy_str(o.instrument, sizeof(o.instrument), instr);
    o.side = side;
    o.type = TS_ORD_LIMIT;
    o.tif = TS_TIF_GTC;
    o.qty = qty;
    o.price = price;
    o.state = TS_ORD_NEW;
    return o;
}

static void test_defaults_approve(void)
{
    setup();
    rms_t *r = make_rms();
    ASSERT(r != NULL);

    rms_on_quote(r, "BTCUSDT", 99.0, 101.0, 100.0, g_vc.now);
    ts_order o = make_order("BTCUSDT", TS_SIDE_BUY, 1.0, 100.0);
    ts_risk_result res = rms_check_order(r, &o);
    ASSERT(res.approved);
    ASSERT_EQ_I(res.code, TS_RISK_OK);

    rms_destroy(r);
}

static void test_kill_switch(void)
{
    setup();
    rms_t *r = make_rms();
    rms_kill(r);
    ASSERT(rms_is_killed(r));

    ts_order o = make_order("BTCUSDT", TS_SIDE_BUY, 1.0, 100.0);
    ts_risk_result res = rms_check_order(r, &o);
    ASSERT(!res.approved);
    ASSERT_EQ_I(res.code, TS_RISK_KILL_SWITCH);

    rms_resume(r);
    ASSERT(!rms_is_killed(r));
    res = rms_check_order(r, &o);
    ASSERT(res.approved);

    rms_destroy(r);
}

static void test_qty_and_notional_limits(void)
{
    setup();
    rms_t *r = make_rms();
    ts_risk_limits lim;
    ts_risk_limits_default(&lim);
    lim.max_order_qty = 10.0;
    lim.max_order_notional = 5000.0;
    rms_set_limits(r, &lim);

    ts_order o = make_order("BTCUSDT", TS_SIDE_BUY, 11.0, 100.0);
    ASSERT_EQ_I(rms_check_order(r, &o).code, TS_RISK_QTY_LIMIT);

    /* qty within limit but notional over: 10 * 600 = 6000 > 5000 */
    o.qty = 10.0;
    o.price = 600.0;
    ASSERT_EQ_I(rms_check_order(r, &o).code, TS_RISK_NOTIONAL_LIMIT);

    rms_destroy(r);
}

static void test_position_limit(void)
{
    setup();
    rms_t *r = make_rms();
    ts_risk_limits lim;
    ts_risk_limits_default(&lim);
    lim.max_order_qty = 1e9;
    lim.max_order_notional = 1e18;
    lim.max_position_qty = 100.0;
    rms_set_limits(r, &lim);

    ts_order o = make_order("BTCUSDT", TS_SIDE_BUY, 60.0, 100.0);
    ASSERT(rms_check_order(r, &o).approved);

    /* simulate fills to build position */
    ts_execution ex;
    memset(&ex, 0, sizeof(ex));
    ts_copy_str(ex.client_id, sizeof(ex.client_id), "c1");
    ts_copy_str(ex.instrument, sizeof(ex.instrument), "BTCUSDT");
    ex.side = TS_SIDE_BUY;
    ex.qty = 60.0;
    ex.price = 100.0;
    ex.ts_ns = g_vc.now;
    ASSERT(rms_on_execution(r, &ex) == TS_OK);

    /* another buy of 50 -> position 110 > 100 */
    ts_order o2 = make_order("BTCUSDT", TS_SIDE_BUY, 50.0, 100.0);
    ts_copy_str(o2.client_id, sizeof(o2.client_id), "c2");
    ASSERT_EQ_I(rms_check_order(r, &o2).code, TS_RISK_POSITION_LIMIT);

    /* sell reduces position: 60 -> allowed */
    ts_order o3 = make_order("BTCUSDT", TS_SIDE_SELL, 50.0, 100.0);
    ts_copy_str(o3.client_id, sizeof(o3.client_id), "c3");
    ASSERT(rms_check_order(r, &o3).approved);

    rms_destroy(r);
}

static void test_rate_limit(void)
{
    setup();
    rms_t *r = make_rms();
    ts_risk_limits lim;
    ts_risk_limits_default(&lim);
    lim.max_order_rate = 3.0;
    rms_set_limits(r, &lim);

    ts_order o = make_order("BTCUSDT", TS_SIDE_BUY, 1.0, 100.0);
    ASSERT(rms_check_order(r, &o).approved);
    ASSERT(rms_check_order(r, &o).approved);
    ASSERT(rms_check_order(r, &o).approved);
    ASSERT_EQ_I(rms_check_order(r, &o).code, TS_RISK_RATE_LIMIT);

    /* advance 1.1s -> window clears */
    g_vc.now += 1100000000LL;
    ASSERT(rms_check_order(r, &o).approved);

    rms_destroy(r);
}

static void test_stale_and_missing_market_data(void)
{
    setup();
    rms_t *r = make_rms();
    ts_risk_limits lim;
    ts_risk_limits_default(&lim);
    lim.require_fresh_market = true;
    lim.max_quote_age_ns = 500000000LL; /* 0.5s */
    rms_set_limits(r, &lim);

    ts_order o = make_order("BTCUSDT", TS_SIDE_BUY, 1.0, 100.0);

    /* no data at all */
    ASSERT_EQ_I(rms_check_order(r, &o).code, TS_RISK_NO_MARKET_DATA);

    /* fresh quote */
    rms_on_quote(r, "BTCUSDT", 99.0, 101.0, 100.0, g_vc.now);
    ASSERT(rms_check_order(r, &o).approved);

    /* stale quote */
    g_vc.now += 600000000LL;
    ASSERT_EQ_I(rms_check_order(r, &o).code, TS_RISK_STALE_DATA);

    rms_destroy(r);
}

static void test_pnl_and_loss_limit(void)
{
    setup();
    rms_t *r = make_rms();
    ts_risk_limits lim;
    ts_risk_limits_default(&lim);
    lim.max_order_qty = 1e9;
    lim.max_order_notional = 1e18;
    lim.max_position_qty = 1e9;
    lim.daily_loss_limit = 50.0;
    rms_set_limits(r, &lim);

    /* buy 10 @ 100, mark at 100 */
    ts_execution buy;
    memset(&buy, 0, sizeof(buy));
    ts_copy_str(buy.client_id, sizeof(buy.client_id), "b1");
    ts_copy_str(buy.instrument, sizeof(buy.instrument), "BTCUSDT");
    buy.side = TS_SIDE_BUY;
    buy.qty = 10.0;
    buy.price = 100.0;
    buy.ts_ns = g_vc.now;
    ASSERT(rms_on_execution(r, &buy) == TS_OK);
    rms_on_quote(r, "BTCUSDT", 99.0, 101.0, 100.0, g_vc.now);
    ASSERT_EQ_D(rms_total_pnl(r), 0.0, 1e-9);

    /* price drops to 90 -> unrealized -100 > loss limit 50 */
    rms_on_quote(r, "BTCUSDT", 89.0, 91.0, 90.0, g_vc.now);
    ASSERT_EQ_D(rms_unrealized_pnl(r), -100.0, 1e-6);

    ts_order o = make_order("BTCUSDT", TS_SIDE_SELL, 1.0, 90.0);
    ASSERT_EQ_I(rms_check_order(r, &o).code, TS_RISK_LOSS_LIMIT);

    /* close the position at 90 -> realized -100, still over limit */
    ts_execution sell;
    memset(&sell, 0, sizeof(sell));
    ts_copy_str(sell.client_id, sizeof(sell.client_id), "s1");
    ts_copy_str(sell.instrument, sizeof(sell.instrument), "BTCUSDT");
    sell.side = TS_SIDE_SELL;
    sell.qty = 10.0;
    sell.price = 90.0;
    sell.ts_ns = g_vc.now;
    ASSERT(rms_on_execution(r, &sell) == TS_OK);
    ASSERT_EQ_D(rms_realized_pnl(r), -100.0, 1e-6);
    ASSERT_EQ_I(rms_position(r, "BTCUSDT")->qty, 0.0);

    /* new orders still blocked by loss limit */
    ts_order o2 = make_order("BTCUSDT", TS_SIDE_BUY, 1.0, 90.0);
    ts_copy_str(o2.client_id, sizeof(o2.client_id), "c9");
    ASSERT_EQ_I(rms_check_order(r, &o2).code, TS_RISK_LOSS_LIMIT);

    rms_destroy(r);
}

static void test_invalid_order_rejected(void)
{
    setup();
    rms_t *r = make_rms();
    ts_order o = make_order("BTCUSDT", TS_SIDE_BUY, 0.0, 100.0);
    ASSERT_EQ_I(rms_check_order(r, &o).code, TS_RISK_INVALID_ORDER);
    rms_destroy(r);
}

const test_case rms_tests[] = {
    {"defaults_approve", test_defaults_approve},
    {"kill_switch", test_kill_switch},
    {"qty_and_notional_limits", test_qty_and_notional_limits},
    {"position_limit", test_position_limit},
    {"rate_limit", test_rate_limit},
    {"stale_and_missing_market_data", test_stale_and_missing_market_data},
    {"pnl_and_loss_limit", test_pnl_and_loss_limit},
    {"invalid_order_rejected", test_invalid_order_rejected},
    {NULL, NULL},
};
