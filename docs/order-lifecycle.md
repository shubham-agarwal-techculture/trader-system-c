# Order lifecycle

## State machine

```
                 ┌──────────┐
   submit ──────>│   NEW    │
                 └────┬─────┘
                      │ rms_check_order
          ┌───────────┴───────────┐
          ▼                       ▼
   ┌─────────────┐         ┌────────────┐
   │ RISK_APPROVED│        │  REJECTED  │ (terminal)
   └──────┬──────┘         └────────────┘
          │ ems_submit -> venue
          ▼
   ┌─────────────┐   partial fill   ┌───────────┐
   │   WORKING   │ ───────────────> │  PARTIAL  │
   └──────┬──────┘                   └─────┬─────┘
          │                                │
          │            full fill           │
          └───────────────┬────────────────┘
                          ▼
                      ┌────────┐
                      │ FILLED │ (terminal)
                      └────────┘

   From WORKING/PARTIAL/RISK_APPROVED also reachable:
     CANCELED (terminal)   — cancel confirmed by venue
     EXPIRED  (terminal)   — TIF/session expiry (IOC remainder etc.)
     UNKNOWN  (non-term)   — venue state uncertain; reconcile first
     HELD     (from NEW)   — risk hold for manual review (reserved)
```

Terminal states: `FILLED`, `CANCELED`, `REJECTED`, `EXPIRED`.
`UNKNOWN` is non-terminal: it means "we do not know the venue outcome"
and blocks naive retries until reconciliation resolves it.

## Transition rules (oms.c)

`transition_allowed()` enforces the table; illegal transitions log a
warning, increment `TS_MET_RECON_BREAKS`, and return `TS_ERR_BAD_STATE`.
`from == to` is always allowed (idempotent re-reports, e.g. a duplicate
ACK).

| From \ To | WORKING | PARTIAL | FILLED | CANCELED | REJECTED | EXPIRED | UNKNOWN |
|---|---|---|---|---|---|---|---|
| NEW | – | – | – | yes | yes | – | yes |
| HELD | – | – | – | yes | yes | – | yes |
| RISK_APPROVED | yes | yes | yes | yes | yes | yes | yes |
| WORKING/PARTIAL | yes | yes | yes | yes | yes | yes | yes |
| UNKNOWN | yes | yes | yes | yes | yes | yes | yes |
| terminal | only same-state | | | | | | |

(NEW/HELD also allow -> RISK_APPROVED; omitted for brevity.)

## Key behaviors

### Idempotent submit

`oms_submit` keys on `client_id`. A resubmit returns the existing order
and bumps `TS_MET_ORDERS_DUPLICATE` — strategies may safely retry after
timeouts without duplicating orders.

### Fills

`oms_on_execution` accumulates `filled_qty` and recomputes
`avg_fill_price` as a weighted average. Overfills (cumulative qty > order
qty) are clamped and counted as reconciliation breaks. State becomes
`PARTIAL` until `filled_qty >= qty`, then `FILLED`.

### Venue correlation

`oms_on_ack` accepts either `client_id` or `venue_order_id` lookup;
execution/reject/cancel look up by `client_id` first, then
`venue_order_id`. This supports venues that echo only their own IDs.

### Uncertain outcomes

`ems_submit` transport failure -> `oms_on_unknown` (never blind retry).
Venue `TS_VEV_UNKNOWN` events -> same state. Resolution requires a
venue query (Phase 3 reconciliation) then an explicit transition.

### Recovery

Every transition persists a `SNAP` journal record containing the full
order. On restart, `journal_replay` -> `oms_restore_order` installs the
latest snapshot per `client_id`; open orders are those in non-terminal
states (`oms_open_count`).

## Amend

`ems_amend` -> `venue_ops.amend` -> venue emits `TS_VEV_AMENDED` ->
`oms_on_amend` updates `price`/`qty` on non-terminal orders and persists
a `SNAP`. Quantity may not be amended below `filled_qty`.

## Cancel / cancel-all

`ems_cancel` / `ems_cancel_all` -> `venue_ops.cancel`/`cancel_all` ->
per-order `TS_VEV_CANCEL_ACK` events -> `oms_on_cancel` -> `CANCELED`.
Cancel-all emits one ACK per working order.
