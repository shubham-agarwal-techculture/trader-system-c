# Operations runbook

## Startup sequence (trading_sim run)

1. Load `--config` (optional INI file).
2. Open journal (append mode; `flush`/`fsync` per config).
3. Create OMS/RMS with journal+metrics+clock.
4. Apply `[risk]` limits; create venue per `[venue] type`.
5. Live gate: non-simulated venue names require `[live] enabled = true`.
6. Replay feed loop: `seqwatch` -> `rms_on_quote` ->
   `ems_on_market_event` -> strategy -> order path -> `ems_poll`.
7. End of feed: `ems_reconcile` report + metrics dump.

## Order path (per strategy order)

```
oms_submit (idempotent, journaled SNAP)
  -> oms_would_self_trade? -> reject SELF_TRADE
  -> rms_check_order -> oms_mark_risk (RISK journal + SNAP)
  -> ems_submit -> venue.submit
ems_poll -> venue events -> OMS/RMS transitions (all journaled)
```

## Recovery

`trading_sim recover --journal FILE`:

- Replays `SNAP` -> OMS order states, `EXEC` -> RMS positions/P&L,
  `KILL` -> kill switch, `ACCT` -> account records.
- Malformed lines counted as `skipped`, never fatal.
- Prints orders/open count, positions, realized P&L, metrics.

Live recovery additionally requires venue reconciliation (Phase 3+):
compare recovered open orders vs `venue->ops->open_orders` via
`ems_reconcile`; resolve `UNKNOWN` orders before re-enabling entry.

## Kill switch

`rms_kill()` / `rms_resume()` — journaled `KILL` records survive restart.
Killed RMS rejects everything with `KILL_SWITCH`. Working orders are NOT
auto-canceled: use `ems_cancel_all()` for emergency flatten, then verify
via `ems_reconcile`.

## Health / alerting signals

| Signal | Meaning | Action |
|---|---|---|
| `mkt_seq_gaps` > 0 | feed sequence gaps | check feed health, consider resubscribe |
| `mkt_stale` | stale marks | verify quote timestamps |
| `recon_breaks` | overfills, illegal transitions, queue drops, reconcile mismatches | investigate immediately |
| `venue_rejects` spike | venue-side rejects | check limits/rate/session |
| `risk_rejected` spike | strategy vs limits | review strategy params |
| open orders / UNKNOWN count | reconciliation debt | run `ems_reconcile` |
| `exec_duplicate` | venue resent fills | normally benign; watch for venue bugs |

## Journal durability

- `flush=true` (default): `fflush` per record — survives process crash,
  not OS crash.
- `fsync=true`: `fsync`/`_commit` per record — survives OS crash; expensive.
- `max_size_mb=N`: at open, files larger than N MB are renamed to
  `<file>.1` (single generation) before appending. Recovery must replay
  both `.1` and current file if both exist.

## Live mode (Kraken)

`trading_sim live` requires `[live] enabled = true`, Kraken env creds,
and a `[venue] type = kraken` config. Market data comes from the Kraken
WebSocket v2 ticker feed (public channel) when available, else REST
Ticker polling at `--interval-ms`. The loop runs seqwatch -> RMS marks
-> strategy -> OMS/RMS/EMS -> `ems_poll` (TradesHistory diff for fills),
and ends with `ems_reconcile` + metrics. Orders are REAL — start with
minimum-size limits and `rms_kill` ready.

## Emergency procedures

1. **Stop new orders**: `rms_kill()` (or stop process — journal has state).
2. **Flatten**: `ems_cancel_all()` then verify `oms_open_count == 0` and
   `ems_reconcile` clean.
3. **Recover**: restart, `recover` journal, reconcile vs venue, resolve
   UNKNOWN orders, `rms_resume()` only after sign-off.

## Secrets

No credentials in this milestone. Future adapters MUST read secrets from
OS credential store / secrets manager — never config files, env dumps,
logs, or the journal. `[live] enabled` is necessary but not sufficient:
credentials + venue entitlement + compliance review are separate gates.
