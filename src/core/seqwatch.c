#include "trading/seqwatch.h"

#include "trading/log.h"

#include <stdlib.h>
#include <string.h>

struct ts_seqwatch {
    ts_seq_channel ch[TS_SEQWATCH_MAX_INSTRUMENTS];
    size_t count;
    ts_metrics *metrics;
};

ts_seqwatch *ts_seqwatch_create(ts_metrics *metrics)
{
    ts_seqwatch *w = calloc(1, sizeof(*w));
    if (w) {
        w->metrics = metrics;
    }
    return w;
}

void ts_seqwatch_destroy(ts_seqwatch *w)
{
    free(w);
}

static ts_seq_channel *find_or_add(ts_seqwatch *w, const char *instrument)
{
    for (size_t i = 0; i < w->count; ++i) {
        if (strcmp(w->ch[i].instrument, instrument) == 0) {
            return &w->ch[i];
        }
    }
    if (w->count >= TS_SEQWATCH_MAX_INSTRUMENTS) {
        return NULL;
    }
    ts_seq_channel *c = &w->ch[w->count++];
    memset(c, 0, sizeof(*c));
    ts_copy_str(c->instrument, sizeof(c->instrument), instrument);
    return c;
}

ts_status ts_seqwatch_on_event(ts_seqwatch *w, const ts_mkt_event *ev)
{
    if (!w || !ev || ev->seq == 0) {
        return TS_ERR_INVALID_ARG;
    }
    ts_seq_channel *c = find_or_add(w, ev->instrument);
    if (!c) {
        ts_metrics_inc(w->metrics, TS_MET_RECON_BREAKS, 1);
        return TS_ERR_FULL;
    }
    c->events++;

    if (!c->primed) {
        c->primed = true;
        c->last_seq = ev->seq;
        return TS_OK;
    }
    if (ev->seq <= c->last_seq) {
        c->dup_or_old++;
        return TS_OK; /* duplicate or out-of-order delivery */
    }
    if (ev->seq > c->last_seq + 1) {
        uint64_t gap = ev->seq - c->last_seq - 1;
        c->gaps += gap;
        ts_metrics_inc(w->metrics, TS_MET_MKT_SEQ_GAPS, gap);
        ts_log(TS_LOG_WARN, "seqwatch: gap on %s: %llu -> %llu (%llu missing)",
               ev->instrument, (unsigned long long)c->last_seq,
               (unsigned long long)ev->seq, (unsigned long long)gap);
    }
    c->last_seq = ev->seq;
    return TS_OK;
}

const ts_seq_channel *ts_seqwatch_channel(const ts_seqwatch *w,
                                          const char *instrument)
{
    if (!w) {
        return NULL;
    }
    for (size_t i = 0; i < w->count; ++i) {
        if (strcmp(w->ch[i].instrument, instrument) == 0) {
            return &w->ch[i];
        }
    }
    return NULL;
}

size_t ts_seqwatch_channel_count(const ts_seqwatch *w)
{
    return w ? w->count : 0;
}

const ts_seq_channel *ts_seqwatch_channel_at(const ts_seqwatch *w, size_t i)
{
    return (w && i < w->count) ? &w->ch[i] : NULL;
}
