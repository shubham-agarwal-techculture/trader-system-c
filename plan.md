# C-Based Multi-Venue HFT Trading System

Design a market-agnostic, low-latency C trading platform with OMS, RMS, EMS, staged paper/live delivery, and support for direct-exchange and broker adapters across multiple markets.

## Context

The workspace currently contains only `description.md`; no source, build system, tests, or existing trading-system components are present. The requested scope is therefore a greenfield system. The user selected all markets, both direct-exchange and broker-mediated access, and a phased paper-to-live roadmap, so the design must avoid baking in one venue, protocol, instrument model, or broker behavior.

## Architecture and scope

1. **Venue-neutral core**
   - Separate market-data, OMS, RMS, EMS, strategy, persistence, and observability concerns.
   - Normalize instruments, quotes, trades, order books (where available), order state, executions, positions, balances, and risk events into stable internal contracts.
   - Keep venue-specific normalization at adapter boundaries so new markets can be added without changing strategy or risk logic.
   - Treat instruments as typed entities with explicit asset class, currency, settlement currency, multiplier, tick/lot rules, session calendars, and tradability state.

2. **OMS (Order Management System)**
   - Maintain the authoritative order lifecycle: accepted, validated, risk-checked, held/rejected, routed, working, partially filled, filled, canceled, rejected, expired, and unknown/needs-reconciliation.
   - Provide idempotent client order IDs, sequence numbers, order amendments/cancellations where supported, order-to-execution linkage, and deterministic state transitions.
   - Persist enough state to reconstruct the order book of open orders after restart and reconcile external venue state.
   - Expose internal APIs/IPC for strategy submission and separate read APIs for monitoring.

3. **RMS (Risk Management System)**
   - Enforce pre-trade controls synchronously before an order can reach EMS: instrument/session eligibility, price/tick/quantity bounds, max order notional, position limits, daily loss limits, exposure limits, order-rate limits, duplicate-order protection, and kill-switch state.
   - Add post-trade checks for fills, position updates, margin/balance changes, stale data, reconciliation breaks, and circuit breakers.
   - Make risk decisions auditable and fail-safe for live mode; define explicit behavior for missing balances, stale market data, disconnected feeds, and uncertain order acknowledgements.
   - Support configurable profiles per strategy/account/venue, with a conservative default and explicit approval for live escalation.

4. **EMS (Execution Management System)**
   - Select venue/order type, translate normalized orders to venue protocols, manage routing, timeouts, retries, amendments, cancels, and order acknowledgement handling.
   - Keep transport and protocol adapters behind interfaces; support direct exchange APIs and broker-mediated APIs as distinct adapter families.
   - Use deterministic routing and latency-aware queues; preserve causality and avoid unbounded queues. Instrument queue depth, dropped messages, backpressure, and recovery behavior.
   - Separate venue acknowledgement from business confirmation and reconcile every externally visible order state.

5. **High-frequency execution path**
   - Design the hot path as a staged pipeline: ingress decode -> normalization -> deterministic strategy signal -> OMS validation -> RMS -> EMS encode/submit, with bounded memory and explicit ownership.
   - Prefer lock-free or single-writer queues only where profiling justifies them; begin with simple, testable queues and isolate allocation from hot loops.
   - Use monotonic clocks and timestamps at ingress, decision, risk decision, dispatch, ACK, and fill; record latency distributions without logging sensitive payloads.
   - Make batching, multicast/UDP, kernel-bypass networking, FPGA/GPU, and exotic transport optimizations later-stage decisions gated by measured bottlenecks and venue support.
   - Support paper-trading adapters that preserve protocol semantics and deterministic latency/fill models without claiming real-market fidelity.

6. **Market and venue adapters**
   - Define contracts for market data, reference data, account/balance, order entry, execution reports, cancel/replace, and session status.
   - Build a `MockVenue` and `ReplayVenue` first, then add venue implementations in priority order. Since “all” markets cannot be supported safely in one initial milestone, implement the abstraction broadly and validate one or two concrete venues per phase.
   - For direct exchange and broker access, use separate adapter implementations over the same internal contracts; do not assume broker semantics are identical to direct exchange semantics.
   - Include capability discovery, session calendars, trading halts, rejects, rate limits, sequence-gap handling, and reconnect/replay policies.

7. **Persistence and recovery**
   - Store configuration snapshots, instruments/reference data, order lifecycle events, executions, positions, balances, risk decisions, and reconciliation state in a durable event log with indexed projections.
   - Choose an embedded database or append-only journal after a dependency/operational review; avoid introducing a distributed database for the first version unless required.
   - Define recovery order: load last committed snapshot, replay durable events, query venue/open-order state, reconcile, and only then enable order entry for live mode.

8. **Observability and operations**
   - Emit structured metrics for feed health, sequence gaps, queue latency, decision latency, risk rejects, order ACK latency, fill latency, positions, P&L, retries, disconnects, and reconciliation breaks.
   - Add alerting for stale feeds, sequence gaps, repeated rejects, queue saturation, risk-limit breaches, uncertain orders, and venue disconnects.
   - Provide health/readiness endpoints or CLI status, audit logs, configuration validation, and a documented emergency flatten/kill-switch procedure.
   - Never log credentials, raw sensitive account payloads, or unnecessary personal data.

9. **Security and compliance boundaries**
   - Keep credentials in OS credential storage or a secrets manager, never in source or ordinary logs.
   - Use least-privilege API keys, separate paper/live credentials, network allowlists where possible, and signed/config-validated deployment artifacts.
   - Treat market data, broker messages, and logs as untrusted input; validate schemas, bounds, timestamps, IDs, and sequence numbers.
   - Include jurisdiction-specific constraints as venue configuration and legal review items, especially for US, Indian, and crypto markets; do not assume one rule set applies everywhere.

10. **Testing strategy**
   - Unit-test state machines, risk rules, normalization, order IDs, retry/reconciliation logic, and clock behavior.
   - Contract-test every venue adapter against recorded fixtures and a fake venue; test disconnects, duplicate ACKs, partial fills, rejects, amendments, sequence gaps, and restart recovery.
   - Use deterministic replay and fault injection for latency, packet loss, exchange rejects, stale books, and crash recovery.
   - Add paper-trading soak tests and capacity tests before live consideration; benchmark hot paths with representative market-data rates and document hardware assumptions.

## Delivery phases

1. **Phase 0 — Foundation:** repository structure, C build system, core types, clock, logging, config, tests, CI, and documentation.
2. **Phase 1 — Simulated OMS/EMS:** normalized order model, in-memory/durable OMS, risk service, mock venue, paper matching, lifecycle and restart tests.
3. **Phase 2 — Market-data and strategy framework:** replay feed, book/trade normalization, strategy interface, signal pipeline, and deterministic paper execution.
4. **Phase 3 — First real adapter:** choose one venue based on user priorities; implement market-data and order-entry adapters, reconciliation, and paper/live staging.
5. **Phase 4 — HFT hardening:** latency instrumentation, profiling, queue tuning, session/calendar handling, failover, capacity testing, and operational runbooks.
6. **Phase 5 — Additional markets:** add direct-exchange and broker adapters one venue at a time, preserving the common contracts and expanding instrument coverage.

## Files to create

- `REPOSITORY_PLAN.md` — the requested detailed implementation and delivery plan.
- During implementation, proposed layout: `src/core`, `src/oms`, `src/rms`, `src/ems`, `src/adapters`, `src/strategy`, `src/persistence`, `src/observability`, `tests`, `fixtures`, `docs`, and a top-level CMake or Make build definition.

## Verification

- [ ] Validate the repository plan for technical consistency and explicit phase gates.
- [ ] During implementation, run formatting/static analysis, unit tests, contract tests, replay tests, and a clean build on the selected platform.
- [ ] Run paper-trading soak/reconciliation tests before enabling any live adapter.
- [ ] Profile latency and resource usage under representative load; document measured results rather than assuming HFT performance.

## Risks and considerations

- “All markets” is a broad product scope; the plan keeps the core market-agnostic but sequences concrete venue support to avoid an unbounded adapter effort.
- HFT requirements vary by venue and may require specialized networking, co-location, exchange certifications, or vendor agreements; those are external dependencies and must be validated before live deployment.
- A C implementation must explicitly manage memory, threading, synchronization, and undefined behavior; the plan prioritizes correctness and deterministic recovery before aggressive optimization.
- Live trading introduces financial and operational risk; paper trading, reconciliation, limits, kill switches, and staged approvals are mandatory gates.
