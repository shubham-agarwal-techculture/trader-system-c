# API reference

Public headers live in `include/trading/`. All functions are C11, use
`ts_status` return codes (`TS_OK` = 0, negative = error), and are not
thread-safe unless stated — the current design is single-threaded
deterministic; concurrency is a later-phase concern.

## core.h

| Symbol | Description |
|---|---|
| `ts_ts_ns` | `int64_t` nanosecond timestamp |
| `ts_status` | `TS_OK`, `TS_ERR_INVALID_ARG`, `TS_ERR_NOT_FOUND`, `TS_ERR_BAD_STATE`, `TS_ERR_FULL`, `TS_ERR_IO`, `TS_ERR_PARSE`, `TS_ERR_NOMEM` |
| `ts_status_str(s)` | Human-readable status name |
| `ts_clock` | `{ now_ns(ctx), ctx }` — injectable clock |
| `ts_clock_mono()` / `ts_clock_mono_now()` | Real monotonic clock (QPC / `CLOCK_MONOTONIC`) |
| `ts_vclock` / `ts_clock_virtual(vc)` | Deterministic virtual clock for replay/tests |
| `ts_vclock_set` / `ts_vclock_advance` | Set/advance virtual time |

## events.h — normalized contracts

### Enums

- `ts_asset_class`: `TS_ASSET_EQUITY`, `TS_ASSET_CRYPTO`, `TS_ASSET_FUTURE`, `TS_ASSET_OPTION`, `TS_ASSET_FX`
- `ts_side`: `TS_SIDE_BUY` (+1), `TS_SIDE_SELL` (-1)
- `ts_order_type`: `TS_ORD_LIMIT`, `TS_ORD_MARKET`, `TS_ORD_STOP_LIMIT`
- `ts_tif`: `TS_TIF_GTC`, `TS_TIF_IOC`, `TS_TIF_FOK`, `TS_TIF_DAY`
- `ts_order_state`: `NEW`, `HELD`, `RISK_APPROVED`, `WORKING`, `PARTIAL`, `FILLED`, `CANCELED`, `REJECTED`, `EXPIRED`, `UNKNOWN`
- `ts_mkt_type`: `TS_MKT_QUOTE`, `TS_MKT_TRADE`, `TS_MKT_SNAPSHOT`
- `ts_venue_event_type`: `TS_VEV_ACK`, `TS_VEV_EXEC`, `TS_VEV_REJECT`, `TS_VEV_CANCEL_ACK`, `TS_VEV_EXPIRED`, `TS_VEV_UNKNOWN`

Helpers: `ts_order_state_is_terminal()`, `ts_*_str()` for all enums.

### Structs

- `ts_instrument` — `code`, `venue`, `asset`, `currency`, `settlement_currency`, `tick_size`, `lot_size`, `contract_multiplier`, `tradable`. Validate with `ts_instrument_validate()`.
- `ts_mkt_event` — `type`, `instrument`, `bid/ask/last`, `bid_qty/ask_qty/last_qty`, `ts_ns` (feed timestamp), `seq` (feed sequence).
- `ts_order_request` — what a strategy submits: `client_id` (idempotency key), `instrument`, `strategy`, `side`, `type`, `tif`, `qty`, `price`. Validate with `ts_order_request_validate()`.
- `ts_order` — full order record: request fields plus `venue_order_id`, `filled_qty`, `avg_fill_price`, `state`, `reject_reason`, `created_ns`, `updated_ns`.
- `ts_execution` — `client_id`, `venue_order_id`, `venue_exec_id`, `instrument`, `side`, `qty`, `price`, `fee`, `ts_ns`.
- `ts_venue_event` — EMS boundary event: `type`, `client_id`, `venue_order_id`, `instrument`, `reason`, embedded `exec`, `ts_ns`.

`ts_copy_str(dst, dstsz, src)` — bounded string copy used everywhere.

## oms.h

```c
oms_t *oms_create(const oms_config *cfg, size_t capacity);
void oms_destroy(oms_t *o);
```

`oms_config`: `{ oms, journal, metrics, clock }` — journal/metrics may be NULL.

```c
ts_status oms_submit(oms_t*, const ts_order_request*, ts_order *out);
const ts_order *oms_find(const oms_t*, const char *client_id);
const ts_order *oms_find_venue(const oms_t*, const char *venue_order_id);
const ts_order *oms_at(const oms_t*, size_t idx);
size_t oms_count / oms_open_count(const oms_t*);
```

Lifecycle mutators (all journaled, all return `ts_status`):

```c
oms_mark_risk(o, client_id, approved, rule, msg);   // NEW -> RISK_APPROVED|REJECTED
oms_on_ack(o, client_id, venue_order_id);          // -> WORKING
oms_on_execution(o, &exec);                        // -> PARTIAL|FILLED
oms_on_reject(o, client_id, reason);
oms_on_cancel(o, client_id);
oms_on_expire(o, client_id, reason);
oms_on_unknown(o, client_id, reason);              // -> UNKNOWN
oms_restore_order(o, &snapshot);                   // journal recovery
oms_orders_in_state(o, state, out, cap);           // reconciliation queries
```

Idempotency: `oms_submit` with an existing `client_id` returns the stored
order — resubmits never create duplicates.

## rms.h

```c
rms_t *rms_create(const rms_config *cfg);   // {rms, journal, metrics, clock}
void rms_set_limits(rms_t*, const ts_risk_limits*);
void rms_kill(rms_t*);  rms_resume(rms_t*); bool rms_is_killed(const rms_t*);
void rms_on_quote(rms_t*, instrument, bid, ask, last, quote_ns);
ts_risk_result rms_check_order(rms_t*, const ts_order*);
ts_status rms_on_execution(rms_t*, const ts_execution*);
```

`ts_risk_limits` fields: `max_order_qty`, `max_order_notional`,
`max_position_qty`, `daily_loss_limit`, `max_order_rate`,
`require_fresh_market`, `max_quote_age_ns`.
`ts_risk_limits_default()` provides conservative values.

`ts_risk_result`: `{ approved, code, rule, msg }` — `code` is a
`ts_risk_code` enum, `rule` a stable string for journals/alerts.

Inspection: `rms_position()`, `rms_realized_pnl()`,
`rms_unrealized_pnl()`, `rms_total_pnl()`, `rms_position_count()`.
Recovery: `rms_restore_position()`, `rms_restore_pnl()`.

## venue.h / ems.h

```c
typedef struct venue_ops {
    const char *name;
    ts_status (*submit)(venue_t*, const ts_order*);
    ts_status (*cancel)(venue_t*, const char *client_id);
    void (*on_market_event)(venue_t*, const ts_mkt_event*);
    ts_status (*poll)(venue_t*, ts_venue_event *out);  // TS_ERR_NOT_FOUND = empty
    bool (*connected)(venue_t*);
    void (*destroy)(venue_t*);
} venue_ops_t;
```

EMS:

```c
ems_t *ems_create(const ems_config *cfg);  // {ems, venue, oms, rms, metrics, journal, clock}
ts_status ems_submit(ems_t*, const ts_order*);   // requires NEW/HELD/RISK_APPROVED
ems_poll(ems_t*);                                 // drain+dispatch venue events
ems_cancel(ems_t*, client_id);
ems_on_market_event(ems_t*, const ts_mkt_event*);
bool ems_venue_connected(ems_t*);
```

Mock venue controls: `venue_mock_create(metrics, clock)`,
`venue_mock_set_reject_all(v, bool)`, `venue_mock_set_latency_ns(v, ns)`
(latency field reserved; not yet applied to event timing).

## journal.h

```c
journal_t *journal_open_append(path);   // create/append
journal_t *journal_open_read(path);
ts_status journal_append_snap/exec/risk/kill/acct(...);
ts_status journal_replay(path, cb, ctx, &skipped);
```

`journal_event` union-ish struct carries `type` plus the relevant payload
(`ts_order`, `ts_execution`, risk fields, kill flag, account fields).
Callback signature: `ts_status (*)(const journal_event*, void *ctx)`.

## strategy.h

```c
strategy_t *strategy_momentum_create(const momentum_config*);
// momentum_config: instrument, strategy_name, qty, move_threshold,
//                  direction state, sink callback + ctx

replay_t *replay_open(path);
ts_status replay_next(replay_t*, ts_mkt_event*);  // TS_ERR_NOT_FOUND = EOF
```

## metrics.h

`ts_metrics_init`, `ts_metrics_inc(m, TS_MET_*, n)`,
`ts_metrics_get`, `ts_metrics_latency(m, slot, name, dur_ns)`,
`ts_metrics_dump`. Counter IDs: `TS_MET_MKT_EVENTS`,
`TS_MET_MKT_SEQ_GAPS`, `TS_MET_MKT_STALE`, `TS_MET_ORDERS_SUBMITTED`,
`TS_MET_ORDERS_DUPLICATE`, `TS_MET_RISK_APPROVED`, `TS_MET_RISK_REJECTED`,
`TS_MET_VENUE_ACKS`, `TS_MET_VENUE_REJECTS`, `TS_MET_EXECUTIONS`,
`TS_MET_CANCELS`, `TS_MET_RECON_BREAKS`. Latency slots 0–4:
decision/risk/dispatch/ack/fill.

## config.h

INI-style loader (`src/core/config.c`):

```c
ts_config *ts_config_load(const char *path);
void ts_config_free(ts_config *c);
const char *ts_config_get(c, section, key, def);
double ts_config_get_double(c, section, key, def);
int64_t ts_config_get_i64(c, section, key, def);
bool ts_config_get_bool(c, section, key, def);
ts_status ts_config_apply_risk(c, ts_risk_limits *lim); // [risk] section
```

Format: `[section]` headers, `key = value`, `#`/`;` comments, max 512
entries / 64-char keys / 256-char values.

## seqwatch.h

Feed sequence-gap tracking per instrument:

```c
ts_seqwatch *ts_seqwatch_create(ts_metrics *metrics);
ts_status ts_seqwatch_on_event(ts_seqwatch*, const ts_mkt_event*);
const ts_seq_channel *ts_seqwatch_channel(w, instrument);
```

`seq == 0` events are ignored. Forward jumps count missing seqs into
`TS_MET_MKT_SEQ_GAPS`; `seq <= last` counts as `dup_or_old`.

## journal.h — durability

```c
void journal_set_flush(journal_t*, bool);  // fflush per record, default on
void journal_set_fsync(journal_t*, bool);  // fsync/_commit per record
```

## venue.h — open_orders + replay venue

`venue_ops_t.open_orders(v, out, cap, &count)` is optional; NULL means
"reconciliation unavailable". `ts_venue_open_order` carries
client_id/venue_order_id/instrument/side/qty/filled_qty/price.

`venue_replay_create(path, metrics, clock)` — scripted venue for
contract/fault tests. Script verbs: `ACK`, `EXEC`, `REJECT`, `CANCEL`,
`EXPIRED`, `UNKNOWN` (see venue-onboarding.md). No auto-ACK on submit.

## ems.h — reconciliation

```c
ts_status ems_reconcile(ems_t*, ts_recon_report *report);
// report: oms_open, venue_open, matched, oms_only, venue_only, supported
```

OMS orders in WORKING/PARTIAL absent from the venue's open-order list are
marked `TS_ORD_UNKNOWN`; venue-only orders count as breaks. Requires
`venue->ops->open_orders`.

## metrics.h — percentiles

`ts_metrics_latency_pct(m, slot, pct)` returns an estimated p-th
percentile in ns from a log2 histogram (upper-bound estimate).
`ts_metrics_dump` prints p50/p95/p99 alongside avg/max. New counter:
`TS_MET_EXEC_DUPLICATE`.

## OMS — exec dedup

`oms_on_execution` suppresses duplicate fills by `venue_exec_id` using a
4096-entry ring (`TS_MET_EXEC_DUPLICATE`). Executions without a venue exec
id are not deduplicated. Overfills are clamped and counted as
`TS_MET_RECON_BREAKS`.

## log.h

`ts_log_set_level(TS_LOG_TRACE..TS_LOG_ERROR)`, `ts_log(lvl, fmt, ...)`.
