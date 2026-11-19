# Documentation index

- [architecture.md](architecture.md) — pipeline, module ownership, order
  states, persistence, hot-path rules
- [order-lifecycle.md](order-lifecycle.md) — state machine, transition
  table, idempotency, fills, UNKNOWN/reconciliation
- [data-model.md](data-model.md) — all structs/enums, journal record
  formats, metrics schema
- [api-reference.md](api-reference.md) — every public function and type
- [risk.md](risk.md) — pre/post-trade checks, P&L model, kill switch,
  fail-safe defaults, gaps
- [testing.md](testing.md) — suite layout, conventions, coverage map,
  replay format, planned fault injection
- [performance.md](performance.md) — hot path, bounds, allocation,
  latency slots, deferred optimizations, benchmark plan
- [configuration.md](configuration.md) — CLI, risk limits, strategy and
  venue knobs, clock, journal
- [operations.md](operations.md) — metrics, kill switch, recovery
  procedure, alerting hooks, secrets policy
- [venue-onboarding.md](venue-onboarding.md) — how to add exchange/broker
  adapters
