#ifndef TRADING_EVENTS_H
#define TRADING_EVENTS_H

#include "trading/core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TS_MAX_CODE 24
#define TS_MAX_ID 40
#define TS_MAX_NAME 16
#define TS_MAX_REASON 128

typedef enum {
    TS_ASSET_EQUITY = 0,
    TS_ASSET_CRYPTO,
    TS_ASSET_FUTURE,
    TS_ASSET_OPTION,
    TS_ASSET_FX,
} ts_asset_class;

const char *ts_asset_class_str(ts_asset_class c);

typedef struct {
    char code[TS_MAX_CODE];      /* e.g. "BTCUSDT", "NSE:RELIANCE" */
    char venue[TS_MAX_NAME];     /* venue / exchange identifier */
    ts_asset_class asset;
    char currency[8];            /* quote currency, e.g. "USD", "INR" */
    char settlement_currency[8];
    double tick_size;
    double lot_size;
    double contract_multiplier;
    bool tradable;
} ts_instrument;

ts_status ts_instrument_validate(const ts_instrument *inst);

/* ---------------- Market data ---------------- */

typedef enum {
    TS_MKT_QUOTE = 0,
    TS_MKT_TRADE,
    TS_MKT_SNAPSHOT,
    TS_MKT_BOOK,   /* top-N book snapshot; see book_depth fields */
} ts_mkt_type;

#define TS_BOOK_DEPTH 8

typedef struct {
    ts_mkt_type type;
    char instrument[TS_MAX_CODE];
    double bid;
    double ask;
    double last;
    double bid_qty;
    double ask_qty;
    double last_qty;
    ts_ts_ns ts_ns;   /* exchange/feed timestamp */
    uint64_t seq;     /* feed sequence number */
    /* L2 top-of-book levels, best first. Only meaningful when
     * type == TS_MKT_BOOK or book_depth > 0. level i: index i. */
    uint8_t book_depth;
    double bid_px[TS_BOOK_DEPTH];
    double bid_qty_lv[TS_BOOK_DEPTH];
    double ask_px[TS_BOOK_DEPTH];
    double ask_qty_lv[TS_BOOK_DEPTH];
} ts_mkt_event;

/* ---------------- Orders ---------------- */

typedef enum {
    TS_SIDE_BUY = 1,
    TS_SIDE_SELL = -1,
} ts_side;

const char *ts_side_str(ts_side s);

typedef enum {
    TS_ORD_LIMIT = 0,
    TS_ORD_MARKET,
    TS_ORD_STOP_LIMIT,
} ts_order_type;

typedef enum {
    TS_TIF_GTC = 0,
    TS_TIF_IOC,
    TS_TIF_FOK,
    TS_TIF_DAY,
} ts_tif;

const char *ts_order_type_str(ts_order_type t);
const char *ts_tif_str(ts_tif t);

/* Order lifecycle states. Terminal: FILLED, CANCELED, REJECTED, EXPIRED. */
typedef enum {
    TS_ORD_NEW = 0,          /* accepted by OMS, awaiting risk check */
    TS_ORD_HELD,             /* held by risk pending review */
    TS_ORD_RISK_APPROVED,    /* passed risk, dispatched to EMS */
    TS_ORD_WORKING,          /* acknowledged by venue, live on book */
    TS_ORD_PARTIAL,          /* partially filled, remainder working */
    TS_ORD_FILLED,           /* terminal: fully filled */
    TS_ORD_CANCELED,         /* terminal: canceled */
    TS_ORD_REJECTED,         /* terminal: rejected (risk or venue) */
    TS_ORD_EXPIRED,          /* terminal: expired (TIF/session) */
    TS_ORD_UNKNOWN,          /* non-terminal: needs reconciliation */
} ts_order_state;

const char *ts_order_state_str(ts_order_state s);
bool ts_order_state_is_terminal(ts_order_state s);

typedef struct {
    char client_id[TS_MAX_ID];        /* idempotency key chosen by strategy */
    char venue_order_id[TS_MAX_ID];   /* assigned by venue/EMS */
    char instrument[TS_MAX_CODE];
    char strategy[TS_MAX_NAME];
    ts_side side;
    ts_order_type type;
    ts_tif tif;
    double qty;                       /* total order quantity */
    double price;                     /* limit price; ignored for MARKET */
    double filled_qty;
    double avg_fill_price;
    ts_order_state state;
    char reject_reason[TS_MAX_REASON];
    ts_ts_ns created_ns;
    ts_ts_ns updated_ns;
} ts_order;

typedef struct {
    char client_id[TS_MAX_ID];
    char venue_order_id[TS_MAX_ID];
    char instrument[TS_MAX_CODE];
    char strategy[TS_MAX_NAME];
    ts_side side;
    ts_order_type type;
    ts_tif tif;
    double qty;
    double price;
} ts_order_request;

ts_status ts_order_request_validate(const ts_order_request *req);

void ts_copy_str(char *dst, size_t dstsz, const char *src);

/* ---------------- Executions ---------------- */

typedef struct {
    char client_id[TS_MAX_ID];
    char venue_order_id[TS_MAX_ID];
    char venue_exec_id[TS_MAX_ID];
    char instrument[TS_MAX_CODE];
    ts_side side;
    double qty;
    double price;
    double fee;
    ts_ts_ns ts_ns;
} ts_execution;

/* ---------------- Venue events (EMS boundary) ---------------- */

typedef enum {
    TS_VEV_ACK = 0,        /* order accepted by venue, now working */
    TS_VEV_EXEC,           /* (partial) execution; exec field populated */
    TS_VEV_REJECT,         /* venue rejected the order */
    TS_VEV_CANCEL_ACK,     /* cancel confirmed */
    TS_VEV_AMENDED,        /* order amended/replace acknowledged by venue */
    TS_VEV_EXPIRED,        /* order expired (e.g. IOC remainder, session end) */
    TS_VEV_UNKNOWN,        /* venue state uncertain; needs reconciliation */
} ts_venue_event_type;

typedef struct {
    ts_venue_event_type type;
    char client_id[TS_MAX_ID];
    char venue_order_id[TS_MAX_ID];
    char instrument[TS_MAX_CODE];
    char reason[TS_MAX_REASON];
    ts_execution exec;
    ts_ts_ns ts_ns;
} ts_venue_event;

#ifdef __cplusplus
}
#endif

#endif /* TRADING_EVENTS_H */
