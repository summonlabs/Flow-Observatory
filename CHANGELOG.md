# Changelog

## 1.0.0 - 2026

Initial release.

* Typed identities, generations, epochs, incarnations and revisions with
  versioned, domain-separated, collision-detecting hashing.
* Lifecycle observation with distinct observed, active, idle, completed, reset,
  expired, unknown and conflicting states; completion requires proof and expiry
  never asserts it.
* Freshness, provenance and evidence quality classified per observation and
  recomputed on every read.
* Multi-source reconciliation whose result is a pure function of the accepted
  observation set, with explicit authority, capability and advisory rules.
* Deterministic integer attribution across endpoints, hops and queues, with
  explicit refusal for stale or superseded generations.
* Versioned, CRC-32C checked, append-only journal with conservative recovery,
  group digests, torn-tail handling and bounded growth.
* Bounded ingest queue and worker pool, real cancellation and shutdown, and a
  compiled-in lock-order and re-entrancy audit.
* Loopback TCP ingest and query protocol with checksum-checked framing.
* `foctl` inspection tool, three examples, a benchmark runner, and a test suite
  covering unit, integration, property, adversarial, concurrency, restart,
  fuzz, independent-process transport and downstream package consumption.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
