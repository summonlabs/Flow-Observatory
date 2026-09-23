# Bounds

Every unbounded quantity is bounded. The bounds live in `flowobs::Limits`, are
reported by `foctl limits`, are recorded in the journal by the `limits` record,
and are enforced with a counted refusal or a counted eviction.

| Bound | Default | On exhaustion |
| --- | --- | --- |
| `max_flows` | 200000 | evict the least recently observed **terminal** flow; if none exists, refuse with `limit-exceeded` |
| `max_sources` | 4096 | refuse with `limit-exceeded` |
| `max_generations_per_flow` | 8 | evict the lowest-numbered terminal, non-current generation; if none exists, refuse |
| `max_paths_per_flow` | 8 | additional paths are not retained |
| `max_queues_per_flow` | 32 | additional queues are not retained |
| `max_path_hops` | 64 | refuse the observation |
| `max_observations_per_source_flow` | 64 | fold the lowest entries into a bounded prefix summary; accounting is preserved exactly |
| `max_history_per_flow` | 128 | drop the oldest transition and count the drop |
| `max_history_flows` | 8192 | history is retained per flow that has it; the bound caps the table |
| `max_anomalies_per_flow` | 32 | drop the oldest anomaly and count the drop |
| `max_sources_per_flow` | 16 | refuse the observation with `limit-exceeded` |
| `max_pending_observations` | 65536 | `enqueue` returns `dropped-queue-full` and counts it |
| `max_ingest_batch` | 512 | workers take smaller batches |
| `worker_threads` | 3 | 0 selects a single-threaded engine |
| `max_result_set` | 10000 | queries and exports truncate and report `truncated` |
| `max_journal_bytes` | 512 MiB | compact once and retry; if it still does not fit, refuse the append |
| `max_key_bytes` | 256 | refuse the observation or the registration |
| `max_detail_bytes` | 512 | detail strings are truncated, never silently dropped |
| `max_frame_bytes` | 1 MiB | refuse the frame with `limit-exceeded` and close the connection |
| `max_connections` | 16 | refuse the socket outright and never queue without bound |
| `max_aggregation_window` | 1 hour | a query may not request a longer window |
| `max_clock_skew` | 5 minutes | clamp (default) or refuse the observation outright |
| `max_ingest_records` | 100000000 | refuse the script or the journal replay |

## Rules

* Externally derived sizes are checked before they are used: lengths, counts and
  buffer sizes read from a frame or a journal go through `checked_add`,
  `checked_mul` and `checked_narrow` first.
* Counter arithmetic saturates and sets an `overflow` flag; it never wraps. The
  saturation is reported as a `counter-overflow` anomaly and as
  `saturated: true` in exports.
* Eviction, where it exists, is deterministic: a total order over the candidates
  (terminal state first, then least recently observed, then lowest identity) so
  that two runtimes under the same pressure evict the same objects.
* Every eviction and every refusal increments a counter and, where a flow is
  identifiable, records an anomaly. Nothing is dropped silently, and the number
  of anomalies that could not be retained is itself reported as
  `anomalies_dropped`.
* `Limits::validate()` rejects inconsistent configurations at construction, so a
  nonsensical bound is a startup failure rather than a surprise at run time.
