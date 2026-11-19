# Configuration

There is no config file loader yet (Phase 4). Configuration is
programmatic: risk limits via `rms_set_limits`, strategy params via
`momentum_config`, venue behavior via `venue_mock_*`, and journal/clock
via the `*_config` structs. This document defines the knobs that exist.

## trading_sim CLI

```
trading_sim run --journal FILE --replay FILE [--instrument CODE]
trading_sim recover --journal FILE
```

| Flag | Default | Meaning |
|---|---|---|
| `--journal` | `trading_journal.log` | Append-only event journal path |
| `--replay` | (required for `run`) | Replay feed file (QUOTE/TRADE lines) |
| `--instrument` | `BTCUSDT` | Instrument the momentum strategy trades |

`recover` replays the journal into a fresh OMS/RMS and prints recovered
order/position counts. It never writes to the journal.

## Risk limits (ts_risk_limits)

Set with `rms_set_limits()`. Defaults from `ts_risk_limits_default()`:

| Field | Default | Check |
|---|---|---|
| `max_order_qty` | 1000.0 | `qty <= max` |
| `max_order_notional` | 100000.0 | `qty*price <= max` (market orders use last mark) |
| `max_position_qty` | 5000.0 | `|pos + signed qty| <= max` per instrument |
| `daily_loss_limit` | 10000.0 | `realized + unrealized >= -limit` |
| `max_order_rate` | 0.0 (off) | submissions/sec in sliding 1s window |
| `require_fresh_market` | false | enforce quote freshness |
| `max_quote_age_ns` | 0 | max age when freshness required |

The simulator overrides defaults in `main.c` (looser limits so the demo
strategy can trade). **Live profiles must be set explicitly** — treat
defaults as a starting point, not approval.

## Momentum strategy (momentum_config)

| Field | Default | Meaning |
|---|---|---|
| `instrument` | required | instrument to trade |
| `strategy_name` | `"momentum"` | used in `client_id`/`strategy` fields |
| `qty` | 1.0 | order size |
| `move_threshold` | 0.5 | mid-price move that triggers an order |

Emits `TS_ORD_MARKET` / `TS_TIF_IOC` orders alternating direction on each
qualifying quote move. Client IDs are `<strategy>-<seq>`.

## Mock venue knobs

- `venue_mock_set_reject_all(v, true)` — reject every order (tests).
- `venue_mock_set_latency_ns(v, ns)` — reserved; simulated venue latency
  is currently zero (events are queued synchronously).

## Journal

Append mode `"a"` — safe to reopen; replay tolerates malformed lines
(counted in `skipped`). There is no rotation/checkpointing yet; for
production add size-based rotation + a `SNAPSHOT` record type (Phase 4).

## Clock

- `ts_clock_mono()` — wall-independent monotonic time (real runs).
- `ts_clock_virtual(&vc)` — deterministic; `trading_sim` sets it to each
  replay event's `ts_ns`. Tests use it exclusively.

## What does NOT exist yet

- Config file (JSON/TOML) loading and validation
- Per-strategy/per-venue risk profiles (single global RMS today)
- Secrets/credentials handling (no live adapters yet)
- Session calendars, trading halts, instrument whitelists
- Dynamic limit reload without restart
