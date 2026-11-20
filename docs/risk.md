# Risk model

## Pre-trade checks (rms_check_order, evaluated in order)

1. **KILL_SWITCH** — absolute gate; no orders pass while engaged.
2. **ORDER_VALIDATION** — quantity > 0, valid side, submittable state.
3. **ORDER_RATE** — sliding 1s window of submissions vs `max_order_rate`.
4. **MAX_ORDER_QTY** — `qty <= max_order_qty`.
5. **MAX_NOTIONAL** — `qty * price <= max_order_notional`
   (market orders use last mark).
6. **MAX_POSITION** — `|position + signed qty| <= max_position_qty`.
7. **MARKET_DATA_REQUIRED / STALE_MARKET_DATA** — only when
   `require_fresh_market` is set; uses per-instrument quote timestamps.
8. **DAILY_LOSS** — `realized + unrealized <= daily_loss_loss_limit`.

First failing rule wins; the rule name and detail are journaled via
`oms_mark_risk` (`RISK` records) and returned in `ts_risk_result`.

## Self-trade prevention

`oms_would_self_trade()` runs in the order path before `rms_check_order`:
a new order is rejected with rule `SELF_TRADE` if it would cross a
non-terminal opposite-side order on the same instrument. Market orders
cross conservatively against any opposite-side working order; limit
orders cross on price overlap.

## Post-trade

- `rms_on_execution` updates signed position, average cost, and realized
  P&L (including flips).
- `rms_on_quote` updates last price and quote timestamp per instrument.
- `rms_on_account` is reserved for buying-power/margin checks.

## Positions and P&L

- Position: signed quantity per instrument.
- Average cost: weighted on adds; on reduce/flip, realized P&L is booked
  for the closed portion and the remainder reopens at fill price.
- Unrealized P&L: `(last_price - avg_cost) * qty` per instrument.

## Kill switch

`rms_kill()` / `rms_resume()` are journaled (`KILL` records) so the state
survives restart. Recovery replays the latest transition. A killed RMS
rejects every order with `KILL_SWITCH`.

## Fail-safe defaults

`ts_risk_limits_default` ships conservative values. Live profiles must be
set explicitly per strategy/account/venue. Missing market data is only
fatal when `require_fresh_market` is enabled — enable it for any strategy
whose orders depend on current prices.

## Known gaps (later phases)

- Buying-power/margin checks against account balances.
- Self-trade prevention across strategies.
- Intraday vs. daily P&L reset calendars.
- Per-venue/session limits and jurisdiction-specific rules.
