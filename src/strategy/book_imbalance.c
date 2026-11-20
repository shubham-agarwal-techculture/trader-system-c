#include "trading/strategy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Book-imbalance strategy: consumes TS_MKT_BOOK (or QUOTE with
 * book_depth > 0) events and emits IOC orders when top-of-book depth
 * imbalance exceeds a threshold.
 *
 * imbalance = (bid_qty_total - ask_qty_total) / (bid_qty_total + ask_qty_total)
 *   > +threshold -> BUY  (bids stacked, expect up-move)
 *   < -threshold -> SELL (asks stacked, expect down-move)
 */

struct bookimb_impl {
    bookimb_config cfg;
    uint64_t seq;
    bool primed;
    int last_dir;
};

static ts_status bookimb_on_event(strategy_t *s, const ts_mkt_event *ev)
{
    struct bookimb_impl *bi = s->impl;
    bookimb_config *c = &bi->cfg;

    if (strcmp(ev->instrument, c->instrument) != 0) {
        return TS_OK;
    }
    if (ev->book_depth == 0) {
        return TS_OK; /* need L2 data */
    }

    double bq = 0.0, aq = 0.0;
    for (int i = 0; i < ev->book_depth; ++i) {
        bq += ev->bid_qty_lv[i];
        aq += ev->ask_qty_lv[i];
    }
    double total = bq + aq;
    if (total <= 0.0) {
        return TS_OK;
    }
    double imb = (bq - aq) / total;

    int dir = 0;
    if (imb > c->imbalance_threshold) {
        dir = 1;
    } else if (imb < -c->imbalance_threshold) {
        dir = -1;
    }
    /* avoid spamming same direction on every book update */
    if (dir == 0 || dir == bi->last_dir) {
        bi->last_dir = dir;
        return TS_OK;
    }
    bi->last_dir = dir;
    bi->primed = true;

    ts_order_request req;
    memset(&req, 0, sizeof(req));
    snprintf(req.client_id, sizeof(req.client_id), "%s-%06llu",
             c->strategy_name, (unsigned long long)++bi->seq);
    ts_copy_str(req.instrument, sizeof(req.instrument), c->instrument);
    ts_copy_str(req.strategy, sizeof(req.strategy), c->strategy_name);
    req.side = dir > 0 ? TS_SIDE_BUY : TS_SIDE_SELL;
    req.type = TS_ORD_MARKET;
    req.tif = TS_TIF_IOC;
    req.qty = c->qty;
    req.price = 0.0;

    if (c->sink) {
        c->sink(c->sink_ctx, &req);
    }
    return TS_OK;
}

static void bookimb_destroy(strategy_t *s)
{
    free(s->impl);
    free(s);
}

strategy_t *strategy_book_imbalance_create(const void *cfg_void)
{
    const bookimb_config *cfg = cfg_void;
    if (!cfg || !cfg->instrument[0] || !cfg->sink) {
        return NULL;
    }
    strategy_t *s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    struct bookimb_impl *bi = calloc(1, sizeof(*bi));
    if (!bi) {
        free(s);
        return NULL;
    }
    bi->cfg = *cfg;
    if (bi->cfg.qty <= 0.0) {
        bi->cfg.qty = 1.0;
    }
    if (bi->cfg.imbalance_threshold <= 0.0 ||
        bi->cfg.imbalance_threshold > 1.0) {
        bi->cfg.imbalance_threshold = 0.3;
    }
    if (bi->cfg.strategy_name[0] == '\0') {
        ts_copy_str(bi->cfg.strategy_name, sizeof(bi->cfg.strategy_name),
                    "book_imbalance");
    }
    s->name = "book_imbalance";
    s->on_event = bookimb_on_event;
    s->destroy = bookimb_destroy;
    s->impl = bi;
    return s;
}
