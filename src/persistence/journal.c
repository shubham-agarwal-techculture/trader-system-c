#include "trading/journal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct journal {
    FILE *fp;
    bool writing;
};

journal_t *journal_open_append(const char *path)
{
    if (!path || !path[0]) {
        return NULL;
    }
    FILE *fp = fopen(path, "a");
    if (!fp) {
        return NULL;
    }
    journal_t *j = calloc(1, sizeof(*j));
    if (!j) {
        fclose(fp);
        return NULL;
    }
    j->fp = fp;
    j->writing = true;
    return j;
}

journal_t *journal_open_read(const char *path)
{
    if (!path || !path[0]) {
        return NULL;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return NULL;
    }
    journal_t *j = calloc(1, sizeof(*j));
    if (!j) {
        fclose(fp);
        return NULL;
    }
    j->fp = fp;
    j->writing = false;
    return j;
}

void journal_close(journal_t *j)
{
    if (!j) {
        return;
    }
    if (j->fp) {
        fclose(j->fp);
    }
    free(j);
}

static ts_status write_line(journal_t *j, const char *fmt, ...)
{
    if (!j || !j->writing || !j->fp) {
        return TS_ERR_BAD_STATE;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(j->fp, fmt, ap);
    va_end(ap);
    if (n < 0 || fputc('\n', j->fp) == EOF) {
        return TS_ERR_IO;
    }
    return TS_OK;
}

/* Field order for SNAP lines (tab-separated, doubles via %.17g):
 * SNAP <ts> <client_id> <venue_order_id> <instrument> <strategy> <side>
 *      <type> <tif> <qty> <price> <filled_qty> <avg_fill_price> <state>
 *      <reject_reason> */
static ts_status journal_write_snap(journal_t *j, const ts_order *o)
{
    return write_line(j,
        "SNAP\t%lld\t%s\t%s\t%s\t%s\t%d\t%d\t%d\t%.17g\t%.17g\t%.17g\t%.17g\t%d\t%s",
        (long long)o->updated_ns,
        o->client_id, o->venue_order_id, o->instrument, o->strategy,
        (int)o->side, (int)o->type, (int)o->tif,
        o->qty, o->price, o->filled_qty, o->avg_fill_price,
        (int)o->state, o->reject_reason);
}

/* EXEC <ts> <client_id> <venue_exec_id> <instrument> <side> <qty> <price> <fee> */
static ts_status journal_write_exec(journal_t *j, const ts_execution *e)
{
    return write_line(j,
        "EXEC\t%lld\t%s\t%s\t%s\t%d\t%.17g\t%.17g\t%.17g",
        (long long)e->ts_ns,
        e->client_id, e->venue_exec_id, e->instrument, (int)e->side,
        e->qty, e->price, e->fee);
}

/* RISK <ts> <client_id> <approved> <rule> <msg> */
static ts_status journal_write_risk(journal_t *j, ts_ts_ns ts_ns,
                                    const char *client_id, bool approved,
                                    const char *rule, const char *msg)
{
    return write_line(j, "RISK\t%lld\t%s\t%d\t%s\t%s",
        (long long)ts_ns, client_id, approved ? 1 : 0, rule,
        msg ? msg : "");
}

/* KILL <ts> <killed> */
static ts_status journal_write_kill(journal_t *j, ts_ts_ns ts_ns, bool killed)
{
    return write_line(j, "KILL\t%lld\t%d", (long long)ts_ns, killed ? 1 : 0);
}

/* ACCT <ts> <currency> <amount> */
static ts_status journal_write_acct(journal_t *j, ts_ts_ns ts_ns,
                                    const char *currency, double amount)
{
    return write_line(j, "ACCT\t%lld\t%s\t%.17g",
        (long long)ts_ns, currency, amount);
}

ts_status journal_append_snap(journal_t *j, const ts_order *o)
{
    return journal_write_snap(j, o);
}

ts_status journal_append_exec(journal_t *j, const ts_execution *e)
{
    return journal_write_exec(j, e);
}

ts_status journal_append_risk(journal_t *j, ts_ts_ns ts_ns,
                              const char *client_id, bool approved,
                              const char *rule, const char *msg)
{
    return journal_write_risk(j, ts_ns, client_id, approved, rule, msg);
}

ts_status journal_append_kill(journal_t *j, ts_ts_ns ts_ns, bool killed)
{
    return journal_write_kill(j, ts_ns, killed);
}

ts_status journal_append_acct(journal_t *j, ts_ts_ns ts_ns,
                              const char *currency, double amount)
{
    return journal_write_acct(j, ts_ns, currency, amount);
}
