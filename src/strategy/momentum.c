#include "trading/strategy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct momentum_impl {
    momentum_config cfg;
};

static ts_status momentum_on_event(strategy_t *s, const ts_mkt_event *ev)
{
    struct momentum_impl *mi = s->impl;
    momentum_config *c = &mi->cfg;

    if (strcmp(ev->instrument, c->instrument) != 0) {
        return TS_OK;
    }
    double mid;
    if (ev->type == TS_MKT_QUOTE) {
        mid = (ev->bid + ev->ask) * 0.5;
    } else {
        mid = ev->last;
    }
    if (mid <= 0.0) {
        return TS_OK;
    }

    if (!c->primed) {
        c->last_mid = mid;
        c->primed = true;
        return TS_OK;
    }

    double move = mid - c->last_mid;
    if (move > c->move_threshold) {
        c->direction = -1; /* was up -> sell */
    } else if (move < -c->move_threshold) {
        c->direction = 1;  /* was down -> buy */
    } else {
        c->last_mid = mid;
        return TS_OK;
    }
    c->last_mid = mid;

    ts_order_request req;
    memset(&req, 0, sizeof(req));
    snprintf(req.client_id, sizeof(req.client_id), "%s-%06llu",
             c->strategy_name, (unsigned long long)++c->seq);
    ts_copy_str(req.instrument, sizeof(req.instrument), c->instrument);
    ts_copy_str(req.strategy, sizeof(req.strategy), c->strategy_name);
    req.side = c->direction > 0 ? TS_SIDE_BUY : TS_SIDE_SELL;
    req.type = TS_ORD_MARKET;
    req.tif = TS_TIF_IOC;
    req.qty = c->qty;
    req.price = 0.0;

    if (c->sink) {
        c->sink(c->sink_ctx, &req);
    }
    return TS_OK;
}

static void momentum_destroy(strategy_t *s)
{
    free(s->impl);
    free(s);
}

strategy_t *strategy_momentum_create(const momentum_config *cfg)
{
    if (!cfg || !cfg->instrument[0] || !cfg->sink) {
        return NULL;
    }
    strategy_t *s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    struct momentum_impl *mi = calloc(1, sizeof(*mi));
    if (!mi) {
        free(s);
        return NULL;
    }
    mi->cfg = *cfg;
    if (mi->cfg.qty <= 0.0) {
        mi->cfg.qty = 1.0;
    }
    if (mi->cfg.move_threshold <= 0.0) {
        mi->cfg.move_threshold = 0.5;
    }
    if (mi->cfg.strategy_name[0] == '\0') {
        ts_copy_str(mi->cfg.strategy_name, sizeof(mi->cfg.strategy_name),
                    "momentum");
    }
    mi->cfg.direction = 1;
    s->name = "momentum";
    s->on_event = momentum_on_event;
    s->destroy = momentum_destroy;
    s->impl = mi;
    return s;
}
