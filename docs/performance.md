# Performance design

HFT is a target, not an assumption. This document records the current
design decisions and the measurement plan.

## Measured results (MSVC RelWithDebInfo, Windows)

`trading_bench --events 100000` drives synthetic quotes through the full
pipeline (seqwatch -> RMS marks -> dummy venue -> momentum strategy ->
OMS/RMS/EMS -> poll) with the durable journal enabled.

| Metric | Before OMS hash index | After client_id hash | After venue-id hash |
|---|---|---|---|
| Throughput | ~1,337 events/sec | ~6,072 events/sec | ~5,950-6,120 events/sec |
| Orders in bench | 65,536 (OMS cap) | 65,536 | 65,536 |
| Executions | 54,957 | 54,957 | 54,957 |

The dominant cost was the OMS `client_id` linear scan per submit —
O(orders) per order, O(n^2) overall. Fixed with an open-addressing
FNV-1a hash index (`client_id -> orders[] slot`, power-of-two capacity
>= 2x order cap; orders are never removed so no deletion needed). A
second identical index on `venue_order_id` was added for the
ACK/EXEC dispatch path — lookups are now O(1) but throughput is
unchanged, proving the remaining cost is elsewhere.

Journal I/O is NOT the bottleneck either: `--nojournal` yields ~6,119
events/sec vs ~5,951 with journal (~3% delta). The residual ~160us/event
is per-event struct copying (`ts_order` ~300B, `ts_venue_event` ~450B
memcpy per dispatch), risk/position scans, and venue event-queue churn —
the price of the safe, allocation-free, correctness-first design. Real
HFT latency work (lock-free queues, zero-copy dispatch, kernel bypass)
remains gated on profiling this path under realistic load.

`oms_find_venue`/`find_by_venue` are now hash-indexed; no linear order
lookups remain on hot paths.

Reproduce: `./build/trading_bench --events 100000`

Virtual-clock latency slots report ~0-1ns because all stages run within
the same synthetic tick; real wall-time distributions require
`trading_sim live` against a real venue.

## Current hot path

```
replay_next / venue feed
  -> rms_on_quote          (mark update, O(positions))
  -> ems_on_market_event   (venue sees quote)
  -> strategy.on_event     (user code; momentum is O(1))
     -> oms_submit         (array scan by client_id, O(n))
     -> rms_check_order    (O(1) rules + O(rate window))
     -> oms_mark_risk      (journal append + snap)
     -> ems_submit         (venue submit, O(working orders))
  -> ems_poll              (drain venue events -> OMS/RMS)
```

Single-threaded, deterministic, virtual-clock driven in replay mode.
No locks anywhere — safe by construction, not by synchronization.

## Bounds

- OMS orders: fixed `capacity` array (default 4096 in app, 64–1024 in tests).
- RMS positions: `TS_RMS_MAX_POSITIONS` = 256.
- RMS rate window: `TS_RMS_RATE_WINDOW` = 1024 timestamps.
- Mock venue: 512 working orders, 4096 queued events, 64 instruments.
- Journal replay lines: 2048 bytes, 24 fields.

Overflow behavior: `TS_ERR_FULL` on OMS submit; positions beyond 256 are
dropped silently (returns `TS_ERR_FULL` to caller — review if a venue
needs more); venue event queue drops with `TS_MET_RECON_BREAKS`.

## Allocation

No heap allocation on the order hot path after init. All state is
preallocated arrays inside `oms_t`/`rms_t`/venue impl. `oms_submit`
copies the request into the order slot.

## Latency instrumentation

`ts_metrics_latency` records count/sum/max ns at slots:

| Slot | Stage | Currently measured |
|---|---|---|
| 0 | decision_ns | strategy->submit wall time |
| 1 | risk_ns | reserved |
| 2 | dispatch_ns | `ems_submit` venue call |
| 3 | ack_ns | reserved (needs ACK timestamping) |
| 4 | fill_ns | event dispatch in `ems_poll` |

In replay mode the virtual clock makes these ~0; they become meaningful
with the real clock + real adapters.

## Deliberately deferred

These are Phase 4 decisions gated on profiling:

- Lock-free queues / multi-threaded pipeline (currently single-threaded)
- Object pools (no hot-path allocation exists to pool)
- `busy_poll`/spin-wait on sockets, kernel bypass (DPDK/Onload),
  multicast UDP market data, FPGA/GPU
- Batching at the venue protocol layer
- NUMA pinning, huge pages, CPU isolation

Rule: adopt only after a measured bottleneck and venue requirement.

## Benchmark plan

1. Synthetic replay generator: N instruments, M messages/sec, realistic
   quote/trade mix.
2. Measure per-stage latency distributions (add p50/p95/p99 to
   `ts_metrics` — currently count/avg/max only).
3. Sweep order rates; watch queue depths and `RECON_BREAKS`.
4. Record hardware, OS, compiler flags with every result.
