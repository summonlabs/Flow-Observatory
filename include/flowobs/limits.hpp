// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every unbounded quantity in the runtime is bounded here. Limits are part of
// the observable contract: they are reported by foctl limits, recorded in the
// journal header, and every eviction or refusal caused by a limit is counted
// and surfaced as an anomaly rather than silently dropped.

#ifndef FLOWOBS_LIMITS_HPP
#define FLOWOBS_LIMITS_HPP

#include <cstddef>
#include <cstdint>

#include "flowobs/error.hpp"
#include "flowobs/export.hpp"
#include "flowobs/time.hpp"

namespace flowobs {

struct FLOWOBS_PUBLIC Limits final {
  // Maximum number of live flow records. Exceeding this evicts the least
  // recently observed terminal flow first; if none is terminal the ingest is
  // refused with ErrorCode::LimitExceeded.
  std::uint32_t max_flows = 200000;

  // Maximum number of distinct observation sources.
  std::uint32_t max_sources = 4096;

  // Maximum distinct path generations retained per flow.
  std::uint32_t max_generations_per_flow = 8;

  // Maximum distinct paths retained per flow.
  std::uint32_t max_paths_per_flow = 8;

  // Maximum distinct queues retained per flow.
  std::uint32_t max_queues_per_flow = 32;

  // Maximum hops in a path.
  std::uint32_t max_path_hops = 64;

  // Maximum observations retained per (flow, generation, source, incarnation).
  std::uint32_t max_observations_per_source_flow = 64;

  // Maximum lifecycle transitions retained per flow.
  std::uint32_t max_history_per_flow = 128;

  // Maximum number of flows for which lifecycle history is retained. When the
  // history table is full, history for the least recently observed flow whose
  // history is complete is dropped, oldest transition first.
  std::uint32_t max_history_flows = 8192;

  // Maximum anomalies retained per flow.
  std::uint32_t max_anomalies_per_flow = 32;

  // Maximum distinct sources retained per flow view.
  std::uint32_t max_sources_per_flow = 16;

  // Bounded ingest queue depth (observations).
  std::size_t max_pending_observations = 65536;

  // Maximum observations taken from the queue in one batch.
  std::size_t max_ingest_batch = 512;

  // Ingest worker threads. 0 selects a single-threaded engine.
  std::uint32_t worker_threads = 3;

  // Maximum rows returned by a query or export.
  std::size_t max_result_set = 10000;

  // Maximum size the journal file may reach before ingest refuses to append.
  std::uint64_t max_journal_bytes = 512ull * 1024ull * 1024ull;

  // Maximum bytes in a canonical identity key.
  std::size_t max_key_bytes = 256;

  // Maximum bytes in a free-form detail string (anomaly text, explanation).
  std::size_t max_detail_bytes = 512;

  // Maximum bytes in a single wire frame payload.
  std::size_t max_frame_bytes = 1024u * 1024u;

  // Maximum simultaneous transport connections.
  std::uint32_t max_connections = 16;

  // Maximum aggregation window a query may request.
  Duration max_aggregation_window = Duration::from_hours(1);

  // Maximum clock skew tolerated between a source's observed_at and this
  // runtime's received_at before the observation is flagged.
  Duration max_clock_skew = Duration::from_minutes(5);

  // Maximum number of text-format records accepted from one ingest stream.
  std::uint64_t max_ingest_records = 100000000ull;

  // Validates internal consistency. Returns InvalidArgument with a specific
  // message when a bound is nonsensical (for example worker_threads > 1024).
  [[nodiscard]] Status validate() const;

  // Human readable, deterministic rendering of every bound. One "key=value"
  // per line in declaration order.
  [[nodiscard]] std::string describe() const;
};

}  // namespace flowobs

#endif  // FLOWOBS_LIMITS_HPP
