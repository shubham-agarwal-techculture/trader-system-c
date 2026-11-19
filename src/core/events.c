#include "trading/events.h"

#include <stdio.h>
#include <string.h>

const char *ts_asset_class_str(ts_asset_class c)
{
    switch (c) {
    case TS_ASSET_EQUITY:  return "EQUITY";
    case TS_ASSET_CRYPTO:  return "CRYPTO";
    case TS_ASSET_FUTURE:  return "FUTURE";
    case TS_ASSET_OPTION:  return "OPTION";
    case TS_ASSET_FX:      return "FX";
    }
    return "UNKNOWN";
}

const char *ts_side_str(ts_side s)
{
    return s == TS_SIDE_BUY ? "BUY" : "SELL";
}

const char *ts_order_type_str(ts_order_type t)
{
    switch (t) {
    case TS_ORD_LIMIT:      return "LIMIT";
    case TS_ORD_MARKET:     return "MARKET";
    case TS_ORD_STOP_LIMIT: return "STOP_LIMIT";
    }
    return "UNKNOWN";
}

const char *ts_tif_str(ts_tif t)
{
    switch (t) {
    case TS_TIF_GTC: return "GTC";
    case TS_TIF_IOC: return "IOC";
    case TS_TIF_FOK: return "FOK";
    case TS_TIF_DAY: return "DAY";
    }
    return "UNKNOWN";
}

const char *ts_order_state_str(ts_order_state s)
{
    switch (s) {
    case TS_ORD_NEW:           return "NEW";
    case TS_ORD_HELD:          return "HELD";
    case TS_ORD_RISK_APPROVED: return "RISK_APPROVED";
    case TS_ORD_WORKING:       return "WORKING";
    case TS_ORD_PARTIAL:       return "PARTIAL";
    case TS_ORD_FILLED:        return "FILLED";
    case TS_ORD_CANCELED:      return "CANCELED";
    case TS_ORD_REJECTED:      return "REJECTED";
    case TS_ORD_EXPIRED:       return "EXPIRED";
    case TS_ORD_UNKNOWN:       return "UNKNOWN";
    }
    return "?";
}

bool ts_order_state_is_terminal(ts_order_state s)
{
    return s == TS_ORD_FILLED || s == TS_ORD_CANCELED ||
           s == TS_ORD_REJECTED || s == TS_ORD_EXPIRED;
}

ts_status ts_instrument_validate(const ts_instrument *inst)
{
    if (!inst || inst->code[0] == '\0') {
        return TS_ERR_INVALID_ARG;
    }
    if (inst->tick_size <= 0.0 || inst->lot_size <= 0.0 ||
        inst->contract_multiplier <= 0.0) {
        return TS_ERR_INVALID_ARG;
    }
    return TS_OK;
}

ts_status ts_order_request_validate(const ts_order_request *req)
{
    if (!req) {
        return TS_ERR_INVALID_ARG;
    }
    if (req->client_id[0] == '\0' || req->instrument[0] == '\0') {
        return TS_ERR_INVALID_ARG;
    }
    if (req->side != TS_SIDE_BUY && req->side != TS_SIDE_SELL) {
        return TS_ERR_INVALID_ARG;
    }
    if (req->qty <= 0.0 || req->qty != req->qty /* NaN */) {
        return TS_ERR_INVALID_ARG;
    }
    if (req->type == TS_ORD_LIMIT || req->type == TS_ORD_STOP_LIMIT) {
        if (req->price <= 0.0 || req->price != req->price) {
            return TS_ERR_INVALID_ARG;
        }
    }
    return TS_OK;
}

/* Bounded copy helpers used across module boundaries. */
void ts_copy_str(char *dst, size_t dstsz, const char *src)
{
    if (dstsz == 0) {
        return;
    }
    snprintf(dst, dstsz, "%s", src ? src : "");
}
