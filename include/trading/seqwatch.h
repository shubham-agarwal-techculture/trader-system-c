#ifndef TRADING_SEQWATCH_H
#define TRADING_SEQWATCH_H

#include "trading/events.h"
#include "trading/metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Feed sequence-gap watcher. Tracks the last seen `seq` per instrument
 * and reports forward jumps as gaps. Out-of-order (seq <= last) events are
 * counted separately as duplicates/replays, not gaps. */

#define TS_SEQWATCH_MAX_INSTRUMENTS 128

typedef struct {
    char instrument[TS_MAX_CODE];
    uint64_t last_seq;
    bool primed;
    uint64_t events;
    uint64_t gaps;
    uint64_t dup_or_old;
} ts_seq_channel;

typedef struct ts_seqwatch ts_seqwatch;

ts_seqwatch *ts_seqwatch_create(ts_metrics *metrics);
void ts_seqwatch_destroy(ts_seqwatch *w);

/* Feed one market event. Returns TS_OK; gaps increment
 * TS_MET_MKT_SEQ_GAPS and TS_MET_MKT_STALE is untouched (staleness is a
 * time-domain check done by RMS). */
ts_status ts_seqwatch_on_event(ts_seqwatch *w, const ts_mkt_event *ev);

const ts_seq_channel *ts_seqwatch_channel(const ts_seqwatch *w,
                                          const char *instrument);
size_t ts_seqwatch_channel_count(const ts_seqwatch *w);
const ts_seq_channel *ts_seqwatch_channel_at(const ts_seqwatch *w, size_t i);

#ifdef __cplusplus
}
#endif

#endif /* TRADING_SEQWATCH_H */
