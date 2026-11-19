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

- `src/ems/venue_mock.c` — deterministic paper venue (current).
- Future: `src/ems/venue_<exchange>.c`, `src/ems/venue_<broker>.c`.

Direct-exchange and broker adapters share the contract but MUST NOT
assume identical semantics — e.g., broker order IDs, aggregate fills,
and throttling differ per venue.
