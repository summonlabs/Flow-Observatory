// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The Flow Observatory runtime.
//
// Scope, stated precisely because the boundary is part of the contract:
//
//   The runtime OWNS per-flow lifecycle and resource-consumption observation.
//   It does NOT schedule flows, choose paths, enforce rates, own admission or
//   congestion policy, or invent application semantics beyond the metadata it
//   is given. It never asserts a fact it has not been given evidence for.

#ifndef FLOWOBS_ENGINE_HPP
#define FLOWOBS_ENGINE_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "flowobs/attribution.hpp"
#include "flowobs/clock.hpp"
#include "flowobs/export.hpp"
#include "flowobs/flow.hpp"
#include "flowobs/identity.hpp"
#include "flowobs/limits.hpp"
#include "flowobs/observation.hpp"
#include "flowobs/persistence.hpp"
#include "flowobs/policy.hpp"
#include "flowobs/stats.hpp"

namespace flowobs {

// Aggregate configuration of a runtime instance.
struct FLOWOBS_PUBLIC EngineConfig final {
  Limits limits{};
  FreshnessPolicy freshness{};
  ExpiryPolicy expiry{};
  CounterPolicy counters{};
  ReconciliationPolicy reconciliation{};
  AttributionPolicy attribution{};
  RecoveryPolicy recovery{};
  QueryPolicy query{};

  // Journal path. Empty means the runtime is purely in-memory.
  std::filesystem::path journal_path;
  // When true and a journal exists, it is opened and restored. When false the
  // journal is created empty.
  bool restore_on_open = true;

  // Injected clock. When null a SystemClock is used.
  const Clock* clock = nullptr;

  [[nodiscard]] Status validate() const;

  // Deterministic digest of every policy field. Recorded in the journal so a
  // reader can tell that the policy changed between runs.
  [[nodiscard]] std::uint64_t policy_digest() const noexcept;
  [[nodiscard]] std::string describe() const;
};

// What the runtime did with one piece of evidence.
enum class IngestDisposition : std::uint8_t {
  Applied = 0,          // accepted and folded into the view
  Duplicate = 1,        // exact duplicate: idempotent no-op
  Refused = 2,          // deliberately not trusted (see code)
  Queued = 3,           // handed to a worker
  DroppedQueueFull = 4, // bounded queue rejected it
};

FLOWOBS_PUBLIC std::string_view to_string(IngestDisposition disposition) noexcept;

struct FLOWOBS_PUBLIC IngestOutcome final {
  IngestDisposition disposition = IngestDisposition::Refused;
  ErrorCode code = ErrorCode::Ok;
  std::string detail;

  FlowGeneration generation{};
  FlowState state_before = FlowState::Unknown;
  FlowState state_after = FlowState::Unknown;
  StateCause cause = StateCause::None;
  std::uint32_t transitions = 0;
  std::uint32_t anomalies = 0;
  std::uint64_t journal_records = 0;

  [[nodiscard]] bool applied() const noexcept {
    return disposition == IngestDisposition::Applied;
  }
  [[nodiscard]] bool idempotent() const noexcept {
    return disposition == IngestDisposition::Duplicate;
  }
  [[nodiscard]] std::string describe() const;
};

// Query filters. All optional; unset fields do not constrain.
struct FLOWOBS_PUBLIC FlowQuery final {
  std::optional<FlowId> flow;
  std::optional<FlowState> state;
  std::optional<GenerationId> generation;
  std::optional<SourceId> source;
  std::optional<EndpointId> endpoint;
  std::optional<PathId> path;
  std::optional<QueueId> queue;
  bool include_terminal = true;
  bool only_live = false;
  bool only_restored = false;
  std::size_t limit = 1000;
  std::size_t offset = 0;
  // Restricts the reported window to evidence whose observed_at is within
  // [since, now]. Unset means no window.
  std::optional<Timestamp> since;

  [[nodiscard]] Status validate(const QueryPolicy& policy) const;
};

struct FLOWOBS_PUBLIC QueryResult final {
  std::vector<FlowSnapshot> flows;
  std::size_t matched = 0;
  std::size_t returned = 0;
  bool truncated = false;
  Timestamp evaluated_at{};

  [[nodiscard]] std::string describe() const;
};

// Deterministic explanation of one flow: why it is in the state it is in, what
// is unknown and why.
struct FLOWOBS_PUBLIC Explanation final {
  FlowId flow{};
  std::string canonical_key;
  Timestamp evaluated_at{};
  std::vector<std::string> lines;

  [[nodiscard]] std::string render() const;
};

struct FLOWOBS_PUBLIC HistoryReport final {
  FlowId flow{};
  std::vector<LifecycleEvent> events;
  std::uint64_t events_dropped = 0;
  std::size_t returned = 0;
  bool truncated = false;

  [[nodiscard]] std::string describe() const;
};

struct FLOWOBS_PUBLIC ExportOptions final {
  enum class Format : std::uint8_t { Json = 0, Csv = 1, Text = 2 };
  Format format = Format::Json;
  FlowQuery query{};
  bool pretty = true;
  bool include_anomalies = true;
  bool include_sources = true;
  bool include_generations = true;
  bool include_history = false;
  bool include_limits = true;
  bool include_stats = true;
  bool deterministic = true;  // never emit a runtime instant that was not supplied
  std::optional<Timestamp> evaluated_at;

  [[nodiscard]] Status validate(const Limits& limits, const QueryPolicy& policy) const;
};

// One combined result the tooling and the transport both consume.
struct FLOWOBS_PUBLIC ExportResult final {
  std::string text;
  std::size_t rows = 0;
  bool truncated = false;
  ExportOptions::Format format = ExportOptions::Format::Json;

  [[nodiscard]] std::string describe() const;
};

// The runtime.
class FLOWOBS_PUBLIC Engine final {
 public:
  Engine();
  explicit Engine(EngineConfig config);
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  ~Engine();

  // --- lifecycle --------------------------------------------------------
  // Opens the journal (when configured), restores state, and starts workers.
  [[nodiscard]] Status open();
  [[nodiscard]] Status close();
  [[nodiscard]] bool is_open() const noexcept;

  // --- configuration ----------------------------------------------------
  [[nodiscard]] const EngineConfig& config() const noexcept;
  [[nodiscard]] const Limits& limits() const noexcept;
  [[nodiscard]] Timestamp now() const;

  // --- sources ----------------------------------------------------------
  // Registers or updates a source descriptor. Authority and capabilities are
  // explicit; they are never inferred from volume or arrival order.
  [[nodiscard]] Status register_source(const SourceDescriptor& descriptor);
  [[nodiscard]] Result<SourceDescriptor> describe_source(SourceId id) const;
  [[nodiscard]] Status list_sources(std::vector<SourceDescriptor>& out) const;
  // Removes a source and every observation attributed to it. Used by tests and
  // by operators retiring a telemetry producer.
  [[nodiscard]] Status retire_source(SourceId id);

  // --- ingest -----------------------------------------------------------
  // Synchronous, thread safe. Folds the observation into the view and appends
  // the resulting durable delta to the journal.
  [[nodiscard]] Result<IngestOutcome> ingest(const Observation& observation);

  // Convenience: stamps received_at and the default capability set derived
  // from the registered descriptor, then ingests.
  [[nodiscard]] Result<IngestOutcome> submit(const Observation& observation);

  // Asynchronous path: enqueues onto the bounded queue. Returns
  // DroppedQueueFull when the queue is full; the observation is never silently
  // discarded.
  [[nodiscard]] Result<IngestOutcome> enqueue(const Observation& observation);
  [[nodiscard]] Status start_workers();
  [[nodiscard]] Status stop_workers();
  [[nodiscard]] Status drain(std::size_t max_observations);
  [[nodiscard]] std::size_t queue_depth() const;

  // --- observation ------------------------------------------------------
  [[nodiscard]] Result<QueryResult> query(const FlowQuery& query) const;
  [[nodiscard]] Result<FlowSnapshot> snapshot(FlowId flow) const;
  [[nodiscard]] Result<HistoryReport> history(FlowId flow, std::size_t limit) const;
  [[nodiscard]] Result<Explanation> explain(FlowId flow) const;
  [[nodiscard]] Result<AttributionReport> attribute(FlowId flow, GenerationId generation,
                                                    const AttributionPolicy& policy) const;
  [[nodiscard]] Result<AttributionReport> attribute_current(FlowId flow) const;
  [[nodiscard]] Result<ExportResult> export_data(const ExportOptions& options) const;

  // Recomputes freshness and applies the expiry policy for the given instant.
  // The only way a generation becomes Idle or Expired. Deterministic.
  [[nodiscard]] Result<TickReport> tick(Timestamp evaluated_at);

  // --- persistence ------------------------------------------------------
  [[nodiscard]] Status flush();
  // Writes a full snapshot and rewrites the journal from it.
  [[nodiscard]] Status compact();
  // Verifies the journal on disk without applying it.
  [[nodiscard]] Status verify_journal() const;
  [[nodiscard]] Status save_snapshot(const std::filesystem::path& path) const;
  [[nodiscard]] Status load_snapshot(const std::filesystem::path& path);

  // --- introspection ----------------------------------------------------
  [[nodiscard]] EngineStats stats() const;
  [[nodiscard]] RuntimeEpoch runtime_epoch() const;
  [[nodiscard]] std::uint64_t state_digest() const;
  // Freezes the runtime: after this call every mutating entry point returns
  // ErrorCode::ShuttingDown and every read continues to work.
  void freeze() noexcept;
  [[nodiscard]] bool frozen() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace flowobs

#endif  // FLOWOBS_ENGINE_HPP
