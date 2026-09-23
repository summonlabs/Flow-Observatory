// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FLOWOBS_STATS_HPP
#define FLOWOBS_STATS_HPP

#include <atomic>
#include <cstdint>
#include <string>

#include "flowobs/export.hpp"
#include "flowobs/identity.hpp"

namespace flowobs {

// Monotone counters describing what the runtime did. Every counter is
// incremented exactly once per completed unit of work so that benchmarks and
// operators can compare "attempted" against "completed".
class FLOWOBS_PUBLIC EngineCounters final {
 public:
  EngineCounters() = default;
  EngineCounters(const EngineCounters&) = delete;
  EngineCounters& operator=(const EngineCounters&) = delete;

  std::atomic<std::uint64_t> observations_received{0};
  std::atomic<std::uint64_t> observations_applied{0};
  std::atomic<std::uint64_t> observations_refused{0};
  std::atomic<std::uint64_t> observations_duplicate{0};
  std::atomic<std::uint64_t> observations_conflicting{0};
  std::atomic<std::uint64_t> observations_dropped_queue_full{0};
  std::atomic<std::uint64_t> observations_requeued{0};

  std::atomic<std::uint64_t> flows_created{0};
  std::atomic<std::uint64_t> flows_evicted{0};
  std::atomic<std::uint64_t> flows_expired{0};
  std::atomic<std::uint64_t> generations_created{0};
  std::atomic<std::uint64_t> generations_evicted{0};
  std::atomic<std::uint64_t> transitions{0};
  std::atomic<std::uint64_t> anomalies_recorded{0};
  std::atomic<std::uint64_t> anomalies_dropped{0};
  std::atomic<std::uint64_t> identity_collisions{0};

  std::atomic<std::uint64_t> queries_served{0};
  std::atomic<std::uint64_t> exports_served{0};
  std::atomic<std::uint64_t> explanations_served{0};
  std::atomic<std::uint64_t> attributions_served{0};
  std::atomic<std::uint64_t> ticks{0};

  std::atomic<std::uint64_t> journal_records_appended{0};
  std::atomic<std::uint64_t> journal_bytes_appended{0};
  std::atomic<std::uint64_t> journal_compactions{0};
  std::atomic<std::uint64_t> journal_failures{0};

  std::atomic<std::uint64_t> transport_frames_in{0};
  std::atomic<std::uint64_t> transport_frames_out{0};
  std::atomic<std::uint64_t> transport_frames_rejected{0};
  std::atomic<std::uint64_t> transport_connections{0};

  std::atomic<std::uint64_t> worker_batches{0};
  std::atomic<std::uint64_t> shutdowns{0};

  void add(EngineCounters& other) noexcept;
  void reset() noexcept;
};

// Immutable snapshot of EngineCounters.
struct FLOWOBS_PUBLIC EngineStats final {
  std::uint64_t observations_received = 0;
  std::uint64_t observations_applied = 0;
  std::uint64_t observations_refused = 0;
  std::uint64_t observations_duplicate = 0;
  std::uint64_t observations_conflicting = 0;
  std::uint64_t observations_dropped_queue_full = 0;
  std::uint64_t observations_requeued = 0;

  std::uint64_t flows_created = 0;
  std::uint64_t flows_evicted = 0;
  std::uint64_t flows_expired = 0;
  std::uint64_t generations_created = 0;
  std::uint64_t generations_evicted = 0;
  std::uint64_t transitions = 0;
  std::uint64_t anomalies_recorded = 0;
  std::uint64_t anomalies_dropped = 0;
  std::uint64_t identity_collisions = 0;

  std::uint64_t queries_served = 0;
  std::uint64_t exports_served = 0;
  std::uint64_t explanations_served = 0;
  std::uint64_t attributions_served = 0;
  std::uint64_t ticks = 0;

  std::uint64_t journal_records_appended = 0;
  std::uint64_t journal_bytes_appended = 0;
  std::uint64_t journal_compactions = 0;
  std::uint64_t journal_failures = 0;

  std::uint64_t transport_frames_in = 0;
  std::uint64_t transport_frames_out = 0;
  std::uint64_t transport_frames_rejected = 0;
  std::uint64_t transport_connections = 0;

  std::uint64_t worker_batches = 0;
  std::uint64_t shutdowns = 0;

  // Runtime observations that are not counters.
  std::uint64_t live_flows = 0;
  std::uint64_t total_flows = 0;
  std::uint64_t queue_depth = 0;
  std::uint64_t queue_capacity = 0;
  std::uint64_t journal_file_bytes = 0;
  bool journal_open = false;
  RuntimeEpoch runtime_epoch{};

  [[nodiscard]] std::string describe() const;
  [[nodiscard]] static EngineStats capture(const EngineCounters& counters);
};

}  // namespace flowobs

#endif  // FLOWOBS_STATS_HPP
