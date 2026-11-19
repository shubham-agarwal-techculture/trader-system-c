# trading_system_c

A C11 trading system with an Order Management System (OMS), Risk Management
System (RMS), and Execution Management System (EMS), designed for
high-frequency trading and multi-venue operation. See `REPOSITORY_PLAN.md`
for the full design and roadmap.

## Status

Implemented (Phases 0–2 of the plan):

- Core types, monotonic/virtual clocks, logging, metrics
- Normalized contracts: instruments, market events, orders, executions,
  venue events
- OMS: idempotent order lifecycle, journaled state machine, recovery
- RMS: pre-trade limits (qty, notional, position, rate, staleness, daily
  loss) + kill switch, position/P&L tracking
- EMS: venue adapter interface + deterministic `MockVenue` paper matcher
- Persistence: append-only journal with replay-based recovery
- Strategy: replay feed adapter + reference momentum strategy
- `trading_sim` CLI and a CTest unit/integration suite

Not yet implemented (later phases): real exchange/broker adapters,
network transports, co-location/kernel-bypass, production session
handling, and live-trading enablement.

## Build

Requires a C11 compiler and CMake >= 3.16.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --config RelWithDebInfo
ctest --test-dir build --output-on-failure
```

## Run the simulator

```sh
./build/trading_sim run --journal data/journal.log \
    --replay fixtures/sample_feed.txt --instrument BTCUSDT

# Rebuild OMS/RMS state from the journal:
./build/trading_sim recover --journal data/journal.log
```

## Layout

```
include/trading/   public module interfaces
src/core/          clocks, logging, status, contract validation
src/oms/           order lifecycle
src/rms/           risk engine
src/ems/           execution dispatch + venue adapters (mock)
src/strategy/      strategy interface, replay adapter, momentum
src/persistence/   append-only journal + replay
src/observability/ metrics
src/app/           trading_sim CLI
tests/             unit + integration tests (CTest)
docs/              architecture, risk, operations, venue onboarding
```

## Design rules

- The OMS owns order state; RMS is a synchronous gate before EMS; EMS is
  the only component that talks to venues.
- Every order/execution/risk/kill event is journaled; recovery replays the
  journal before order entry is re-enabled.
- All queues and tables are fixed-capacity; overflow is explicit.
- Paper trading verifies plumbing only — it is not evidence of live
  performance or venue compatibility.
