#ifndef TRADING_JOURNAL_H
#define TRADING_JOURNAL_H

#include "trading/events.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Append-only event journal. This is the durable record of order snapshots,
 * executions, risk decisions, account updates and kill-switch transitions.
 * It is the recovery source of truth for the OMS/RMS. */

typedef enum {
    JE_SNAP = 0,   /* full order snapshot after a state change */
    JE_EXEC,       /* one execution report */
    JE_RISK,       /* risk decision audit record */
    JE_KILL,       /* kill-switch transition */
    JE_ACCT,       /* account cash balance update: <ccy> <amount> */
} journal_event_type;

typedef struct {
    journal_event_type type;
    ts_ts_ns ts_ns;
    ts_order order;          /* JE_SNAP */
    ts_execution exec;       /* JE_EXEC */
    char client_id[TS_MAX_ID];
    bool approved;           /* JE_RISK */
    char rule[32];           /* JE_RISK: rule code, e.g. "POSITION_LIMIT" */
    char msg[TS_MAX_REASON]; /* JE_RISK: human-readable detail */
    bool killed;             /* JE_KILL */
    char currency[8];        /* JE_ACCT */
    double amount;           /* JE_ACCT */
} journal_event;

typedef struct journal journal_t;

/* Open for append (creates file if needed). Pass NULL path components are
 * rejected. */
journal_t *journal_open_append(const char *path);
journal_t *journal_open_read(const char *path);
void journal_close(journal_t *j);

ts_status journal_append_snap(journal_t *j, const ts_order *o);
ts_status journal_append_exec(journal_t *j, const ts_execution *e);
ts_status journal_append_risk(journal_t *j, ts_ts_ns ts_ns,
                              const char *client_id, bool approved,
                              const char *rule, const char *msg);
ts_status journal_append_kill(journal_t *j, ts_ts_ns ts_ns, bool killed);
ts_status journal_append_acct(journal_t *j, ts_ts_ns ts_ns,
                              const char *currency, double amount);

/* Parse an existing journal and invoke cb for each well-formed event.
 * Returns TS_OK even if some lines were skipped (counted via *skipped). */
typedef ts_status (*journal_replay_fn)(const journal_event *ev, void *ctx);
ts_status journal_replay(const char *path, journal_replay_fn cb, void *ctx,
                         size_t *skipped);

#ifdef __cplusplus
}
#endif

#endif /* TRADING_JOURNAL_H */
