# Validation

## How to run it

```console
$ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DFLOWOBS_WARNINGS_AS_ERRORS=ON
$ cmake --build build --parallel
$ ctest --test-dir build --output-on-failure
```

**No test declares a timeout, and no test logic waits on a timeout.** CTest runs
each binary without a `TIMEOUT` property; the test harness has no deadline
parameter; the transport test's waits are blocking reads that complete when the
peer closes its end of the pipe. A test either completes or the process does not
terminate - nothing is ever reported as passing because a deadline expired.

## Suites

| Target | What it covers |
| --- | --- |
| `fo_test_unit` | checksums and known vectors, identity hashing and collision rules, checked arithmetic, time formatting and parsing, enum tables, counter accumulators, the bounded history ring, policy validation, freshness classification, the bounded ledger and its folds, attribution arithmetic, journal framing, the state codec and replay |
| `fo_test_engine` | the public API on a single thread with a manual clock: every lifecycle state, every refusal, every bound, freeze, queue behaviour, source retirement, query filters, explanations, export formats, save/load |
| `fo_test_property` | arrival-order independence, redelivery idempotence, consumption monotonicity, reset non-fabrication, bound invariants, freshness monotonicity, attribution conservation, digest stability |
| `fo_test_adversarial` | replayed incarnations, sequence replay with altered content, self-declared capability, advisory sources, authority override, equal-authority disagreement, future timestamps, missing keys and timestamps, identity substitution, oversized and malformed text, expired generations, unsupported metrics, bit-flipped journals, torn tails, unknown record kinds, journal growth, duplicate inflation, policy change, restored freshness |
| `fo_test_concurrency` | parallel ingest against single-threaded ingest, concurrent readers observing a consistent view, real shutdown and drain, repeated open/close, monotone statistics, the lock-order checker and the ordering audit |
| `fo_test_restart` | accounting survival, liveness non-resurrection, reconfirmation, snapshot fencing, completion durability, repeated restarts, identity-scheme mismatch |
| `fo_test_transport` | independent operating-system processes over loopback TCP, protocol errors, a second process reading the durable result, cross-process idempotence |
| `fo_test_fuzz` | seeded structural mutation of FO1 lines, engine inputs, codec payloads and journal bytes |
| `fo_test_downstream` | install into a scratch prefix, `find_package` from an independent project, build and run |

## Defining invariants and where they are proven

* **The reconciled view is a pure function of the accepted set.**
  `property.arrival_order_does_not_change_the_view` builds one observation
  multiset and compares byte-identical exports across independent shuffles, for
  eight seeded rounds.
* **Redelivery is idempotent.** `property.duplicate_and_repeat_delivery_is_idempotent`
  redelivers every observation three times and asserts an unchanged state digest;
  `engine.duplicate_delivery_is_idempotent` asserts the same through the public
  API, and `transport` asserts it across processes.
* **Completion requires evidence.** `engine.completion_requires_evidence_and_capability`,
  `engine.unregistered_source_cannot_declare_completion`,
  `adversarial.self_declared_capability_is_not_authority` and
  `restart.completed_stays_completed_across_a_restart`.
* **One-sided observation is not bidirectional truth.**
  `engine.one_sided_visibility_is_never_bidirectional_truth` and
  `fold.aggregate_reading_never_proves_bidirectional`.
* **Stale generations cannot support current attribution.**
  `attribution.stale_generation_cannot_support_current_attribution`,
  `attribution.superseded_generation_cannot_support_current_attribution`,
  `engine.generation_regression_is_history_not_current_truth`.
* **Resets do not fabricate consumption.**
  `counters.regression_never_fabricates_consumption`,
  `property.counter_resets_never_fabricate_consumption`,
  `engine.reset_is_terminal_and_leaves_the_gap_unknown`.
* **Restart does not resurrect liveness.**
  `restart.accounting_survives_liveness_does_not`,
  `adversarial.persistence.restored_evidence_never_becomes_fresh_by_itself`,
  `integration.journal_reopen_keeps_accounting_and_never_resurrects_liveness`.
* **Persistence is integrity checked.** `journal.*`, `codec.*`,
  `adversarial.persistence.flipping_one_payload_bit_is_detected`.
* **Bounds hold.** `limits.validation_rejects_nonsense`,
  `engine.bounds_are_enforced_with_explicit_refusals`,
  `invariant.bounded_state_stays_bounded_under_adversarial_churn`,
  `adversarial.persistence.journal_cannot_grow_past_its_bound`.
* **Concurrency ownership is audited.**
  `lockorder.real_engine_operations_never_violate_the_order`,
  `lockorder.many_threads_contending_never_violate_the_order`,
  `lockorder.checker_detects_inversion_and_reentrancy`.

## What the suite does not claim

* No hardware, switch, ASIC, RDMA, InfiniBand or NVLink behaviour is exercised,
  because none is implemented.
* No multi-host behaviour is exercised. The transport test uses separate
  processes on one machine over a loopback socket.
* No third-party telemetry source is exercised. Sources are produced by the
  runtime's own scripts and helpers.
* ThreadSanitizer is not available for the MSVC toolchain used here, so no TSan
  run is claimed. The concurrency guarantees are supported by the in-runtime
  lock-order audit, by a deliberately single-lock design, and by the concurrent
  tests above.
* AddressSanitizer is available and is used; see the release report for the exact
  configuration that was run.
