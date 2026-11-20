# Testing

## Layout

```
tests/
  testutil.h/.c   — ASSERT macros + suite runner (setjmp-based)
  test_core.c     — clocks, validation, state helpers
  test_oms.c      — order lifecycle, idempotency, capacity
  test_rms.c      — every risk rule, P&L, kill switch, rate window
  test_ems.c      — venue events -> OMS/RMS, mock venue semantics
  test_journal.c  — journal round-trip + crash recovery
  test_pipeline.c — end-to-end replay + journal recovery equivalence
  test_hardening.c — config, seqwatch, exec dedup, reconcile, replay venue
  test_main.c     — aggregates suites
```

Each `test_*.c` exports `const test_case <suite>[]` terminated by
`{NULL, NULL}`; register new suites in `test_main.c` and add the file to
`CMakeLists.txt`.

## Running

```sh
cmake --build build
ctest --test-dir build --output-on-failure   # or ./build/trading_tests
```

## Conventions

- Virtual clock (`ts_vclock`) everywhere — no wall-clock dependence.
- Temporary journal/replay files use `ts_clock_mono_now()` in names and
  are `remove()`d at test end.
- ASSERT variants: `ASSERT(cond)`, `ASSERT_EQ_I`, `ASSERT_EQ_D(a,b,eps)`,
  `ASSERT_STR`. A failure longjmps to the runner and marks the test FAIL
  without aborting the suite.

## Coverage map (plan Phase 1–2 exit criteria)

| Requirement | Test |
|---|---|
| Order lifecycle incl. partial fills | `oms:lifecycle_to_filled` |
| Idempotent submit | `oms:idempotent_resubmit`, `ems:duplicate_submit_idempotent` |
| Capacity bound | `oms:capacity_full` |
| Illegal transition rejection | `oms:reject_and_cancel` |
| Risk rules | `rms:*` (8 tests, one per rule group) |
| Kill switch + journal persistence | `rms:kill_switch`, `journal:recovery_rebuilds_state` |
| Venue ACK/EXEC/REJECT/CANCEL/EXPIRED routing | `ems:*` (6 tests) |
| IOC/FOK semantics | `ems:ioc_no_fill_expires` |
| Journal round-trip | `journal:journal_write_read` |
| Crash/restart recovery | `journal:recovery_rebuilds_state`, `pipeline:journal_recovers_pipeline_state` |
| End-to-end deterministic replay | `pipeline:end_to_end_replay` |
| Config load + risk apply | `hardening:config_load_and_get` |
| Feed seq-gap detection | `hardening:seqwatch_gaps` |
| Exec dedup by venue_exec_id | `hardening:exec_dedup` |
| Venue reconciliation | `hardening:reconcile_matched_and_breaks` |
| Scripted venue events (dup exec, reject) | `hardening:replay_venue_scripted_events` |
| Replay venue open_orders | `hardening:replay_venue_open_orders` |
| Journal flush/fsync | `hardening:journal_flush_and_fsync` |
| Amend working order | `hardening:amend_working_order` |
| Cancel-all | `hardening:cancel_all` |
| Disconnect -> UNKNOWN | `hardening:disconnect_marks_unknown` |
| Book-imbalance strategy | `hardening:book_imbalance_strategy` |

## Replay format (fixtures + tests)

```
QUOTE <instrument> <bid> <ask> [last] [ts_ns] [seq]
TRADE <instrument> <price> <qty> [ts_ns] [seq]
```

Blank lines and `#` comments skipped. See `fixtures/sample_feed.txt`.

## Still to add (Phase 4 fault injection)

- Sequence-gap injection on feeds (`seq` field exists).
- Venue disconnect mid-order -> `TS_VEV_UNKNOWN` -> reconcile path.
- Duplicate execution reports (venue-level dedup by `venue_exec_id` —
  currently handled implicitly; needs explicit test).
- Crash between `oms_submit` and journal flush (journal is currently
  `fprintf`-buffered; consider `fflush`/`fsync` policy decision).
- Soak tests: N-hour replay at rate, memory/queue stability.
- Benchmarks: hot-path latency under message-rate sweeps.
