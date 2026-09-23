# Persistence

## Journal layout

```
file header            64 bytes, CRC protected
record*                kind u16 | reserved u16 | length u32 | crc u32 | payload
```

File header:

| Offset | Field |
| --- | --- |
| 0 | magic `FOBSJNL1` |
| 8 | format version (u16) |
| 10 | identity scheme (u16) |
| 12 | flags (u32) |
| 16 | runtime epoch (u64) |
| 24 | creation instant (i64 nanoseconds) |
| 32 | record count (u64) |
| 40 | reserved |
| 48 | reserved |
| 52 | header CRC-32C over bytes 0..51 |
| 56 | reserved |

The per-record CRC covers the record's kind, reserved field, length and payload.
Multi-byte values are written byte by byte, so the format does not depend on the
host's endianness, padding or compiler layout.

## Records

| Kind | Meaning |
| --- | --- |
| 1 `snapshot-begin` | opens a snapshot group |
| 2 `source-descriptor` | a source registration |
| 3 `flow` | a durable flow record inside a snapshot group |
| 4 `snapshot-end` | closes a snapshot group and carries its digest |
| 5 `limits` | the bounds in force when the snapshot was written |
| 6 `engine-counters` | informational |
| 7 `tombstone` | reserved |
| 8 `observation` | one accepted observation |

The journal is an **observation log with periodic snapshots**. Normal ingest
appends observation records; compaction rewrites the file with a single snapshot
group.

## Recovery

`replay_records` walks the log in order:

* a snapshot group replaces the accumulated state, after its group digest,
  format version, identity scheme and record counts have all been verified;
* a source descriptor upserts a source;
* a flow record upserts a flow;
* an observation is queued for replay.

The engine then materialises the snapshot and **replays the observations through
the same `apply_locked` path that produced the live state**. The evaluation
instant for each replayed observation is the instant it was originally received,
so the replayed lifecycle history is the history the previous run computed.

After replay every generation is marked restored, and its sources are marked not
reconfirmed. A subsequent `tick` therefore classifies them as `observed`,
`idle` or `expired` - never `active`.

## Failure handling

| Condition | Result |
| --- | --- |
| bad magic | `format-mismatch`, journal refused |
| header CRC mismatch | `integrity-failure`, journal refused |
| format or identity version out of range | `version-mismatch`, journal refused |
| unknown record kind | `format-mismatch`, journal refused |
| record CRC mismatch | `integrity-failure`, journal refused |
| incomplete record header or payload at end of file | torn tail: dropped when `TruncateTornTail`, refused otherwise |
| snapshot group reordered, duplicated or truncated | group digest mismatch, `integrity-failure` |
| snapshot written under a different counter policy | `version-mismatch`, journal refused |

A complete record with a bad checksum is always corruption and never a torn
tail; recovery never repairs it. Compaction writes to a sibling file, flushes it
and then replaces the journal atomically.

## Growth

The journal never grows past `Limits::max_journal_bytes`. When an append would
cross the bound the engine compacts once and retries; if the compacted snapshot
still does not fit, the observation is refused with `limit-exceeded` and the
failure is counted. There is no unbounded growth path.

## What is *not* persisted

* Freshness. It is recomputed from the evaluation instant and the runtime epoch.
* Liveness. A restored flow is never reported live until new evidence arrives.
* The ingest anomaly log and the lifecycle transition log of the *current* view
  digest: they are persisted for operators, but excluded from
  `Engine::state_digest()`.
