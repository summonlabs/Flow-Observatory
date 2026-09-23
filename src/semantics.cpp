// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowobs/semantics.hpp"

#include <string>

namespace flowobs {

namespace {

// Table-driven so that adding an enumerator without a name is a compile error
// in the static_assert below.
struct NamePair final {
  std::uint8_t value;
  std::string_view name;
};

template <std::size_t N>
bool lookup(const NamePair (&table)[N], std::string_view text, std::uint8_t& out) noexcept {
  for (const NamePair& pair : table) {
    if (pair.name == text) {
      out = pair.value;
      return true;
    }
  }
  return false;
}

constexpr NamePair kFlowStates[] = {
    {0, "unknown"}, {1, "observed"}, {2, "active"},    {3, "idle"},
    {4, "completed"}, {5, "reset"}, {6, "expired"},    {7, "conflicting"},
};
static_assert(std::size(kFlowStates) == kFlowStateCount, "flow state table out of date");

constexpr NamePair kStateCauses[] = {
    {0, "none"},          {1, "first-observation"},   {2, "progress-observed"},
    {3, "idle-threshold"}, {4, "completion-observed"}, {5, "terminal-counters"},
    {6, "reset-observed"}, {7, "counter-regression"},  {8, "expiry-policy"},
    {9, "authority-conflict"}, {10, "conflict-resolved"}, {11, "recovered-from-journal"},
    {12, "generation-advanced"}, {13, "evidence-fenced"},
};
static_assert(std::size(kStateCauses) == kStateCauseCount, "state cause table out of date");

constexpr NamePair kFreshness[] = {
    {0, "unknown"}, {1, "fresh"}, {2, "stale"}, {3, "expired"},
};
static_assert(std::size(kFreshness) == kFreshnessCount, "freshness table out of date");

constexpr NamePair kEvidence[] = {
    {0, "unknown"}, {1, "complete"}, {2, "incomplete"}, {3, "unsupported"},
};
static_assert(std::size(kEvidence) == kEvidenceCount, "evidence table out of date");

constexpr NamePair kConsistency[] = {
    {0, "unknown"}, {1, "corroborated"}, {2, "conflicting"}, {3, "resolved"},
};
static_assert(std::size(kConsistency) == kConsistencyCount, "consistency table out of date");

constexpr NamePair kDirections[] = {
    {0, "unspecified"}, {1, "forward"}, {2, "reverse"}, {3, "aggregate"},
};
static_assert(std::size(kDirections) == kDirectionCount, "direction table out of date");

constexpr NamePair kVisibility[] = {
    {0, "unknown"}, {1, "one-sided"}, {2, "two-sided"},
};
static_assert(std::size(kVisibility) == kVisibilityCount, "visibility table out of date");

constexpr NamePair kObservationKinds[] = {
    {0, "unknown"},  {1, "open"},   {2, "sample"},      {3, "progress"},
    {4, "idle"},     {5, "close"},  {6, "reset"},       {7, "error"},
    {8, "path-change"}, {9, "counter-reset"}, {10, "heartbeat"},
};
static_assert(std::size(kObservationKinds) == kObservationKindCount,
              "observation kind table out of date");

constexpr NamePair kAnomalyKinds[] = {
    {0, "none"},
    {1, "duplicate-evidence"},
    {2, "sequence-conflict"},
    {3, "stale-epoch-replay"},
    {4, "stale-incarnation-replay"},
    {5, "stale-generation-evidence"},
    {6, "stale-revision-replay"},
    {7, "late-evidence-rejected"},
    {8, "counter-regression"},
    {9, "counter-overflow"},
    {10, "clock-skew"},
    {11, "authority-conflict"},
    {12, "capability-unsupported"},
    {13, "identity-collision"},
    {14, "limit-eviction"},
    {15, "unsupported-metric"},
    {16, "incomplete-evidence"},
    {17, "one-sided-visibility"},
    {18, "restart-gap"},
    {19, "malformed-evidence"},
};
static_assert(std::size(kAnomalyKinds) == kAnomalyKindCount, "anomaly table out of date");

template <std::size_t N>
std::string_view name_of(const NamePair (&table)[N], std::uint8_t value) noexcept {
  for (const NamePair& pair : table) {
    if (pair.value == value) {
      return pair.name;
    }
  }
  return "invalid";
}

}  // namespace

std::string_view to_string(FlowState state) noexcept {
  return name_of(kFlowStates, static_cast<std::uint8_t>(state));
}
bool parse_flow_state(std::string_view text, FlowState& out) noexcept {
  std::uint8_t raw = 0;
  if (!lookup(kFlowStates, text, raw)) return false;
  out = static_cast<FlowState>(raw);
  return true;
}

bool is_terminal(FlowState state) noexcept {
  return state == FlowState::Completed || state == FlowState::Reset ||
         state == FlowState::Expired;
}
bool asserts_liveness(FlowState state) noexcept { return state == FlowState::Active; }
bool asserts_completion(FlowState state) noexcept { return state == FlowState::Completed; }

std::string_view to_string(StateCause cause) noexcept {
  return name_of(kStateCauses, static_cast<std::uint8_t>(cause));
}
bool parse_state_cause(std::string_view text, StateCause& out) noexcept {
  std::uint8_t raw = 0;
  if (!lookup(kStateCauses, text, raw)) return false;
  out = static_cast<StateCause>(raw);
  return true;
}

std::string_view to_string(Freshness f) noexcept {
  return name_of(kFreshness, static_cast<std::uint8_t>(f));
}
bool parse_freshness(std::string_view text, Freshness& out) noexcept {
  std::uint8_t raw = 0;
  if (!lookup(kFreshness, text, raw)) return false;
  out = static_cast<Freshness>(raw);
  return true;
}
bool is_usable_for_liveness(Freshness f) noexcept { return f == Freshness::Fresh; }
bool is_usable_for_attribution(Freshness f) noexcept { return f == Freshness::Fresh; }

std::string_view to_string(Evidence e) noexcept {
  return name_of(kEvidence, static_cast<std::uint8_t>(e));
}
bool parse_evidence(std::string_view text, Evidence& out) noexcept {
  std::uint8_t raw = 0;
  if (!lookup(kEvidence, text, raw)) return false;
  out = static_cast<Evidence>(raw);
  return true;
}

std::string_view to_string(Consistency c) noexcept {
  return name_of(kConsistency, static_cast<std::uint8_t>(c));
}
bool parse_consistency(std::string_view text, Consistency& out) noexcept {
  std::uint8_t raw = 0;
  if (!lookup(kConsistency, text, raw)) return false;
  out = static_cast<Consistency>(raw);
  return true;
}

std::string_view to_string(Direction d) noexcept {
  return name_of(kDirections, static_cast<std::uint8_t>(d));
}
bool parse_direction(std::string_view text, Direction& out) noexcept {
  std::uint8_t raw = 0;
  if (!lookup(kDirections, text, raw)) return false;
  out = static_cast<Direction>(raw);
  return true;
}

std::string_view to_string(Visibility v) noexcept {
  return name_of(kVisibility, static_cast<std::uint8_t>(v));
}
bool parse_visibility(std::string_view text, Visibility& out) noexcept {
  std::uint8_t raw = 0;
  if (!lookup(kVisibility, text, raw)) return false;
  out = static_cast<Visibility>(raw);
  return true;
}

std::string Coverage::describe() const {
  std::string out;
  out.reserve(96);
  out += "forward=";
  if (forward_complete) {
    out += "complete";
  } else if (forward_partial) {
    out += "partial";
  } else {
    out += "unobserved";
  }
  out += " reverse=";
  if (reverse_complete) {
    out += "complete";
  } else if (reverse_partial) {
    out += "partial";
  } else {
    out += "unobserved";
  }
  return out;
}

std::string_view to_string(ObservationKind k) noexcept {
  return name_of(kObservationKinds, static_cast<std::uint8_t>(k));
}
bool parse_observation_kind(std::string_view text, ObservationKind& out) noexcept {
  std::uint8_t raw = 0;
  if (!lookup(kObservationKinds, text, raw)) return false;
  out = static_cast<ObservationKind>(raw);
  return true;
}

bool kind_can_prove_completion(ObservationKind k) noexcept {
  // Only an explicit termination or a declared terminal snapshot can prove
  // completion. A heartbeat, a sample or an idle notice never can, and neither
  // can an error: an error is not a proof that the flow ended.
  switch (k) {
    case ObservationKind::Close:
      return true;
    case ObservationKind::Sample:
    case ObservationKind::Progress:
    case ObservationKind::CounterReset:
      // A cumulative snapshot can carry a terminal declaration, but the kind
      // alone does not prove completion; the engine additionally requires
      // Complete evidence and the completion capability. Treating a plain
      // sample as proof is exactly the mistake this function exists to stop.
      return false;
    default:
      return false;
  }
}

bool kind_announces_counter_restart(ObservationKind k) noexcept {
  return k == ObservationKind::CounterReset || k == ObservationKind::Reset;
}

std::string_view to_string(AnomalyKind k) noexcept {
  return name_of(kAnomalyKinds, static_cast<std::uint8_t>(k));
}
bool parse_anomaly_kind(std::string_view text, AnomalyKind& out) noexcept {
  std::uint8_t raw = 0;
  if (!lookup(kAnomalyKinds, text, raw)) return false;
  out = static_cast<AnomalyKind>(raw);
  return true;
}

bool anomaly_is_conservative_refusal(AnomalyKind k) noexcept {
  switch (k) {
    case AnomalyKind::DuplicateEvidence:
    case AnomalyKind::SequenceConflict:
    case AnomalyKind::StaleEpochReplay:
    case AnomalyKind::StaleIncarnationReplay:
    case AnomalyKind::StaleGenerationEvidence:
    case AnomalyKind::StaleRevisionReplay:
    case AnomalyKind::LateEvidenceRejected:
    case AnomalyKind::CounterRegression:
    case AnomalyKind::CounterOverflow:
    case AnomalyKind::ClockSkew:
    case AnomalyKind::AuthorityConflict:
    case AnomalyKind::CapabilityUnsupported:
    case AnomalyKind::IdentityCollision:
    case AnomalyKind::LimitEviction:
    case AnomalyKind::UnsupportedMetric:
    case AnomalyKind::IncompleteEvidence:
    case AnomalyKind::OneSidedVisibility:
    case AnomalyKind::RestartGap:
    case AnomalyKind::MalformedEvidence:
      return true;
    case AnomalyKind::None:
      return false;
  }
  return false;
}

}  // namespace flowobs
