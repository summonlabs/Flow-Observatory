// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The reconciled view of a flow. Everything in this header is *derived*: it is
// a pure function of the accepted observation set, the policy and the
// evaluation instant. Nothing here is stored as durable truth.

#ifndef FLOWOBS_FLOW_HPP
#define FLOWOBS_FLOW_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "flowobs/counters.hpp"
#include "flowobs/export.hpp"
#include "flowobs/identity.hpp"
#include "flowobs/observation.hpp"
#include "flowobs/semantics.hpp"
#include "flowobs/time.hpp"

namespace flowobs {

// A recorded refusal, fence, eviction or disagreement.
struct FLOWOBS_PUBLIC Anomaly final {
  AnomalyKind kind = AnomalyKind::None;
  Timestamp at{};          // runtime instant the anomaly was recorded
  Timestamp observed_at{}; // observation instant it relates to, when known
  FlowGeneration generation{};
  SourceId source{};
  std::string detail;

  [[nodiscard]] std::string describe() const;
};

// One lifecycle transition of one flow generation.
struct FLOWOBS_PUBLIC LifecycleEvent final {
  Timestamp at{};
  Timestamp observed_at{};
  FlowGeneration generation{};
  FlowState from = FlowState::Unknown;
  FlowState to = FlowState::Unknown;
  StateCause cause = StateCause::None;
  SourceId source{};
  std::string detail;

  [[nodiscard]] std::string describe() const;
};

// The view of one source's evidence about one flow generation.
struct FLOWOBS_PUBLIC SourceView final {
  SourceId source{};
  std::string canonical_key;
  std::uint32_t authority = 0;
  bool advisory_only = false;
  SourceCapabilities capabilities{};

  IncarnationId incarnation{};
  EpochId epoch{};
  RevisionId high_revision{};
  SequenceNo high_sequence{};

  Timestamp first_observed_at{};
  Timestamp last_observed_at{};
  Timestamp last_received_at{};
  Timestamp last_progress_at{};

  Freshness freshness = Freshness::Unknown;
  Evidence last_evidence = Evidence::Unknown;
  ObservationKind last_kind = ObservationKind::Unknown;
  Direction last_direction = Direction::Unspecified;

  Coverage coverage{};
  DirectionalAccounting forward{};
  DirectionalAccounting reverse{};
  // Readings that the source did not attribute to a direction. They are
  // accounted for, but they never prove coverage of either direction.
  DirectionalAccounting aggregate{};

  Consumption forward_totals{};
  Consumption reverse_totals{};
  Consumption aggregate_totals{};
  // True when this source produced at least one accepted observation whose kind
  // or counters showed forward progress.
  bool observed_progress = false;

  // Set when the source declared a terminal state in its last accepted
  // observation (for example a Close).
  std::optional<FlowState> declared_terminal{};
  StateCause declared_cause = StateCause::None;

  // Correlation declared by the source.
  EndpointId endpoint_a{};
  EndpointId endpoint_b{};
  std::optional<PathId> path;
  std::vector<LinkId> links;
  std::vector<QueueId> queues;
  bool has_queue = false;
  // Source-supplied capacity units for the declared path hops, in the same
  // order as `links`. Empty means capacity-weighted attribution is unsupported
  // for this source, which the attribution report states explicitly.
  std::vector<std::uint64_t> hop_capacity_units;

  std::uint32_t accepted_observations = 0;
  std::uint32_t refused_observations = 0;
  std::uint32_t dropped_observations = 0;  // folded into the bounded prefix
  std::uint32_t duplicates_suppressed = 0;
  std::uint32_t content_conflicts = 0;
  std::uint32_t historical_incarnations = 0;
  std::uint32_t distinct_incarnations = 0;
  std::uint32_t after_terminal_observations = 0;

  // Anomalies produced while folding this source's evidence, by kind.
  std::uint32_t anomaly_counts[kAnomalyKindCount] = {};
  std::uint32_t regressions = 0;
  std::uint32_t stale_epoch_events = 0;
  std::uint32_t stale_incarnation_events = 0;
  std::uint32_t clock_skew_events = 0;
  std::uint32_t restart_gaps = 0;
  std::uint32_t unsupported_events = 0;
  std::uint32_t incomplete_events = 0;
  bool saturated = false;

  // True when at least one accepted observation for this source/generation was
  // restored from a journal rather than received in the current runtime epoch.
  bool restored_from_journal = false;
  // True when fresh evidence has been received for this source/generation in
  // the current runtime epoch.
  bool reconfirmed = false;

  [[nodiscard]] bool usable_for_liveness() const noexcept {
    return !advisory_only && is_usable_for_liveness(freshness) && reconfirmed;
  }
};

// The view of one generation of one flow.
struct FLOWOBS_PUBLIC GenerationView final {
  FlowGeneration generation{};
  bool is_current = false;

  Timestamp first_observed_at{};
  Timestamp last_observed_at{};
  Timestamp last_received_at{};
  Timestamp last_progress_at{};
  Timestamp closed_at{};

  FlowState state = FlowState::Unknown;
  StateCause cause = StateCause::None;
  Freshness freshness = Freshness::Unknown;
  Consistency consistency = Consistency::Unknown;
  Coverage coverage{};
  Visibility visibility = Visibility::Unknown;

  // Resources attached to this generation by accepted evidence.
  std::optional<PathId> path;
  std::vector<LinkId> links;
  std::vector<QueueId> queues;
  std::vector<std::uint64_t> hop_capacity_units;
  EndpointId endpoint_a{};
  EndpointId endpoint_b{};

  Consumption forward{};
  Consumption reverse{};
  Consumption aggregate{};
  Consumption total{};

  std::vector<SourceView> sources;  // sorted: authority desc, then SourceId asc
  SourceId primary_source{};
  std::uint32_t excluded_stale_sources = 0;
  std::uint32_t excluded_advisory_sources = 0;
  bool has_path_correlation = false;
  bool has_queue_correlation = false;

  // True when the generation could not be reconciled because equal-authority
  // sources disagree.
  bool unresolved_conflict = false;
  std::string conflict_detail;
};

// One flow, as the runtime currently understands it.
struct FLOWOBS_PUBLIC FlowSnapshot final {
  FlowId id{};
  std::string canonical_key;
  std::uint64_t key_collisions = 0;

  GenerationId current_generation{};
  RevisionId revision{};

  FlowState state = FlowState::Unknown;
  StateCause cause = StateCause::None;
  Freshness freshness = Freshness::Unknown;
  Consistency consistency = Consistency::Unknown;
  Coverage coverage{};
  Visibility visibility = Visibility::Unknown;

  Timestamp created_at{};
  Timestamp state_changed_at{};
  Timestamp first_observed_at{};
  Timestamp last_observed_at{};
  Timestamp last_received_at{};
  Timestamp last_progress_at{};

  EndpointId endpoint_a{};
  EndpointId endpoint_b{};

  Consumption total{};

  std::vector<GenerationView> generations;
  std::vector<Anomaly> anomalies;
  std::uint64_t anomalies_dropped = 0;

  // True when any part of this snapshot originates from a previous runtime
  // epoch. Such a flow is never reported Active until fresh evidence arrives.
  bool restored_from_journal = false;
  RuntimeEpoch restored_epoch{};
  RuntimeEpoch current_epoch{};

  [[nodiscard]] const GenerationView* find_generation(GenerationId id) const noexcept;
  [[nodiscard]] bool is_live() const noexcept {
    return state == FlowState::Active && !restored_from_journal;
  }
  [[nodiscard]] std::string one_line() const;
};

// Result of an explicit expiry / freshness sweep.
struct FLOWOBS_PUBLIC TickReport final {
  Timestamp evaluated_at{};
  std::uint32_t flows_examined = 0;
  std::uint32_t transitions = 0;
  std::uint32_t expired = 0;
  std::uint32_t became_idle = 0;
  std::vector<LifecycleEvent> events;
};

}  // namespace flowobs

#endif  // FLOWOBS_FLOW_HPP
