#ifndef TRADING_KRAKEN_WS_H
#define TRADING_KRAKEN_WS_H

#include "trading/events.h"
#include "trading/venue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Kraken public WebSocket v2 ticker feed (wss://ws.kraken.com/v2).
 * Runs a dedicated reader thread (the ONLY thread in the system —
 * justified: WS recv is blocking) that parses ticker snapshots into
 * normalized ts_mkt_event QUOTE events on an internal bounded queue.
 *
 * venue_kraken_ws_start() spawns the thread; venue_kraken_next_market_event()
 * drains it non-blockingly for the live loop; venue_kraken_ws_stop()
 * (called by venue destroy) joins it. */

ts_status venue_kraken_ws_start(venue_t *v, const char *instrument);
/* TS_OK + filled ev, or TS_ERR_NOT_FOUND when queue empty. */
ts_status venue_kraken_next_market_event(venue_t *v, ts_mkt_event *out);
bool venue_kraken_ws_running(venue_t *v);

#ifdef __cplusplus
}
#endif

#endif /* TRADING_KRAKEN_WS_H */
