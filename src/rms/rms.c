#include "trading/rms.h"

#include "trading/log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rms {
    ts_risk_limits lim;
    bool killed;
    ts_rms_position pos[TS_RMS_MAX_POSITIONS];
    size_t pos_count;
    double realized_pnl;
    /* Order-rate sliding window of submission timestamps (ns). */
    ts_ts_ns rate_ts[TS_RMS_RATE_WINDOW];
    size_t rate_head;
    size_t rate_count;
    journal_t *journal;
    ts_metrics *metrics;
    ts_clock clock;
};

void ts_risk_limits_default(ts_risk_limits *lim)
{
    memset(lim, 0, sizeof(*lim));
    lim->max_order_qty = 1000.0;
    lim->max_order_notional = 100000.0;
    lim->max_position_qty = 5000.0;
    lim->daily_loss_limit = 10000.0;
    lim->max_order_rate = 0.0; /* unlimited until configured */
    lim->require_fresh_market = false;
    lim->max_quote_age_ns = 0;
}

const char *ts_risk_code_str(ts_risk_code c)
{
    switch (c) {
    case TS_RISK_OK: return "OK";
    case TS_RISK_KILL_SWITCH: return "KILL_SWITCH";
    case TS_RISK_INVALID_ORDER: return "INVALID_ORDER";
    case TS_RISK_RATE_LIMIT: return "RATE_LIMIT";
    case TS_RISK_QTY_LIMIT: return "QTY_LIMIT";
    case TS_RISK_NOTIONAL_LIMIT: return "NOTIONAL_LIMIT";
    case TS_RISK_POSITION_LIMIT: return "POSITION_LIMIT";
    case TS_RISK_LOSS_LIMIT: return "LOSS_LIMIT";
    case TS_RISK_STALE_DATA: return "STALE_DATA";
    case TS_RISK_NO_MARKET_DATA: return "NO_MARKET_DATA";
    case TS_RISK_DUPLICATE: return "DUPLICATE";
    }
    return "?";
}

static const char *code_rule(ts_risk_code c)
{
    switch (c) {
    case TS_RISK_KILL_SWITCH: return "KILL_SWITCH";
    case TS_RISK_INVALID_ORDER: return "ORDER_VALIDATION";
    case TS_RISK_RATE_LIMIT: return "ORDER_RATE";
    case TS_RISK_QTY_LIMIT: return "MAX_ORDER_QTY";
    case TS_RISK_NOTIONAL_LIMIT: return "MAX_NOTIONAL";
    case TS_RISK_POSITION_LIMIT: return "MAX_POSITION";
    case TS_RISK_LOSS_LIMIT: return "DAILY_LOSS";
    case TS_RISK_STALE_DATA: return "STALE_MARKET_DATA";
    case TS_RISK_NO_MARKET_DATA: return "MARKET_DATA_REQUIRED";
    case TS_RISK_DUPLICATE: return "DUPLICATE_ORDER";
    default: return "UNKNOWN";
    }
}

static ts_ts_ns now_ns(const rms_t *r)
{
    return r->clock.now_ns(r->clock.ctx);
}

rms_t *rms_create(const rms_config *cfg)
{
    if (!cfg) {
        return NULL;
    }
    rms_t *r = calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    ts_risk_limits_default(&r->lim);
    r->journal = cfg->journal;
    r->metrics = cfg->metrics;
    r->clock = cfg->clock;
    return r;
}

void rms_destroy(rms_t *r)
{
    free(r);
}

void rms_set_limits(rms_t *r, const ts_risk_limits *lim)
{
    if (r && lim) {
        r->lim = *lim;
    }
}

const ts_risk_limits *rms_get_limits(const rms_t *r)
{
    return r ? &r->lim : NULL;
}

static void journal_kill(rms_t *r, bool killed)
{
    r->killed = killed;
    if (r->journal) {
        journal_append_kill(r->journal, now_ns(r), killed);
    }
    ts_log(TS_LOG_WARN, "rms: kill switch %s", killed ? "ENGAGED" : "DISENGAGED");
}

void rms_kill(rms_t *r)
{
    if (r && !r->killed) {
        journal_kill(r, true);
    }
}

rms_t *rms_resume(rms_t *r)
{
    if (r && r->killed) {
        journal_kill(r, false);
    }
    return r;
}

bool rms_is_killed(const rms_t *r)
{
    return r && r->killed;
}

static ts_rms_position *find_pos(rms_t *r, const char *instrument)
{
    for (size_t i = 0; i < r->pos_count; ++i) {
        if (strcmp(r->pos[i].instrument, instrument) == 0) {
            return &r->pos[i];
        }
    }
    return NULL;
}

static ts_rms_position *get_or_add_pos(rms_t *r, const char *instrument)
{
    ts_rms_position *p = find_pos(r, instrument);
    if (p) {
        return p;
    }
    if (r->pos_count >= TS_RMS_MAX_POSITIONS) {
        return NULL;
    }
    p = &r->pos[r->pos_count++];
    memset(p, 0, sizeof(*p));
    ts_copy_str(p->instrument, sizeof(p->instrument), instrument);
    return p;
}

void rms_on_quote(rms_t *r, const char *instrument, double bid, double ask,
                  double last, ts_ts_ns quote_ns)
{
    if (!r || !instrument) {
        return;
    }
    ts_rms_position *p = get_or_add_pos(r, instrument);
    if (!p) {
        return;
    }
    p->last_price = last != 0.0 ? last : (bid + ask) * 0.5;
    p->last_quote_ns = quote_ns;
}

static double position_after(const ts_rms_position *p, const ts_order *ord)
{
    double signed_qty = (double)ord->side * ord->qty;
    return (p ? p->qty : 0.0) + signed_qty;
}

static double order_notional(const ts_order *ord, const ts_rms_position *p)
{
    double px = ord->price;
    if (ord->type == TS_ORD_MARKET && p && p->last_price > 0.0) {
        px = p->last_price;
    }
    if (px <= 0.0) {
        px = 1.0; /* cannot compute; notional check skipped below */
    }
    return ord->qty * px;
}

static bool rate_limit_ok(rms_t *r, ts_ts_ns now)
{
    if (r->lim.max_order_rate <= 0.0) {
        return true;
    }
    const ts_ts_ns window_ns = 1000000000LL; /* 1 second */
    /* Drop timestamps older than the window. */
    size_t kept = 0;
    for (size_t i = 0; i < r->rate_count; ++i) {
        size_t idx = (r->rate_head + i) % TS_RMS_RATE_WINDOW;
        if (now - r->rate_ts[idx] < window_ns) {
            r->rate_ts[(r->rate_head + kept) % TS_RMS_RATE_WINDOW] =
                r->rate_ts[idx];
            kept++;
        }
    }
    r->rate_count = kept;
    r->rate_head = 0;

    if ((double)r->rate_count >= r->lim.max_order_rate) {
        return false;
    }
    r->rate_ts[(r->rate_head + r->rate_count) % TS_RMS_RATE_WINDOW] = now;
    r->rate_count++;
    return true;
}

static ts_risk_result reject(ts_risk_code code, const char *detail)
{
    ts_risk_result res;
    memset(&res, 0, sizeof(res));
    res.approved = false;
    res.code = code;
    res.rule = code_rule(code);
    snprintf(res.msg, sizeof(res.msg), "%s", detail ? detail : "");
    return res;
}

static ts_risk_result approve(void)
{
    ts_risk_result res;
    memset(&res, 0, sizeof(res));
    res.approved = true;
    res.code = TS_RISK_OK;
    res.rule = "OK";
    return res;
}

ts_risk_result rms_check_order(rms_t *r, const ts_order *ord)
{
    if (!r || !ord) {
        return reject(TS_RISK_INVALID_ORDER, "null argument");
    }
    ts_ts_ns now = now_ns(r);
    ts_rms_position *p = find_pos(r, ord->instrument);

    /* 1. Kill switch — absolute gate. */
    if (r->killed) {
        return reject(TS_RISK_KILL_SWITCH, "kill switch engaged");
    }

    /* 2. Order sanity. */
    if (ord->qty <= 0.0 || ord->side == 0) {
        return reject(TS_RISK_INVALID_ORDER, "non-positive quantity or side");
    }
    if (ord->state != TS_ORD_NEW && ord->state != TS_ORD_RISK_APPROVED) {
        return reject(TS_RISK_INVALID_ORDER, "order not in submittable state");
    }

    /* 3. Rate limit. */
    if (!rate_limit_ok(r, now)) {
        return reject(TS_RISK_RATE_LIMIT, "order rate exceeded");
    }

    /* 4. Quantity limit. */
    if (ord->qty > r->lim.max_order_qty) {
        char buf[TS_MAX_REASON];
        snprintf(buf, sizeof(buf), "qty %.6g > max %.6g",
                 ord->qty, r->lim.max_order_qty);
        return reject(TS_RISK_QTY_LIMIT, buf);
    }

    /* 5. Notional limit (skip when price unknown and no mark). */
    double notional = order_notional(ord, p);
    bool price_known = ord->price > 0.0 ||
                       (ord->type == TS_ORD_MARKET && p && p->last_price > 0.0);
    if (price_known && notional > r->lim.max_order_notional) {
        char buf[TS_MAX_REASON];
        snprintf(buf, sizeof(buf), "notional %.2f > max %.2f",
                 notional, r->lim.max_order_notional);
        return reject(TS_RISK_NOTIONAL_LIMIT, buf);
    }

    /* 6. Position limit. */
    double pos_after = position_after(p, ord);
    if (fabs(pos_after) > r->lim.max_position_qty) {
        char buf[TS_MAX_REASON];
        snprintf(buf, sizeof(buf), "position %.6g > max %.6g",
                 pos_after, r->lim.max_position_qty);
        return reject(TS_RISK_POSITION_LIMIT, buf);
    }

    /* 7. Market data freshness (only when required). */
    if (r->lim.require_fresh_market) {
        if (!p || p->last_quote_ns == 0) {
            return reject(TS_RISK_NO_MARKET_DATA,
                          "no market data for instrument");
        }
        if (now - p->last_quote_ns > r->lim.max_quote_age_ns) {
            return reject(TS_RISK_STALE_DATA, "quote older than max age");
        }
    }

    /* 8. Daily loss limit: realized + unrealized. */
    if (r->lim.daily_loss_limit > 0.0) {
        double unreal = rms_unrealized_pnl(r);
        double total = r->realized_pnl + unreal;
        if (total < -r->lim.daily_loss_limit) {
            char buf[TS_MAX_REASON];
            snprintf(buf, sizeof(buf), "pnl %.2f < -%.2f",
                     total, r->lim.daily_loss_limit);
            return reject(TS_RISK_LOSS_LIMIT, buf);
        }
    }

    return approve();
}

ts_status rms_on_execution(rms_t *r, const ts_execution *exec)
{
    if (!r || !exec) {
        return TS_ERR_INVALID_ARG;
    }
    ts_rms_position *p = get_or_add_pos(r, exec->instrument);
    if (!p) {
        return TS_ERR_FULL;
    }

    double signed_qty = (double)exec->side * exec->qty;
    double old_qty = p->qty;

    if (old_qty == 0.0 || (old_qty > 0) == (signed_qty > 0)) {
        /* Increasing position (or opening). */
        double new_qty = old_qty + signed_qty;
        if (new_qty == 0.0) {
            p->avg_cost = 0.0;
        } else {
            p->avg_cost =
                (p->avg_cost * fabs(old_qty) + exec->price * fabs(signed_qty)) /
                fabs(new_qty);
        }
        p->qty = new_qty;
    } else {
        /* Reducing or flipping. */
        double closed = fabs(signed_qty) < fabs(old_qty)
                            ? fabs(signed_qty)
                            : fabs(old_qty);
        double direction = old_qty > 0 ? 1.0 : -1.0;
        r->realized_pnl += (exec->price - p->avg_cost) * closed * direction;

        double new_qty = old_qty + signed_qty;
        if ((new_qty > 0) != (old_qty > 0) || new_qty == 0.0) {
            /* Flat or flipped: remainder (if any) opens at fill price. */
            double remainder = fabs(new_qty);
            p->avg_cost = remainder > 0.0 ? exec->price : 0.0;
        }
        p->qty = new_qty;
    }
    return TS_OK;
}

ts_status rms_on_account(rms_t *r, const char *currency, double amount)
{
    (void)r; (void)currency; (void)amount;
    /* Reserved for buying-power checks; recorded in journal by caller. */
    return TS_OK;
}

const ts_rms_position *rms_position(rms_t *r, const char *instrument)
{
    return find_pos(r, instrument);
}

double rms_realized_pnl(const rms_t *r)
{
    return r ? r->realized_pnl : 0.0;
}

double rms_unrealized_pnl(rms_t *r)
{
    if (!r) {
        return 0.0;
    }
    double u = 0.0;
    for (size_t i = 0; i < r->pos_count; ++i) {
        ts_rms_position *p = &r->pos[i];
        if (p->qty != 0.0 && p->last_price > 0.0 && p->avg_cost > 0.0) {
            u += (p->last_price - p->avg_cost) * p->qty;
        }
    }
    return u;
}

double rms_total_pnl(rms_t *r)
{
    return rms_realized_pnl(r) + rms_unrealized_pnl(r);
}

size_t rms_position_count(const rms_t *r)
{
    return r ? r->pos_count : 0;
}

ts_status rms_restore_position(rms_t *r, const char *instrument, double qty,
                               double avg_cost)
{
    ts_rms_position *p = get_or_add_pos(r, instrument);
    if (!p) {
        return TS_ERR_FULL;
    }
    p->qty = qty;
    p->avg_cost = avg_cost;
    return TS_OK;
}

ts_status rms_restore_pnl(rms_t *r, double realized_pnl)
{
    if (!r) {
        return TS_ERR_INVALID_ARG;
    }
    r->realized_pnl = realized_pnl;
    return TS_OK;
}
