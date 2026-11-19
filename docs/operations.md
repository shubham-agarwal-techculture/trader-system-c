# Operations

## Health signals

`ts_metrics` counters: market events, sequence gaps, stale marks, orders
submitted/duplicates, risk approvals/rejects, venue ACKs/rejects,
executions, cancels, reconciliation breaks. Latency records: decision,
risk, dispatch, ACK, fill (count/avg/max ns).

`trading_sim run` prints a metrics dump at end of replay;
`trading_sim recover` prints recovered state.

## Kill switch

Engage via `rms_kill(rms)` — journaled, survives restart, blocks all new
orders at the RMS gate. Working orders are NOT auto-canceled; pair with
`ems_cancel` per open order or venue cancel-all for emergency flatten
(Phase 4 runbook).

## Recovery procedure

1. Stop order entry (kill switch / process stop).
2. Run `trading_sim recover --journal <file>` (or the recovery path in
   the host application) to rebuild OMS/RMS.
3. Compare open orders and positions against venue state
   (Phase 3 reconciliation).
4. Resolve `TS_ORD_UNKNOWN` orders before re-enabling entry.

## Alerting hooks (to wire to monitoring)

- `TS_MET_MKT_SEQ_GAPS`, `TS_MET_MKT_STALE` — feed health.
- `TS_MET_RECON_BREAKS` — overfills, illegal transitions, event drops.
- `TS_MET_RISK_REJECTED` spikes — strategy or limit misconfiguration.
- `TS_MET_VENUE_REJECTS` — venue-side problems.
- Open-order count and `TS_ORD_UNKNOWN` count — reconciliation debt.

## Secrets

No credentials exist in this milestone. Future adapters must read secrets
from OS credential storage or a secrets manager — never from config files,
logs, or the journal.
