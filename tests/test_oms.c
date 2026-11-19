#include "testutil.h"

#include "trading/oms.h"

#include <string.h>

static ts_vclock g_vc;
static ts_clock g_clock;

static void setup(void)
{
    g_vc.now = 1000000;
    g_clock = ts_clock_virtual(&g_vc);
}

static ts_order_request make_req(const char *id)
{
    ts_order_request req;
    memset(&req, 0, sizeof(req));
    ts_copy_str(req.client_id, sizeof(req.client_id), id);
    ts_copy_str(req.instrument, sizeof(req.instrument), "BTCUSDT");
    ts_copy_str(req.strategy, sizeof(req.strategy), "test");
    req.side = TS_SIDE_BUY;
    req.type = TS_ORD_LIMIT;
    req.tif = TS_TIF_GTC;
    req.qty = 1.0;
    req.price = 100.0;
    return req;
}

static void test_submit_and_find(void)
{
    setup();
    oms_config cfg = {.clock = g_clock};
    oms_t *o = oms_create(&cfg, 16);
    ASSERT(o != NULL);

    ts_order_request req = make_req("c1");
    ts_order ord;
    ASSERT(oms_submit(o, &req, &ord) == TS_OK);
    ASSERT_EQ_I(ord.state, TS_ORD_NEW);
    ASSERT_EQ_I(oms_count(o), 1);

    const ts_order *found = oms_find(o, "c1");
    ASSERT(found != NULL);
    ASSERT_STR(found->instrument, "BTCUSDT");

    oms_destroy(o);
}

static void test_idempotent_resubmit(void)
{
    setup();
    oms_config cfg = {.clock = g_clock};
    oms_t *o = oms_create(&cfg, 16);

    ts_order_request req = make_req("dup");
    ts_order a, b;
    ASSERT(oms_submit(o, &req, &a) == TS_OK);
    ASSERT(oms_submit(o, &req, &b) == TS_OK);
    ASSERT_EQ_I(oms_count(o), 1);
    ASSERT_STR(b.client_id, "dup");

    oms_destroy(o);
}

static void test_capacity_full(void)
{
    setup();
    oms_config cfg = {.clock = g_clock};
    oms_t *o = oms_create(&cfg, 2);

    ts_order_request r1 = make_req("a");
    ts_order_request r2 = make_req("b");
    ts_order_request r3 = make_req("c");
    ASSERT(oms_submit(o, &r1, NULL) == TS_OK);
    ASSERT(oms_submit(o, &r2, NULL) == TS_OK);
    ASSERT(oms_submit(o, &r3, NULL) == TS_ERR_FULL);

    oms_destroy(o);
}

static void test_lifecycle_to_filled(void)
{
    setup();
    oms_config cfg = {.clock = g_clock};
    oms_t *o = oms_create(&cfg, 16);

    ts_order_request req = make_req("life");
    ASSERT(oms_submit(o, &req, NULL) == TS_OK);

    ASSERT(oms_mark_risk(o, "life", true, "OK", "") == TS_OK);
    ASSERT_EQ_I(oms_find(o, "life")->state, TS_ORD_RISK_APPROVED);

    ASSERT(oms_on_ack(o, "life", "V1") == TS_OK);
    ASSERT_EQ_I(oms_find(o, "life")->state, TS_ORD_WORKING);
    ASSERT_STR(oms_find(o, "life")->venue_order_id, "V1");

    ts_execution ex;
    memset(&ex, 0, sizeof(ex));
    ts_copy_str(ex.client_id, sizeof(ex.client_id), "life");
    ts_copy_str(ex.instrument, sizeof(ex.instrument), "BTCUSDT");
    ex.side = TS_SIDE_BUY;
    ex.qty = 0.4;
    ex.price = 100.5;
    ex.ts_ns = g_vc.now;
    ASSERT(oms_on_execution(o, &ex) == TS_OK);
    ASSERT_EQ_I(oms_find(o, "life")->state, TS_ORD_PARTIAL);
    ASSERT_EQ_D(oms_find(o, "life")->filled_qty, 0.4, 1e-9);

    ex.qty = 0.6;
    ex.price = 100.6;
    ASSERT(oms_on_execution(o, &ex) == TS_OK);
    ASSERT_EQ_I(oms_find(o, "life")->state, TS_ORD_FILLED);
    ASSERT_EQ_D(oms_find(o, "life")->filled_qty, 1.0, 1e-9);
    /* weighted avg: (0.4*100.5 + 0.6*100.6)/1.0 = 100.56 */
    ASSERT_EQ_D(oms_find(o, "life")->avg_fill_price, 100.56, 1e-6);

    oms_destroy(o);
}

static void test_reject_and_cancel(void)
{
    setup();
    oms_config cfg = {.clock = g_clock};
    oms_t *o = oms_create(&cfg, 16);

    ts_order_request r1 = make_req("rej");
    ts_order_request r2 = make_req("cxl");
    ASSERT(oms_submit(o, &r1, NULL) == TS_OK);
    ASSERT(oms_submit(o, &r2, NULL) == TS_OK);

    /* risk reject */
    ASSERT(oms_mark_risk(o, "rej", false, "QTY_LIMIT", "too big") == TS_OK);
    ASSERT_EQ_I(oms_find(o, "rej")->state, TS_ORD_REJECTED);

    /* approve + ack + cancel */
    ASSERT(oms_mark_risk(o, "cxl", true, "OK", "") == TS_OK);
    ASSERT(oms_on_ack(o, "cxl", "V2") == TS_OK);
    ASSERT(oms_on_cancel(o, "cxl") == TS_OK);
    ASSERT_EQ_I(oms_find(o, "cxl")->state, TS_ORD_CANCELED);

    /* terminal states reject further transitions */
    ASSERT(oms_on_ack(o, "cxl", "V3") == TS_ERR_BAD_STATE);

    /* unknown lookup */
    ASSERT(oms_find(o, "nope") == NULL);
    ASSERT(oms_mark_risk(o, "nope", true, "OK", "") == TS_ERR_NOT_FOUND);

    oms_destroy(o);
}

static void test_invalid_submit(void)
{
    setup();
    oms_config cfg = {.clock = g_clock};
    oms_t *o = oms_create(&cfg, 16);

    ts_order_request req = make_req("bad");
    req.qty = -1.0;
    ASSERT(oms_submit(o, &req, NULL) == TS_ERR_INVALID_ARG);
    ASSERT(oms_submit(o, NULL, NULL) == TS_ERR_INVALID_ARG);

    oms_destroy(o);
}

static void test_open_count(void)
{
    setup();
    oms_config cfg = {.clock = g_clock};
    oms_t *o = oms_create(&cfg, 16);

    ts_order_request r1 = make_req("o1");
    ts_order_request r2 = make_req("o2");
    ASSERT(oms_submit(o, &r1, NULL) == TS_OK);
    ASSERT(oms_submit(o, &r2, NULL) == TS_OK);
    ASSERT_EQ_I(oms_open_count(o), 2);
    ASSERT(oms_mark_risk(o, "o1", false, "X", "") == TS_OK);
    ASSERT_EQ_I(oms_open_count(o), 1);

    oms_destroy(o);
}

const test_case oms_tests[] = {
    {"submit_and_find", test_submit_and_find},
    {"idempotent_resubmit", test_idempotent_resubmit},
    {"capacity_full", test_capacity_full},
    {"lifecycle_to_filled", test_lifecycle_to_filled},
    {"reject_and_cancel", test_reject_and_cancel},
    {"invalid_submit", test_invalid_submit},
    {"open_count", test_open_count},
    {NULL, NULL},
};
