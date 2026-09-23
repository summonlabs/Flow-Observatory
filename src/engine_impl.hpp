// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Internal runtime state. This header is not installed: it exists so that the
// implementation can be split across translation units without exposing the
// mutable state of the runtime to consumers.

#ifndef FLOWOBS_SRC_ENGINE_IMPL_HPP
#define FLOWOBS_SRC_ENGINE_IMPL_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "flowobs/clock.hpp"
#include "flowobs/engine.hpp"
#include "flowobs/history.hpp"
#include "flowobs/reconcile.hpp"

namespace flowobs {

// Per-source evidence for one flow generation.
struct SourceBinding final {
  SourceId source{};
  SourceLedger ledger;
  bool restored = false;
  bool reconfirmed = true;
  std::uint32_t refused_observations = 0;
  // Bitset of fold anomaly kinds already promoted to the flow level, so that a
  // long-running fold does not spam the bounded per-flow anomaly list.
  std::uint32_t promoted_anomaly_kinds = 0;
  std::uint32_t stale_epoch_events = 0;
  std::uint32_t stale_incarnation_events = 0;
  std::uint32_t clock_skew_events = 0;
  std::uint32_t unsupported_events = 0;
  std::uint32_t incomplete_events = 0;

  // Cached fold. Freshness is deliberately *not* cached: it depends on the
  // evaluation instant and is recomputed on every access, so a tick can never
  // be served from a stale classification.
  mutable SourceView cached;
  mutable bool cache_valid = false;
};

struct GenerationRuntime final {
  GenerationId id{};
  FlowState state = FlowState::Unknown;
  StateCause cause = StateCause::None;
  Timestamp first_observed_at{};
  Timestamp last_observed_at{};
  Timestamp last_received_at{};
  Timestamp last_progress_at{};
  Timestamp closed_at{};
  Coverage coverage{};
  EndpointId endpoint_a{};
  EndpointId endpoint_b{};
  std::vector<SourceBinding> bindings;  // ascending by SourceId
  Timestamp last_touched{};
};

struct FlowRuntime final {
  FlowId id{};
  std::string canonical_key;
  std::uint64_t key_collisions = 0;
  GenerationId current_generation{};
  RevisionId revision{};
  FlowState state = FlowState::Unknown;
  StateCause cause = StateCause::None;
  Coverage coverage{};
  Timestamp created_at{};
  Timestamp state_changed_at{};
  Timestamp first_observed_at{};
  Timestamp last_observed_at{};
  Timestamp last_received_at{};
  Timestamp last_progress_at{};
  EndpointId endpoint_a{};
  EndpointId endpoint_b{};
  std::vector<GenerationRuntime> generations;  // ascending by generation id
  LifecycleHistory history;
  std::vector<Anomaly> anomalies;
  std::uint64_t anomalies_dropped = 0;
  bool restored = false;
  RuntimeEpoch restored_epoch{};
  Timestamp last_touched{};
};

struct SourceState final {
  SourceDescriptor descriptor;
  bool retired = false;
  Timestamp registered_at{};
};

// A bounded, deterministic identity registry per domain. It exists so that an
// identity collision - two distinct canonical keys hashing to the same identity
// - is detected and reported rather than silently merged.
template <class Id>
struct IdentityRegistry final {
  std::map<Id, std::string> entries;

  enum class Outcome : std::uint8_t { Registered, Known, Collision, Full };

  Outcome observe(Id id, const std::string& key, std::size_t capacity,
                  std::string& existing) {
    const auto found = entries.find(id);
    if (found != entries.end()) {
      existing = found->second;
      return found->second == key ? Outcome::Known : Outcome::Collision;
    }
    if (entries.size() >= capacity) {
      return Outcome::Full;
    }
    entries.emplace(id, key);
    return Outcome::Registered;
  }
};

struct Engine::Impl final {
  EngineConfig config{};
  SystemClock default_clock{};
  const Clock* clock = nullptr;

  mutable std::shared_mutex mutex;
  EngineCounters counters{};

  std::map<FlowId, FlowRuntime> flows;
  std::map<SourceId, SourceState> sources;
  IdentityRegistry<EndpointId> endpoints;
  IdentityRegistry<PathId> paths;
  IdentityRegistry<LinkId> links;
  IdentityRegistry<QueueId> queues;

  std::unique_ptr<Journal> journal;
  RuntimeEpoch epoch{};
  Timestamp opened_at{};
  bool open = false;
  bool replaying = false;
  // Evaluation instant used while replaying the journal: the instant the
  // evidence was originally received, so a replay reproduces the live history
  // exactly instead of re-deriving it from the current wall clock.
  Timestamp replay_now{};
  // The runtime epoch the journal was written in. Evidence replayed from it is
  // attributed to that epoch, never to the current one.
  RuntimeEpoch replay_epoch{};
  // Evaluation instant used by an explicit tick, so that a tick is a pure
  // function of the instant it is given.
  Timestamp evaluation_override{};
  std::uint64_t snapshot_sequence = 0;

  std::atomic<bool> frozen{false};
  std::atomic<bool> stop{false};

  std::mutex queue_mutex;
  std::condition_variable queue_cv;
  std::condition_variable queue_space_cv;
  std::deque<Observation> queue;
  std::vector<std::thread> workers;
  bool workers_running = false;

  [[nodiscard]] Timestamp now() const {
    if (evaluation_override.known()) {
      return evaluation_override;
    }
    return clock != nullptr ? clock->now() : default_clock.now();
  }

  [[nodiscard]] FreshnessContext context() const;

  static void note(FlowRuntime& flow, const Anomaly& anomaly, const Limits& limits);
  static Anomaly make_anomaly(AnomalyKind kind, Timestamp at, const Observation& observation,
                              std::string detail);
  static void transition(FlowRuntime& flow, GenerationRuntime& generation, FlowState next,
                         StateCause cause, Timestamp at, Timestamp observed_at, SourceId by,
                         std::string detail, const Limits& limits);
  static const SourceDescriptor& descriptor_for(const SourceState& state) {
    return state.descriptor;
  }

  [[nodiscard]] SourceView source_view(const SourceBinding& binding,
                                       const SourceDescriptor& descriptor,
                                       const FreshnessContext& ctx) const;
  [[nodiscard]] GenerationView generation_view(const FlowRuntime& flow,
                                               const GenerationRuntime& generation,
                                               const FreshnessContext& ctx) const;
  void recompute_flow(FlowRuntime& flow, Timestamp at, const Limits& limits,
                      std::vector<LifecycleEvent>* new_events = nullptr);
  [[nodiscard]] FlowSnapshot snapshot_of(const FlowRuntime& flow, const FreshnessContext& ctx,
                                         const EngineConfig& cfg) const;

  [[nodiscard]] PersistedSource persist_source(const SourceBinding& binding) const;
  [[nodiscard]] PersistedFlow persist_flow(const FlowRuntime& flow,
                                           const FreshnessContext& ctx) const;
  [[nodiscard]] PersistedState persist_state() const;
  // The same projection with ingest diagnostics cleared: this is what
  // Engine::state_digest hashes, because the determinism contract is about the
  // evidence-derived view and not about how many duplicates the transport
  // happened to deliver.
  [[nodiscard]] PersistedState persist_view_state() const;

  [[nodiscard]] Status compact_locked();
  [[nodiscard]] Status append_observation_locked(const Observation& observation);
  [[nodiscard]] Status append_source_locked(const SourceDescriptor& descriptor);

  [[nodiscard]] Status validate_observation(const Observation& observation) const;
  Status register_identity(FlowRuntime& flow, const Observation& observation,
                           AnomalyKind& anomaly_kind, std::string& detail);
  GenerationRuntime& obtain_generation(FlowRuntime& flow, GenerationId id, Timestamp at);
  SourceBinding& obtain_binding(GenerationRuntime& generation, SourceId source,
                                FlowGeneration flow_generation);
  [[nodiscard]] Result<IngestOutcome> apply_locked(const Observation& observation);
  [[nodiscard]] Status evict_one_flow_locked();
  [[nodiscard]] Status evict_one_generation_locked(FlowRuntime& flow);

  [[nodiscard]] Status open_journal_locked();
  [[nodiscard]] Status restore_locked(const std::vector<journal::Record>& records,
                                      RuntimeEpoch previous_epoch);
  [[nodiscard]] Status start_workers_locked();
  [[nodiscard]] Status stop_workers_locked();
  [[nodiscard]] TickReport tick_locked(Timestamp evaluated_at);
};

}  // namespace flowobs

#endif  // FLOWOBS_SRC_ENGINE_IMPL_HPP
