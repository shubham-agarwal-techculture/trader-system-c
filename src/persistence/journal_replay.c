#include "trading/journal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 2048
#define MAX_FIELDS 24

/* Tokenize a line on tabs/newlines. Fields are not modified in place for
 * strings we need to keep; we copy into a token table. */
static int split_fields(char *line, char **fields, int maxf)
{
    int n = 0;
    char *p = line;
    while (*p && n < maxf) {
        fields[n++] = p;
        char *tab = strchr(p, '\t');
        if (!tab) {
            break;
        }
        *tab = '\0';
        p = tab + 1;
    }
    /* trim trailing newline/CR from last field */
    if (n > 0) {
        size_t len = strlen(fields[n - 1]);
        while (len > 0 && (fields[n - 1][len - 1] == '\n' ||
                           fields[n - 1][len - 1] == '\r')) {
            fields[n - 1][--len] = '\0';
        }
    }
    return n;
}

static double parse_double(const char *s, bool *ok)
{
    char *end = NULL;
    double v = strtod(s, &end);
    if (!end || *end != '\0') {
        *ok = false;
        return 0.0;
    }
    return v;
}

static int64_t parse_i64(const char *s, bool *ok)
{
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (!end || *end != '\0') {
        *ok = false;
        return 0;
    }
    return (int64_t)v;
}

static void clear_str(char *dst, size_t n)
{
    memset(dst, 0, n);
}

ts_status journal_replay(const char *path, journal_replay_fn cb, void *ctx,
                         size_t *skipped)
{
    if (skipped) {
        *skipped = 0;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return TS_ERR_IO;
    }

    char line[MAX_LINE];
    size_t lineno = 0;
    ts_status result = TS_OK;

    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        journal_event ev;
        memset(&ev, 0, sizeof(ev));
        clear_str(ev.client_id, sizeof(ev.client_id));
        clear_str(ev.rule, sizeof(ev.rule));
        clear_str(ev.msg, sizeof(ev.msg));
        clear_str(ev.currency, sizeof(ev.currency));

        char work[MAX_LINE];
        snprintf(work, sizeof(work), "%s", line);
        char *fields[MAX_FIELDS];
        int nf = split_fields(work, fields, MAX_FIELDS);
        if (nf < 2) {
            if (skipped) (*skipped)++;
            continue;
        }

        bool ok = true;
        if (strcmp(fields[0], "SNAP") == 0) {
            if (nf < 15) { if (skipped) (*skipped)++; continue; }
            ev.type = JE_SNAP;
            ev.ts_ns = parse_i64(fields[1], &ok);
            ts_copy_str(ev.order.client_id, sizeof(ev.order.client_id), fields[2]);
            ts_copy_str(ev.order.venue_order_id, sizeof(ev.order.venue_order_id), fields[3]);
            ts_copy_str(ev.order.instrument, sizeof(ev.order.instrument), fields[4]);
            ts_copy_str(ev.order.strategy, sizeof(ev.order.strategy), fields[5]);
            ev.order.side = (ts_side)atoi(fields[6]);
            ev.order.type = (ts_order_type)atoi(fields[7]);
            ev.order.tif = (ts_tif)atoi(fields[8]);
            ev.order.qty = parse_double(fields[9], &ok);
            ev.order.price = parse_double(fields[10], &ok);
            ev.order.filled_qty = parse_double(fields[11], &ok);
            ev.order.avg_fill_price = parse_double(fields[12], &ok);
            ev.order.state = (ts_order_state)atoi(fields[13]);
            ts_copy_str(ev.order.reject_reason, sizeof(ev.order.reject_reason), fields[14]);
            ev.order.created_ns = ev.ts_ns;
            ev.order.updated_ns = ev.ts_ns;
            if (!ok) { if (skipped) (*skipped)++; continue; }
        } else if (strcmp(fields[0], "EXEC") == 0) {
            if (nf < 9) { if (skipped) (*skipped)++; continue; }
            ev.type = JE_EXEC;
            ev.ts_ns = parse_i64(fields[1], &ok);
            ts_copy_str(ev.exec.client_id, sizeof(ev.exec.client_id), fields[2]);
            ts_copy_str(ev.exec.venue_exec_id, sizeof(ev.exec.venue_exec_id), fields[3]);
            ts_copy_str(ev.exec.instrument, sizeof(ev.exec.instrument), fields[4]);
            ev.exec.side = (ts_side)atoi(fields[5]);
            ev.exec.qty = parse_double(fields[6], &ok);
            ev.exec.price = parse_double(fields[7], &ok);
            ev.exec.fee = parse_double(fields[8], &ok);
            ev.exec.ts_ns = ev.ts_ns;
            if (!ok) { if (skipped) (*skipped)++; continue; }
        } else if (strcmp(fields[0], "RISK") == 0) {
            if (nf < 6) { if (skipped) (*skipped)++; continue; }
            ev.type = JE_RISK;
            ev.ts_ns = parse_i64(fields[1], &ok);
            ts_copy_str(ev.client_id, sizeof(ev.client_id), fields[2]);
            ev.approved = atoi(fields[3]) != 0;
            ts_copy_str(ev.rule, sizeof(ev.rule), fields[4]);
            ts_copy_str(ev.msg, sizeof(ev.msg), fields[5]);
            if (!ok) { if (skipped) (*skipped)++; continue; }
        } else if (strcmp(fields[0], "KILL") == 0) {
            if (nf < 3) { if (skipped) (*skipped)++; continue; }
            ev.type = JE_KILL;
            ev.ts_ns = parse_i64(fields[1], &ok);
            ev.killed = atoi(fields[2]) != 0;
            if (!ok) { if (skipped) (*skipped)++; continue; }
        } else if (strcmp(fields[0], "ACCT") == 0) {
            if (nf < 4) { if (skipped) (*skipped)++; continue; }
            ev.type = JE_ACCT;
            ev.ts_ns = parse_i64(fields[1], &ok);
            ts_copy_str(ev.currency, sizeof(ev.currency), fields[2]);
            ev.amount = parse_double(fields[3], &ok);
            if (!ok) { if (skipped) (*skipped)++; continue; }
        } else {
            if (skipped) (*skipped)++;
            continue;
        }

        ts_status st = cb ? cb(&ev, ctx) : TS_OK;
        if (st != TS_OK) {
            result = st;
            break;
        }
    }

    fclose(fp);
    return result;
}
