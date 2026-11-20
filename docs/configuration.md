# Configuration

`trading_sim` accepts `--config FILE` with an INI-style file (sections,
`key = value`, `#`/`;` comments). Without `--config`, built-in defaults
apply. Loader: `src/core/config.c` (`ts_config_load` + typed getters).

Example `sim.cfg`:

```ini
[risk]
max_order_qty = 100
max_order_notional = 1000000
max_position_qty = 500
daily_loss_limit = 5000
max_order_rate = 1000
require_fresh_market = false
max_quote_age_ns = 0

[strategy]
instrument = BTCUSDT
qty = 0.5
move_threshold = 0.5     # momentum only
imbalance_threshold = 0.3 # book_imbalance only
name = momentum          # momentum | book_imbalance

[venue]
type = mock          # mock | replay
reject_all = false   # mock only
script = venue.txt   # replay only

[journal]
flush = true         # fflush per record (default on)
fsync = false        # fsync per record (expensive; live option)
max_size_mb = 0      # rotate to <file>.1 when exceeded at open (0 = off)

[live]
enabled = false      # REQUIRED for kraken (and any future real venue)
```

Kraken credentials come from env vars `KRAKEN_API_KEY` /
`KRAKEN_API_SECRET` — never from config files.

CLI flags override nothing in the file — flags win for journal/replay/
instrument/venue-script paths; config supplies risk limits, strategy
params, venue type, and journal durability.

## trading_sim CLI

```
trading_sim run --journal FILE --replay FILE [--instrument CODE] [--config FILE] [--venue-script FILE]
trading_sim live --journal FILE --config FILE [--instrument CODE] [--duration-s N] [--interval-ms N]
trading_sim recover --journal FILE
```

`live` pulls market data from the Kraken venue (REST ticker poll) and
runs the full pipeline against the real exchange. Requires
`[live] enabled = true` + `KRAKEN_API_KEY`/`KRAKEN_API_SECRET` env vars.
`--duration-s 0` runs until interrupted; `--interval-ms` sets the poll
cadence (default 1000, Kraken public rate limit ~1 req/s).

| Flag | Default | Meaning |
|---|---|---|
| `--journal` | `trading_journal.log` | Append-only event journal path |
| `--replay` | (required for `run`) | Replay feed file (QUOTE/TRADE lines) |
| `--instrument` | `BTCUSDT` | Instrument the momentum strategy trades |
| `--config` | none | INI config file |
| `--venue-script` | none | Venue event script (forces replay venue) |

`run` ends with an `ems_reconcile` report comparing OMS open orders to
venue open orders; mismatches mark orders `UNKNOWN` and count
`recon_breaks`.

`recover` replays the journal into a fresh OMS/RMS and prints recovered
order/position counts. It never writes to the journal.

## Risk limits (ts_risk_limits / [risk])

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

- `venue_mock_set_reject_all(v, true)` / `[venue] reject_all` — reject
  every order (tests).
- `venue_mock_set_latency_ns(v, ns)` — reserved; simulated venue latency
  is currently zero (events are queued synchronously).

## Replay venue

`venue_replay_create(path, metrics, clock)` replays a scripted venue-event
file — see `docs/venue-onboarding.md` for the script format. Unlike the
mock venue it does NOT auto-ACK on submit; scripts must contain explicit
`ACK`/`EXEC`/`REJECT`/`CANCEL`/`EXPIRED`/`UNKNOWN` lines, giving tests
full control of event ordering for fault injection.

## Journal durability

Append mode flushes (`fflush`) after every record by default —
`journal_set_flush(j, false)` disables, `journal_set_fsync(j, true)`
enables per-record `fsync`/`_commit` for live durability. Config:
`[journal] flush`, `[journal] fsync`.

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
