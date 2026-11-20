#ifndef TRADING_STRATEGY_H
#define TRADING_STRATEGY_H

#include "trading/events.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Strategy interface. Strategies consume normalized market events and emit
 * order requests through a sink callback. They never touch venues, OMS
 * internals, or risk state directly. */

typedef void (*ts_order_sink)(void *ctx, const ts_order_request *req);

typedef struct strategy strategy_t;

typedef ts_status (*strategy_on_event_fn)(strategy_t *s,
                                          const ts_mkt_event *ev);

struct strategy {
    const char *name;
    strategy_on_event_fn on_event;
    void (*destroy)(strategy_t *s);
    void *impl;
};

/* Reference strategy: simple alternating momentum. On each qualifying quote
 * update it emits a small market order alternating buy/sell, used to
 * exercise the full OMS->RMS->EMS path deterministically. */
typedef struct {
    char instrument[TS_MAX_CODE];
    char strategy_name[TS_MAX_NAME];
    double qty;
    double move_threshold;   /* abs mid-price move required to trigger */
    int direction;           /* +1 buy next, -1 sell next */
    double last_mid;
    bool primed;
    uint64_t seq;
    ts_order_sink sink;
    void *sink_ctx;
} momentum_config;

strategy_t *strategy_momentum_create(const momentum_config *cfg);

/* Book-imbalance strategy: trades top-of-book depth imbalance on
 * TS_MKT_BOOK events. imbalance = (bid_qty - ask_qty) / total;
 * > +threshold -> BUY, < -threshold -> SELL (IOC market orders). */
typedef struct {
    char instrument[TS_MAX_CODE];
    char strategy_name[TS_MAX_NAME];
    double qty;
    double imbalance_threshold; /* (0,1]; default 0.3 */
    ts_order_sink sink;
    void *sink_ctx;
} bookimb_config;

strategy_t *strategy_book_imbalance_create(const bookimb_config *cfg);

/* Replay adapter: reads normalized market events from a simple text file.
 * Line formats:
 *   QUOTE <instrument> <bid> <ask> [last] [ts_ns] [seq]
 *   TRADE <instrument> <price> <qty> [ts_ns] [seq]
 * Blank lines and '#' comments are ignored. */

typedef struct replay replay_t;

replay_t *replay_open(const char *path);
void replay_close(replay_t *r);
/* Returns TS_OK and fills ev, TS_ERR_NOT_FOUND at EOF. */
ts_status replay_next(replay_t *r, ts_mkt_event *ev);
size_t replay_lineno(const replay_t *r);

#ifdef __cplusplus
}
#endif

#endif /* TRADING_STRATEGY_H */
