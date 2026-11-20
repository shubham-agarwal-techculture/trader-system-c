# Data model

All domain types are plain C structs in `include/trading/events.h`.
Strings are fixed-size char arrays (`TS_MAX_CODE`=24, `TS_MAX_ID`=40,
`TS_MAX_NAME`=16, `TS_MAX_REASON`=128) — copy with `ts_copy_str`, never
`strcpy`.

## ts_instrument

| Field | Meaning |
|---|---|
| `code` | Canonical instrument id, e.g. `BTCUSDT`, `NSE:RELIANCE` |
| `venue` | Venue/exchange identifier |
| `asset` | `TS_ASSET_*` class |
| `currency` / `settlement_currency` | Quote and settlement ccys |
| `tick_size` / `lot_size` | Minimum price increment / order increment |
| `contract_multiplier` | e.g. 1 for spot, 50 for some futures |
| `tradable` | Venue says instrument is tradable |

`ts_instrument_validate()` requires non-empty code, positive
tick/lot/multiplier.

## ts_mkt_event

Normalized market data. `type` selects interpretation:

- `TS_MKT_QUOTE` — `bid`, `ask`, optional `last`, sizes in `*_qty`
- `TS_MKT_TRADE` — `last` = trade price, `last_qty` = size
- `TS_MKT_SNAPSHOT` — full snapshot; same fields as quote plus sizes
- `TS_MKT_BOOK` — top-N book snapshot: `book_depth` levels in
  `bid_px/bid_qty_lv/ask_px/ask_qty_lv` (best first, max `TS_BOOK_DEPTH`=8);
  `bid`/`ask`/`bid_qty`/`ask_qty` mirror level 0

Replay format adds: `BOOK <instrument> <depth> <bpx1> <bqy1> <apx1> <aqy1> ...`

`ts_ns` is the exchange/feed timestamp; `seq` is the feed sequence number
(gap detection is a Phase 4 item — the field exists today).

## ts_order_request → ts_order

A strategy emits `ts_order_request`. The OMS expands it into `ts_order`
adding: `venue_order_id` (EMS/venue assigned), `filled_qty`,
`avg_fill_price`, `state`, `reject_reason`, `created_ns`, `updated_ns`.

`client_id` is the idempotency key: it must be unique per logical order
and stable across retries. Strategies should embed a sequence number
(e.g. `mom-000001`).

## ts_execution

One fill report. `client_id` + `venue_order_id` link it to the order;
`venue_exec_id` dedups at the venue level. `qty`/`price`/`fee` are in
instrument units and quote currency. Multiple executions per order are
normal (partial fills).

## ts_venue_event

What a venue adapter emits to the EMS. `type` ∈ ACK / EXEC / REJECT /
CANCEL_ACK / EXPIRED / UNKNOWN. For `TS_VEV_EXEC` the embedded
`ts_execution` carries fill details. `reason` carries reject/expire text.
`UNKNOWN` means "venue state uncertain — reconcile before retrying".

## Journal record formats

Tab-separated, one event per line, written by
`src/persistence/journal.c`, parsed by `journal_replay.c`.
Doubles serialize with `%.17g` (round-trip safe).

```
SNAP\t<updated_ns>\t<client_id>\t<venue_order_id>\t<instrument>\t<strategy>\n      \t<side:int>\t<type:int>\t<tif:int>\t<qty>\t<price>\t<filled_qty>\n      \t<avg_fill_price>\t<state:int>\t<reject_reason>
EXEC\t<ts_ns>\t<client_id>\t<venue_exec_id>\t<instrument>\t<side:int>\n      \t<qty>\t<price>\t<fee>
RISK\t<ts_ns>\t<client_id>\t<approved:0|1>\t<rule>\t<msg>
KILL\t<ts_ns>\t<killed:0|1>
ACCT\t<ts_ns>\t<currency>\t<amount>
```

Notes:

- `SNAP` stores the full order — recovery does not need to replay
  transitions, only the latest snapshot per `client_id`.
- `EXEC` is the source for position/P&L rebuild (replayed through
  `rms_on_execution`).
- `RISK`/`ACCT` are audit/account records; `RISK` is informational on
  replay.
- Malformed lines are skipped and counted (`skipped` out-param).

## Metrics schema

`ts_metrics` holds `TS_MET_COUNT` named counters plus 8 latency slots
(count/sum/max ns). Slot names are bound on first use:
0 `decision_ns`, 1 `risk_ns`, 2 `dispatch_ns`, 3 `ack_ns`, 4 `fill_ns`.
