# Venue adapters

All venues implement `venue_ops_t` (`include/trading/venue.h`). The EMS
is the only component that talks to venues; strategies and risk never see
venue protocols.

## Contract

```c
typedef struct venue_ops {
    const char *name;
    ts_status (*submit)(venue_t*, const ts_order*);          // required
    ts_status (*cancel)(venue_t*, const char *client_id);   // required
    void (*on_market_event)(venue_t*, const ts_mkt_event*); // required
    ts_status (*poll)(venue_t*, ts_venue_event *out);       // required
    bool (*connected)(venue_t*);                            // required
    ts_status (*amend)(venue_t*, const char*, double, double);      // opt
    ts_status (*cancel_all)(venue_t*);                              // opt
    ts_status (*open_orders)(venue_t*, ts_venue_open_order*,
                             size_t cap, size_t *count);            // opt
    void (*destroy)(venue_t*);
} venue_ops_t;
```

Rules every adapter MUST follow:

- `submit` is idempotent on `client_id` — resubmits must not duplicate.
- `poll` returns `TS_ERR_NOT_FOUND` when empty; events are drained FIFO.
- ACK/EXEC/REJECT/CANCEL_ACK/EXPIRED/UNKNOWN/AMENDED events go through
  `poll`, never callbacks into OMS directly.
- `open_orders == NULL` means "reconciliation unsupported" — callers must
  not treat it as "no open orders".
- Venue must not block; slow work belongs behind its own queue.

## Implemented adapters

### mock — `src/ems/venue_mock.c`

Minimal deterministic paper matcher. Auto-ACKs on submit, fills
marketable orders immediately against the last quote (full fills only),
rests GTC orders, honors IOC/FOK expiry. Knobs: `reject_all`,
`set_connected`. No latency, no partial fills.

### replay — `src/ems/venue_replay.c`

Scripted event source for contract/fault tests. Loads all events at
open; `submit`/`cancel` only record orders — the script supplies every
outbound event, so tests control exact ordering (duplicates,
out-of-order, rejects, UNKNOWN). Implements `open_orders` with closure
tracking (terminal events + full fills drained via `poll`).

Script format:

```
ACK      <client_id> <venue_order_id>
EXEC     <client_id> <exec_id> <instrument> <BUY|SELL> <qty> <price> [fee]
REJECT   <client_id> <reason words...>
CANCEL   <client_id>
EXPIRED  <client_id> [reason...]
UNKNOWN  <client_id> [reason...]
```

### dummy — `src/ems/venue_dummy.c`

Realistic simulated venue for exercising the live-order path:

| Knob | Config key | Effect |
|---|---|---|
| `latency_ns` | `[venue] latency_ns` | one-way event delay; `poll` hides events until `ready_ns` |
| `fill_fraction` | `[venue] fill_fraction` | (0,1] fraction of remainder filled per match attempt |
| `reject_every` | `[venue] reject_every` | reject every Nth submission deterministically |
| `max_orders_per_sec` | `[venue] max_orders_per_sec` | sliding-window submit rate limit |
| `connected` | `venue_dummy_set_connected` | disconnect simulation |

Supports amend/cancel-all/open_orders, BOOK-aware top-of-book matching,
IOC/FOK/DAY expiry. Like mock, fills at top-of-book with no queue
position or adverse selection — plumbing verification only.

## Selecting a venue in trading_sim

```ini
[venue]
type = mock | replay | dummy    # default: mock
reject_all = false              # mock only
script = venue.txt              # replay only
latency_ns = 0                  # dummy only
fill_fraction = 1.0             # dummy only
reject_every = 0                # dummy only
max_orders_per_sec = 0          # dummy only (0 = unlimited)
```

`--venue-script FILE` forces replay regardless of config.

## Live gate

`trading_sim` refuses any venue whose `ops->name` is not
`mock`/`replay`/`dummy` unless `[live] enabled = true` is set. Real
adapters slot in here: implement `venue_ops_t`, register a name, and the
gate enforces explicit enablement.

## Venue event types (TS_VEV_*)

| Event | OMS effect | Carries |
|---|---|---|
| `ACK` | -> WORKING | venue_order_id |
| `EXEC` | -> PARTIAL/FILLED | embedded `ts_execution` |
| `REJECT` | -> REJECTED | reason |
| `CANCEL_ACK` | -> CANCELED | — |
| `AMENDED` | price/qty update | new price/qty in `exec.price`/`exec.qty` |
| `EXPIRED` | -> EXPIRED | reason |
| `UNKNOWN` | -> UNKNOWN (reconcile) | reason |

## Adding a real adapter — checklist

1. Choose venue + TLS/WebSocket library (decision pending).
2. Implement `venue_ops_t` in `src/ems/venue_<name>.c`; keep ALL
   protocol quirks inside the adapter.
3. Contract tests: reuse replay-venue scenarios as the template —
   submit/ACK/partial fills/full fill/reject/cancel/dup exec/
   out-of-order/UNKNOWN/disconnect/reconnect.
4. `open_orders` + `ems_reconcile` for post-restart reconciliation.
5. Config: `[venue] type = <name>` + credentials via secrets manager
   (never config files or journal).
6. `[live] enabled = true` required before `trading_sim` will run it.
