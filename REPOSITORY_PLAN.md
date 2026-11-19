# C-Based Multi-Venue HFT Trading System

Design and build a market-agnostic, low-latency trading platform in C with a clear separation between order management (OMS), risk management (RMS), and execution management (EMS). Support multiple markets and both direct-exchange and broker-mediated connectivity through a phased, paper-first roadmap.

## 1. Project context and current state

This repository is currently a greenfield project. The existing workspace contains only `description.md`; there is no source code, build system, test suite, or prior trading-system implementation to preserve. The implementation should therefore establish a maintainable foundation rather than retrofit an existing architecture.

The requested scope is intentionally broad:

- Multiple markets and asset classes.
- Both direct exchange connectivity and broker-mediated connectivity.
- Order Management System (OMS).
- Risk Management System (RMS).
- Execution Management System (EMS).
- High-frequency trading (HFT) performance goals.
- A phased delivery model that reaches live trading only after paper trading and operational validation.

Because no single exchange protocol or broker API represents all markets, the core system must be venue-neutral. Concrete venue behavior belongs behind adapter interfaces.

## 2. Goals and non-goals

### Goals

- Provide a deterministic order lifecycle and an auditable record of every order, risk decision, execution, and reconciliation event.
- Enforce configurable pre-trade and post-trade risk controls before any live order can reach a venue.
- Normalize heterogeneous market data and order events into stable internal contracts.
- Support direct-exchange and broker APIs without changing the strategy or core trading logic.
- Make the hot path measurable, bounded, and safe to operate under load.
- Recover cleanly after process restarts and venue disconnects.
- Deliver value in phases: simulated trading first, then real adapters, then hardening and additional venues.

### Non-goals for the first implementation

- Supporting every exchange and broker at once.
- Guaranteeing exchange-specific co-location, kernel-bypass networking, FPGA acceleration, or sub-microsecond latency before measurement and venue requirements justify them.
- Treating paper trading as proof of live profitability or venue compatibility.
- Bypassing exchange rules, broker agreements, market-data licenses, or jurisdictional requirements.
- Building a distributed trading platform before a single-process design has been profiled and shown to be insufficient.

## 3. Recommended architecture

The system should use a modular, layered architecture with explicit data ownership and lifecycle boundaries.

```text
Market data adapters ─┐
                      ├─> Normalization ─> Strategy engine ─> OMS ─> RMS ─> EMS ─> Venue adapters
Reference/account ───┘                         │             │       │
                                               │             │       └─> Venue protocols
                                               └─────────────┴─> Persistence / observability
```

### 3.1 Core domain contracts

Define versioned internal representations for:

- Instrument identity, asset class, exchange/venue, currency, settlement currency, multiplier, tick size, lot size, expiry, and tradability state.
- Market snapshots, quotes, trades, order-book updates, sequence numbers, timestamps, and feed health.
- Client order ID, venue order ID, correlation ID, account, strategy, instrument, side, order type, quantity, price, time-in-force, and state.
- Executions, fills, commissions, fees, positions, balances, margin, and currency conversions.
- Risk decisions, approvals, rejections, limit changes, kill-switch events, and reconciliation breaks.

Contracts must be language-stable at module boundaries, have explicit ownership rules, and be validated at untrusted adapter boundaries. Avoid exposing raw venue messages to strategy or risk code.

### 3.2 OMS — Order Management System

The OMS is the authoritative owner of the internal order lifecycle.

Required capabilities:

- Accept strategy orders through an internal API or IPC boundary.
- Generate idempotent client order IDs and preserve causality across retries.
- Validate basic order fields before invoking risk.
- Track accepted, validated, risk-approved, risk-rejected, routed, working, partially filled, filled, canceled, expired, rejected, and unknown/reconciliation-required states.
- Support cancel and amend operations where the venue and instrument permit them.
- Link executions to orders without double-counting fills.
- Rebuild open-order state after restart and reconcile it with venue state.
- Expose read-only monitoring views separately from mutation APIs.

The OMS must not silently retry an order whose acknowledgement or execution state is uncertain. Ambiguous states should become reconciliation tasks rather than duplicate live orders.

### 3.3 RMS — Risk Management System

The RMS is a synchronous gate between OMS and EMS. It must be able to reject or hold an order before dispatch.

Initial pre-trade controls:

- Enabled/disabled trading state and kill switch.
- Instrument and venue eligibility.
- Market/session status and trading halts.
- Fresh market-data requirement for strategies that depend on current prices.
- Maximum order quantity and notional value.
- Price and tick-size validation.
- Maximum position per instrument, strategy, account, and venue.
- Maximum gross/net exposure and concentration limits.
- Daily loss, drawdown, and unrealized-loss limits.
- Order-rate and cancellation-rate limits.
- Duplicate-order and self-trade protection where applicable.
- Credit, buying-power, and margin checks when account data is available.

Post-trade controls and monitoring:

- Fill-driven position and exposure updates.
- Balance and margin reconciliation.
- Stale-data and feed-gap detection.
- Reconciliation-break handling.
- Circuit breakers that reduce or disable order entry.

Risk profiles should be configurable per strategy, account, and venue. The default live profile should be conservative. Risk decisions must be structured, timestamped, and auditable, including the rule that caused a rejection.

### 3.4 EMS — Execution Management System

The EMS translates approved normalized orders into venue-specific requests and translates venue responses back into normalized events.

Required capabilities:

- Venue selection and routing rules.
- Protocol encoding and decoding for each adapter.
- Order entry, cancel, amend/replace, and status-query support according to venue capability.
- Acknowledgement timeout and retry policy that is idempotent and venue-aware.
- Sequence-number and duplicate-event handling.
- Disconnect, reconnect, replay, and session-resumption behavior.
- Bounded queues, backpressure, and explicit behavior for overload.
- Separate transport health from business-level order confirmation.

Direct-exchange and broker adapters may have different semantics for order IDs, partial fills, amendments, rate limits, session state, and error reporting. The EMS should expose capabilities rather than assume a lowest-common-denominator protocol.

### 3.5 Strategy engine

The strategy engine consumes normalized market and account events and emits order intents. It should not own venue connections, risk rules, or order persistence.

Initial requirements:

- Deterministic event ordering and reproducible replay.
- Explicit strategy lifecycle: initialize, subscribe, start, pause, stop, and flatten if configured.
- Per-strategy position and order attribution.
- Configurable parameters with validation and versioning.
- A simple reference strategy for testing, plus an interface for additional strategies.

The first reference strategy should be deliberately simple, such as a quote or signal-to-order adapter, so the OMS/RMS/EMS path can be validated before complex alpha research.

### 3.6 Persistence and recovery

Persistence should be introduced early enough to test recovery, but its storage technology should be selected after a dependency and operational review.

Persist:

- Configuration snapshots and schema versions.
- Instrument/reference data.
- Order lifecycle events.
- Risk decisions and kill-switch transitions.
- Executions, positions, balances, and reconciliation state.
- Strategy parameters and important operational events.

Recommended recovery sequence:

1. Load the latest durable snapshot.
2. Replay durable events to a known checkpoint.
3. Query venue open orders, executions, positions, and balances.
4. Reconcile internal and external state.
5. Resolve or quarantine discrepancies.
6. Enable order entry only after reconciliation passes configured checks.

The system must be crash-safe: a restart must not create duplicate live orders or silently lose an uncertain order.

### 3.7 Observability and operations

Emit structured metrics and logs for:

- Feed sequence gaps, latency, reconnects, and staleness.
- Queue depth, enqueue/dequeue latency, drops, and backpressure.
- Strategy decision latency.
- OMS transition counts and invalid transitions.
- Risk approvals, rejects, and limit utilization.
- EMS encode/submit latency, ACK latency, retries, and timeouts.
- Fill latency, slippage where measurable, positions, P&L, and fees.
- Reconciliation breaks and recovery state.

Operational requirements:

- Health and readiness indicators.
- Audit trail for configuration changes and risk decisions.
- Alerting for stale feeds, sequence gaps, queue saturation, repeated rejects, uncertain orders, venue disconnects, and reconciliation failures.
- A documented kill-switch and emergency-flatten procedure.
- Configuration validation before startup.
- Secrets stored outside source control and ordinary logs.

## 4. High-frequency design principles

HFT is a performance target, not a reason to sacrifice correctness. The implementation should start with a clear, testable pipeline and optimize only after profiling.

### 4.1 Hot path

The intended flow is:

```text
Ingress decode -> normalize -> strategy decision -> OMS validation -> RMS -> EMS encode/submit
```

Design rules:

- Bound all queues and make overflow behavior explicit.
- Avoid unbounded allocation and blocking calls in the hot path.
- Use preallocated buffers and object pools where profiling shows allocation pressure.
- Keep ownership and lifetime of messages explicit.
- Use monotonic clocks for latency measurement and venue timestamps for market-event ordering.
- Preserve causal order for events belonging to the same instrument or order.
- Prefer simple queues initially; consider lock-free structures only after contention is measured.
- Isolate network I/O from deterministic strategy and risk logic.

### 4.2 Performance measurement

Instrument timestamps at:

- Market-data ingress.
- Normalization completion.
- Strategy decision.
- Risk decision.
- EMS dispatch.
- Venue acknowledgement.
- Execution report.

Record latency distributions, not only averages. Maintain a benchmark harness with representative message rates, instruments, order rates, and failure injection. Hardware, OS tuning, network path, exchange location, and venue protocol all affect results; document the test environment with every performance claim.

Advanced techniques such as multicast/UDP, kernel bypass, busy polling, NUMA-aware placement, FPGA, GPU, or specialized exchange protocols should be evaluated only after the baseline design and venue requirements are understood.

## 5. Venue and market adapter model

The system should initially support a broad internal model while adding concrete venues incrementally.

### 5.1 Adapter families

1. **Mock/replay adapters** — deterministic local venues for unit, integration, and paper trading.
2. **Direct exchange adapters** — exchange-native market data, order entry, execution reports, and session rules.
3. **Broker adapters** — broker APIs that may abstract multiple venues and expose different account, order, and market-data semantics.

Each adapter family should implement capability-specific interfaces rather than a single interface that hides important differences.

### 5.2 Venue capabilities

Adapters should report support for:

- Real-time and historical market data.
- Order books, trades, quotes, or bars.
- Limit, market, stop, and venue-specific order types.
- Amend, cancel, cancel-all, and replace behavior.
- Partial fills and execution reports.
- Session calendars, halts, and trading states.
- Rate limits and message throttling.
- Reconnect and replay semantics.
- Account balances, margin, and position queries.

### 5.3 Market coverage strategy

“All markets” should be interpreted as a market-agnostic architecture plus a prioritized venue roadmap, not a claim that every venue is implemented in the first release. Add markets in phases and validate one or two concrete venues at a time. Candidate categories include US equities, Indian equities, equity derivatives, and crypto spot/perpetual venues, but the final priority should be selected based on data access, broker/exchange access, compliance requirements, and user preference.

## 6. Security and compliance

- Use least-privilege API credentials and separate paper/live credentials.
- Store secrets in an OS credential store or approved secrets manager.
- Never commit secrets, session tokens, or sensitive account payloads.
- Validate all external messages, schemas, identifiers, timestamps, quantities, prices, and sequence numbers.
- Use allowlists for destinations where the operating environment permits.
- Keep audit logs immutable or append-only where practical.
- Review market-data redistribution and exchange agreements before enabling a venue.
- Treat jurisdictional trading rules as venue-specific configuration and obtain appropriate legal/compliance review. Rules differ across US, Indian, crypto, and derivatives markets and may change.
- Include a documented process for disabling a venue or strategy without restarting the entire system where possible.

## 7. Proposed repository layout

```text
trading_system_c/
├── CMakeLists.txt
├── README.md
├── REPOSITORY_PLAN.md
├── src/
│   ├── core/          # types, clock, config, logging, errors, queues
│   ├── oms/           # order lifecycle and state machine
│   ├── rms/           # pre/post-trade risk controls
│   ├── ems/           # routing, dispatch, acknowledgements, retries
│   ├── strategy/      # strategy interface and reference strategy
│   ├── adapters/
│   │   ├── mock/
│   │   ├── replay/
│   │   ├── exchange/
│   │   └── broker/
│   ├── persistence/   # event log, snapshots, recovery
│   └── observability/ # metrics, health, alerts
├── include/           # public/internal module interfaces
├── tests/
│   ├── unit/
│   ├── contract/
│   ├── replay/
│   └── integration/
├── fixtures/          # sanitized market/order fixtures
├── docs/
│   ├── architecture.md
│   ├── risk.md
│   ├── operations.md
│   └── venue-onboarding.md
└── tools/             # local benchmark and diagnostic utilities
```

The exact layout may be refined during implementation, but module boundaries should remain explicit. Prefer C11 or a newer portable C standard supported by the selected toolchain; document compiler and platform assumptions.

## 8. Implementation phases

### Phase 0 — Foundation

Deliverables:

- Repository structure and build system.
- Core types, error handling, clock abstraction, configuration loader, structured logging, and test framework.
- CI pipeline for formatting, static analysis, unit tests, and clean builds.
- Architecture and risk documentation.

Exit criteria:

- A clean build and test run on the target development platform.
- No trading behavior is enabled yet.

### Phase 1 — Simulated OMS/EMS

Deliverables:

- Normalized order and execution contracts.
- OMS state machine with idempotency and lifecycle tests.
- RMS pre-trade engine with configurable limits and audit records.
- Mock venue and deterministic paper matcher.
- EMS dispatch path, ACK/timeout handling, and reconciliation hooks.
- Durable event journal or embedded persistence after dependency selection.

Exit criteria:

- Orders can be submitted, risk-checked, routed to a mock venue, partially filled, filled, canceled, and recovered after restart.
- Duplicate submissions and ambiguous acknowledgements cannot create duplicate orders.
- Unit, state-machine, and restart/reconciliation tests pass.

### Phase 2 — Market data and strategy framework

Deliverables:

- Replay adapter and normalized quote/trade/book events.
- Market-data sequence and staleness handling.
- Strategy interface and reference strategy.
- Signal-to-order path through OMS, RMS, and EMS.
- Deterministic replay tests and latency instrumentation.

Exit criteria:

- A recorded market-data session can drive the reference strategy and paper orders reproducibly.
- Feed gaps, stale data, and disconnected replay are handled explicitly.

### Phase 3 — First real venue

Deliverables:

- Select one priority venue and access method.
- Implement market-data and order-entry adapter contracts.
- Implement session calendar, authentication, rate limits, reconnect, and reconciliation.
- Run paper trading against the venue where supported; otherwise use a certified simulator.
- Add live-mode gates, credentials handling, and venue-specific risk configuration.

Exit criteria:

- Adapter contract tests pass against recorded fixtures and a fake venue.
- Reconciliation passes for orders, fills, positions, and balances.
- Paper soak test completes without unexplained state divergence.
- Live mode remains disabled until explicit operational approval.

### Phase 4 — HFT hardening

Deliverables:

- Hot-path profiling and bottleneck report.
- Queue and allocation optimization based on measurements.
- Capacity, soak, fault-injection, and restart tests.
- Alerting, dashboards, runbooks, kill switch, and emergency-flatten procedure.
- Hardware/network assumptions documented.

Exit criteria:

- Performance targets are measured and documented rather than assumed.
- The system remains correct under queue pressure, venue disconnects, rejects, duplicate events, and process crashes.
- Operations team can detect, diagnose, and stop unsafe behavior.

### Phase 5 — Additional markets and venues

Deliverables:

- Add direct-exchange and broker adapters one venue at a time.
- Expand asset-class and instrument normalization as required.
- Add venue-specific session, compliance, and risk profiles.
- Repeat contract, paper, reconciliation, and soak gates for each venue.

Exit criteria:

- Each new venue has documented capabilities, test evidence, risk configuration, and an explicit enablement decision.

## 9. Testing strategy

### Unit tests

Cover:

- Order state transitions and illegal transitions.
- Risk rule evaluation and precedence.
- Instrument and message validation.
- ID generation, deduplication, and ordering.
- Clock and timestamp handling.
- Persistence serialization and recovery.

### Contract tests

Every adapter must pass tests for:

- Authentication/session startup.
- Market-data subscription and normalization.
- Order entry, acknowledgement, rejection, partial fill, fill, cancel, and amend behavior.
- Duplicate and out-of-order events.
- Sequence gaps and reconnect/replay.
- Rate limits and venue-specific errors.
- Account, position, and balance reconciliation.

Use recorded, sanitized fixtures plus a fake venue. Never require live credentials for the default test suite.

### Integration and fault-injection tests

Test:

- Process restart during every order lifecycle stage.
- Venue disconnect before and after dispatch.
- ACK timeout and uncertain order state.
- Duplicate execution reports.
- Partial fills followed by cancel or reject.
- Stale market data and feed gaps.
- Queue saturation and backpressure.
- Risk-limit changes while orders are working.
- Kill-switch activation during order flow.

### Performance tests

Create reproducible benchmarks for ingress decode, normalization, strategy decision, risk decision, EMS dispatch, and end-to-end order-to-ACK latency. Test multiple instrument counts, message rates, and order rates. Record p50, p95, p99, maximum, CPU, memory, queue depth, and drop counts. Compare results across runs and hardware configurations.

## 10. Dependency and technology decisions

Before implementation, review and select:

- C standard and compiler toolchain.
- Build system, likely CMake for cross-platform development.
- Unit and contract test framework.
- Logging and metrics libraries, or a small internal implementation if dependencies are undesirable.
- JSON/configuration parser, with strict validation.
- Embedded database or append-only journal for persistence.
- Networking and TLS libraries required by selected venues.
- Time synchronization and clock source strategy.

Prefer mature, well-maintained dependencies with compatible licenses. Pin versions and document why each dependency is needed. Avoid adding a dependency merely for convenience in a latency-sensitive path without profiling the cost.

## 11. Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Scope expands to every market at once | Delayed delivery and untestable adapters | Market-agnostic core plus phased venue onboarding |
| HFT assumptions are not validated | Poor latency or unnecessary complexity | Profile baseline, measure bottlenecks, adopt advanced techniques only when justified |
| Venue semantics differ materially | Incorrect orders or reconciliation | Capability model, adapter contracts, venue-specific tests and runbooks |
| Crash or disconnect creates duplicate orders | Financial loss | Idempotency, durable events, uncertain-state quarantine, reconciliation before order entry |
| Risk data is stale or missing | Unsafe orders | Fail-safe defaults, freshness checks, conservative live profile, kill switch |
| Secrets or account data leak | Security incident | Secrets manager, least privilege, redaction, access controls, audit logs |
| Market rules differ by jurisdiction | Compliance exposure | Venue-specific configuration and legal review before enablement |
| C memory or concurrency bugs | Crashes or corruption | Clear ownership, sanitizers, static analysis, fuzz/property tests, bounded queues |
| Paper results are overinterpreted | False confidence | Document fill model limitations and require live-staging gates |

## 12. Acceptance criteria

The first production-shaped milestone is complete when:

- The C application builds cleanly with tests enabled.
- OMS, RMS, and EMS are separated by explicit interfaces and have deterministic behavior.
- A strategy can submit an order through the full simulated path.
- Risk limits can approve, reject, and hold orders with auditable reasons.
- Mock/replay venues support fills, partial fills, rejects, cancels, and amendments as applicable.
- Restart and reconciliation tests pass without duplicate orders.
- Market-data gaps and stale data are visible and handled safely.
- Metrics, health state, logs, and kill-switch controls are available.
- No live credentials or live order entry are enabled by default.
- Documentation explains architecture, configuration, testing, operations, and venue onboarding.

Live trading is a separate milestone requiring venue-specific evidence, reconciliation results, risk approval, operational readiness, and explicit user authorization.

## 13. Recommended first engineering tasks

1. Create the repository layout and CMake build.
2. Define core value types, error codes, clock, logger, and configuration schema.
3. Implement the normalized order and event contracts.
4. Implement the OMS state machine with unit tests.
5. Implement the RMS rule engine with configurable limits and audit records.
6. Implement a mock venue and paper EMS path.
7. Add persistence and restart/reconciliation tests.
8. Add replay market data and the reference strategy.
9. Instrument latency and run the first benchmark.
10. Select the first real venue and begin adapter discovery.

This order establishes correctness and recoverability before performance optimization or venue-specific complexity.
