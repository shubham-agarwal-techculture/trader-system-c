#include "trading/strategy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct replay {
    void *impl;
};

typedef struct {
    FILE *fp;
    size_t lineno;
} replay_impl;

replay_t *replay_open(const char *path)
{
    if (!path) {
        return NULL;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return NULL;
    }
    replay_impl *ri = calloc(1, sizeof(*ri));
    if (!ri) {
        fclose(fp);
        return NULL;
    }
    ri->fp = fp;
    replay_t *r = calloc(1, sizeof(*r));
    if (!r) {
        fclose(fp);
        free(ri);
        return NULL;
    }
    r->impl = ri;
    return r;
}

void replay_close(replay_t *r)
{
    if (!r) {
        return;
    }
    replay_impl *ri = r->impl;
    if (ri && ri->fp) {
        fclose(ri->fp);
    }
    free(ri);
    free(r);
}

size_t replay_lineno(const replay_t *r)
{
    return r && r->impl ? ((replay_impl *)r->impl)->lineno : 0;
}

ts_status replay_next(replay_t *r, ts_mkt_event *ev)
{
    if (!r || !ev || !r->impl) {
        return TS_ERR_INVALID_ARG;
    }
    replay_impl *ri = r->impl;
    char line[1024];

    while (fgets(line, sizeof(line), ri->fp)) {
        ri->lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '#') {
            continue;
        }

        memset(ev, 0, sizeof(*ev));
        char tag[16];
        char instr[TS_MAX_CODE];
        double a, b, c;
        long long ts = 0;
        unsigned long long seq = 0;

        int n = sscanf(p, "%15s %23s %lf %lf %lf %lld %llu",
                       tag, instr, &a, &b, &c, &ts, &seq);
        if (n < 4) {
            continue; /* malformed line: skip */
        }
        ts_copy_str(ev->instrument, sizeof(ev->instrument), instr);
        ev->ts_ns = (ts_ts_ns)ts;
        ev->seq = seq;

        if (strcmp(tag, "QUOTE") == 0) {
            ev->type = TS_MKT_QUOTE;
            ev->bid = a;
            ev->ask = b;
            ev->last = (n >= 5) ? c : (a + b) * 0.5;
        } else if (strcmp(tag, "TRADE") == 0) {
            ev->type = TS_MKT_TRADE;
            ev->last = a;
            ev->last_qty = b;
        } else {
            continue;
        }
        return TS_OK;
    }
    return TS_ERR_NOT_FOUND;
}
