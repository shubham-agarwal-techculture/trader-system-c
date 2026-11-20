# Venue onboarding

To add a venue (direct exchange or broker):

1. **Capabilities** — document supported order types, TIFs, amend/cancel
   semantics, market data channels, rate limits, session calendar,
   reconnect/replay behavior.
2. **Adapter** — implement `venue_ops_t` (`include/trading/venue.h`):
   `submit`, `cancel`, `on_market_event`, `poll`, `connected`, `destroy`.
   Translate wire protocol <-> normalized `ts_order`/`ts_venue_event`.
   Keep ALL venue quirks inside the adapter.
3. **Contract tests** — fixtures of recorded messages; fake-venue tests
   for ACK/reject/partial-fill/cancel/duplicate/seq-gap/reconnect.
4. **Paper validation** — run soak tests through the paper path before
   any live enablement.
5. **Risk profile** — per-venue `ts_risk_limits`, instrument whitelist,
   session rules, jurisdictional constraints.
6. **Reconciliation** — order/exec/position/balance diff vs. venue
   before enabling order entry.

## Adapter families

- `src/ems/venue_mock.c` — deterministic paper venue: auto-ACKs,
  matches against latest quote, GTC/IOC/FOK semantics.
- `src/ems/venue_replay.c` — scripted venue for contract/fault tests.
  Script format (one event per line, `#` comments):

  ```
  ACK      <client_id> <venue_order_id>
  EXEC     <client_id> <exec_id> <instrument> <BUY|SELL> <qty> <price> [fee]
  REJECT   <client_id> <reason words...>
  CANCEL   <client_id>
  EXPIRED  <client_id> [reason...]
  UNKNOWN  <client_id> [reason...]
  ```

  Replay venue does NOT auto-ACK — scripts must include ACK lines so
  tests control exact event ordering (duplicates, out-of-order, rejects).
- Future: `src/ems/venue_<exchange>.c`, `src/ems/venue_<broker>.c`.

Direct-exchange and broker adapters share the contract but MUST NOT
assume identical semantics — e.g., broker order IDs, aggregate fills,
and throttling differ per venue.

## Contract-test checklist (run against every adapter)

Use `tests/test_hardening.c` replay-venue scenarios as the template:
submit -> ACK -> partial fills -> full fill; reject paths; cancel;
duplicate exec id suppression; UNKNOWN events; `open_orders`
reconciliation via `ems_reconcile`.
