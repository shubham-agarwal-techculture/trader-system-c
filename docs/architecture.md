# Architecture

## Pipeline

```
replay/exchange feed -> normalize (ts_mkt_event)
    -> rms_on_quote (marks/freshness)
    -> ems_on_market_event (venue sees market data)
    -> strategy.on_event -> order_sink
        -> oms_submit (idempotent, NEW)
        -> rms_check_order (sync gate)
        -> oms_mark_risk (RISK_APPROVED | REJECTED)
        -> ems_submit -> venue.submit
    -> ems_poll -> venue events
        -> ACK       -> oms_on_ack      (WORKING)
        -> EXEC      -> oms_on_execution + rms_on_execution
        -> REJECT    -> oms_on_reject   (REJECTED)
        -> CANCEL_ACK-> oms_on_cancel   (CANCELED)
        -> EXPIRED   -> oms_on_expire   (EXPIRED)
        -> UNKNOWN   -> oms_on_unknown  (UNKNOWN, reconciliation)
```

## Ownership

- **OMS** owns the order lifecycle and persists every transition as an
  order snapshot in the journal.
- **RMS** owns positions, realized/unrealized P&L, rate windows, and the
  kill switch. It never sees venue protocols.
- **EMS** owns venue communication and translates venue events back into
  OMS/RMS calls. It never makes risk decisions.
- **Venue adapters** (`venue_ops_t`) translate normalized orders/events to
  and from wire protocols. Direct-exchange and broker adapters implement
  the same contract; semantics that differ (IDs, amendments, rate limits)
  are adapter capabilities, not core assumptions.

## Order states

`NEW -> HELD|RISK_APPROVED|REJECTED|CANCELED|UNKNOWN`
`RISK_APPROVED -> WORKING|PARTIAL|FILLED|REJECTED|CANCELED|EXPIRED|UNKNOWN`
`WORKING/PARTIAL -> WORKING|PARTIAL|FILLED|CANCELED|REJECTED|EXPIRED|UNKNOWN`
Terminal: `FILLED`, `CANCELED`, `REJECTED`, `EXPIRED`.
`UNKNOWN` is the reconciliation state: an order whose venue outcome is
uncertain must be resolved by querying venue state before any retry.

## Persistence

`journal_append_*` writes tab-separated records:

- `SNAP` — full order snapshot on every state change (recovery source)
- `EXEC` — one execution report (rebuilds positions/P&L)
- `RISK` — risk decision audit
- `KILL` — kill-switch transitions
- `ACCT` — account balance updates

Recovery replays the journal into a fresh OMS/RMS. Live enablement
additionally requires venue reconciliation (Phase 3+).

## Hot-path rules

- Fixed-capacity tables; no unbounded allocation.
- Virtual clock (`ts_vclock`) makes replay and tests deterministic.
- Latency metrics recorded at decision/risk/dispatch/ack/fill stages.
- Lock-free structures are deferred until profiling justifies them.
