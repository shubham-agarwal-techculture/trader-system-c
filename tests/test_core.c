#include "testutil.h"

#include "trading/core.h"
#include "trading/events.h"

#include <string.h>

static void test_status_strings(void)
{
    ASSERT(ts_status_str(TS_OK) != NULL);
    ASSERT(ts_status_str(TS_ERR_IO) != NULL);
}

static void test_vclock(void)
{
    ts_vclock vc = {1000};
    ts_clock c = ts_clock_virtual(&vc);
    ASSERT_EQ_I(c.now_ns(c.ctx), 1000);
    ts_vclock_advance(&vc, 500);
    ASSERT_EQ_I(c.now_ns(c.ctx), 1500);
    ts_vclock_set(&vc, 42);
    ASSERT_EQ_I(c.now_ns(c.ctx), 42);
}

static void test_instrument_validate(void)
{
    ts_instrument inst;
    memset(&inst, 0, sizeof(inst));
    ts_copy_str(inst.code, sizeof(inst.code), "BTCUSDT");
    ts_copy_str(inst.venue, sizeof(inst.venue), "mock");
    inst.asset = TS_ASSET_CRYPTO;
    ts_copy_str(inst.currency, sizeof(inst.currency), "USDT");
    inst.tick_size = 0.01;
    inst.lot_size = 0.001;
    inst.contract_multiplier = 1.0;
    inst.tradable = true;
    ASSERT(ts_instrument_validate(&inst) == TS_OK);

    inst.tick_size = 0.0;
    ASSERT(ts_instrument_validate(&inst) == TS_ERR_INVALID_ARG);
}

static void test_order_request_validate(void)
{
    ts_order_request req;
    memset(&req, 0, sizeof(req));
    ts_copy_str(req.client_id, sizeof(req.client_id), "c1");
    ts_copy_str(req.instrument, sizeof(req.instrument), "BTCUSDT");
    req.side = TS_SIDE_BUY;
    req.type = TS_ORD_LIMIT;
    req.tif = TS_TIF_GTC;
    req.qty = 1.0;
    req.price = 100.0;
    ASSERT(ts_order_request_validate(&req) == TS_OK);

    req.qty = 0.0;
    ASSERT(ts_order_request_validate(&req) == TS_ERR_INVALID_ARG);
    req.qty = 1.0;
    req.price = -5.0;
    ASSERT(ts_order_request_validate(&req) == TS_ERR_INVALID_ARG);
    req.price = 100.0;
    req.side = (ts_side)7;
    ASSERT(ts_order_request_validate(&req) == TS_ERR_INVALID_ARG);
}

static void test_state_helpers(void)
{
    ASSERT(ts_order_state_is_terminal(TS_ORD_FILLED));
    ASSERT(ts_order_state_is_terminal(TS_ORD_REJECTED));
    ASSERT(!ts_order_state_is_terminal(TS_ORD_WORKING));
    ASSERT(ts_order_state_is_terminal(TS_ORD_EXPIRED));
    ASSERT(!ts_order_state_is_terminal(TS_ORD_UNKNOWN));
}

const test_case core_tests[] = {
    {"status_strings", test_status_strings},
    {"vclock", test_vclock},
    {"instrument_validate", test_instrument_validate},
    {"order_request_validate", test_order_request_validate},
    {"state_helpers", test_state_helpers},
    {NULL, NULL},
};
