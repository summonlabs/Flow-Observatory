// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The vocabulary of the runtime. Every enumerator here has an explicit,
// documented meaning; none of them is inferred from absence of evidence.

#ifndef FLOWOBS_SEMANTICS_HPP
#define FLOWOBS_SEMANTICS_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "flowobs/export.hpp"

namespace flowobs {

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------

// Lifecycle of a flow generation as this runtime currently understands it.
//
//  Unknown      Nothing is known. This is the state of a flow that exists only
//               because a persisted record or a downstream reference named it.
//  Observed     At least one accepted observation exists, but it carries no
//               progress evidence. The runtime knows the flow exists.
//  Active       Accepted, fresh evidence shows forward progress inside the
//               idle window.
//  Idle         The flow was Active and has produced no progress evidence for
//               longer than the idle threshold, while still inside the expiry
//               window. Idle is not completion.
//  Completed    The flow is terminated and that termination is *proven*: an
//               explicit completion observation from a source that holds the
//               completion capability, or a terminal counter snapshot declared
//               complete. Only this state asserts completion.
//  Reset        The flow generation was reset (counter regression with reset
//               evidence, or an explicit reset observation). Consumption for
//               the interval that ended at the reset is partially unknown and
//               is reported as such; a reset never fabricates consumption.
//  Expired      The expiry policy closed the generation because evidence aged
//               out. Expired explicitly does *not* assert completion.
//  Conflicting  Two sources of equal authority disagree about the current
//               generation and no rule resolves the disagreement. Conflicting
//               is a deliberate "we do not know", never a failure.
enum class FlowState : std::uint8_t {
  Unknown = 0,
  Observed = 1,
  Active = 2,
  Idle = 3,
  Completed = 4,
  Reset = 5,
  Expired = 6,
  Conflicting = 7,
};

inline constexpr std::uint8_t kFlowStateCount = 8;

FLOWOBS_PUBLIC std::string_view to_string(FlowState state) noexcept;

// Parses the canonical lowercase name. Returns false for anything else.
FLOWOBS_PUBLIC bool parse_flow_state(std::string_view text, FlowState& out) noexcept;

// True for states from which no further transition can occur without a new
// generation: Completed, Reset, Expired.
FLOWOBS_PUBLIC bool is_terminal(FlowState state) noexcept;

// True when the state asserts that the flow carries traffic right now. Only
// Active does.
FLOWOBS_PUBLIC bool asserts_liveness(FlowState state) noexcept;

// True when the state asserts that the flow ended. Only Completed does.
FLOWOBS_PUBLIC bool asserts_completion(FlowState state) noexcept;

// Why the flow entered its current state. Recorded with every transition so
// that explanations never have to guess.
enum class StateCause : std::uint8_t {
  None = 0,
  FirstObservation = 1,
  ProgressObserved = 2,
  IdleThresholdReached = 3,
  CompletionObserved = 4,
  TerminalCountersObserved = 5,
  ResetObserved = 6,
  CounterRegression = 7,
  ExpiryPolicy = 8,
  AuthorityConflict = 9,
  ConflictResolved = 10,
  RecoveredFromJournal = 11,
  GenerationAdvanced = 12,
  EvidenceFenced = 13,
};

inline constexpr std::uint8_t kStateCauseCount = 14;

FLOWOBS_PUBLIC std::string_view to_string(StateCause cause) noexcept;
FLOWOBS_PUBLIC bool parse_state_cause(std::string_view text, StateCause& out) noexcept;

// ---------------------------------------------------------------------------
// Freshness.
// ---------------------------------------------------------------------------

// Freshness is a function of the observation instant, the receive instant and
// the evaluation instant, using the configured windows. It is never stored as
// a durable property of the evidence itself.
//
//  Unknown   The timestamps needed to decide are missing or the evidence was
//            restored from persistence in a previous runtime epoch and has not
//            been re-confirmed.
//  Fresh     observed_at is inside fresh_window of the evaluation instant.
//  Stale     observed_at is older than fresh_window but inside expire_window.
//            Stale evidence may be shown but must not drive liveness,
//            completion or current attribution.
//  Expired   observed_at is older than expire_window. Expired evidence is
//            retained for history only.
enum class Freshness : std::uint8_t {
  Unknown = 0,
  Fresh = 1,
  Stale = 2,
  Expired = 3,
};

inline constexpr std::uint8_t kFreshnessCount = 4;

FLOWOBS_PUBLIC std::string_view to_string(Freshness f) noexcept;
FLOWOBS_PUBLIC bool parse_freshness(std::string_view text, Freshness& out) noexcept;

// True only for Fresh. Callers must use this rather than comparing ordinals.
FLOWOBS_PUBLIC bool is_usable_for_liveness(Freshness f) noexcept;
FLOWOBS_PUBLIC bool is_usable_for_attribution(Freshness f) noexcept;

// ---------------------------------------------------------------------------
// Evidence quality.
// ---------------------------------------------------------------------------

// What the producing source claims about its own observation.
//
//  Unknown      The source did not say. Treated as Incomplete for every
//               decision that requires completeness.
//  Complete     The source asserts the observation is a complete statement
//               about the subject for the instant it covers.
//  Incomplete   The source explicitly says the observation is partial (for
//               example a counter snapshot taken while the flow still runs).
//  Unsupported  The subject cannot be measured by this source (for example a
//               retransmission count on a transport that has none). The value
//               is absent and must never be rendered as zero.
enum class Evidence : std::uint8_t {
  Unknown = 0,
  Complete = 1,
  Incomplete = 2,
  Unsupported = 3,
};

inline constexpr std::uint8_t kEvidenceCount = 4;

FLOWOBS_PUBLIC std::string_view to_string(Evidence e) noexcept;
FLOWOBS_PUBLIC bool parse_evidence(std::string_view text, Evidence& out) noexcept;

// Derived, never supplied by a source: whether independent sources agree.
enum class Consistency : std::uint8_t {
  Unknown = 0,      // fewer than two independent readings exist
  Corroborated = 1, // two or more sources agree within tolerance
  Conflicting = 2,  // two or more sources of equal authority disagree
  Resolved = 3,     // sources disagree, a strictly higher authority decided
};

inline constexpr std::uint8_t kConsistencyCount = 4;

FLOWOBS_PUBLIC std::string_view to_string(Consistency c) noexcept;
FLOWOBS_PUBLIC bool parse_consistency(std::string_view text, Consistency& out) noexcept;

// ---------------------------------------------------------------------------
// Direction and visibility.
// ---------------------------------------------------------------------------

// Which half of a flow an observation describes. The runtime never converts one
// direction into the other.
enum class Direction : std::uint8_t {
  Unspecified = 0,
  Forward = 1,    // measured at the flow's initiator endpoint, leaving it
  Reverse = 2,    // measured at the flow's target endpoint, returning
  Aggregate = 3,  // a single reading covering both halves; cannot prove
                  // bidirectional coverage on its own
};

inline constexpr std::uint8_t kDirectionCount = 4;

FLOWOBS_PUBLIC std::string_view to_string(Direction d) noexcept;
FLOWOBS_PUBLIC bool parse_direction(std::string_view text, Direction& out) noexcept;

// How much of the flow the runtime can actually see.
//
//  Unknown    No accepted observation yet.
//  OneSided   Evidence exists for exactly one direction only. A one-sided
//             observation is never bidirectional truth: completion and
//             symmetric consumption are reported as unproven.
//  TwoSided   Both directions carry accepted, fresh, complete evidence.
enum class Visibility : std::uint8_t {
  Unknown = 0,
  OneSided = 1,
  TwoSided = 2,
};

inline constexpr std::uint8_t kVisibilityCount = 3;

FLOWOBS_PUBLIC std::string_view to_string(Visibility v) noexcept;
FLOWOBS_PUBLIC bool parse_visibility(std::string_view text, Visibility& out) noexcept;

// Directional coverage is tracked per direction and per provenance class.
struct FLOWOBS_PUBLIC Coverage final {
  bool forward_complete = false;
  bool forward_partial = false;
  bool reverse_complete = false;
  bool reverse_partial = false;

  [[nodiscard]] bool any() const noexcept {
    return forward_complete || forward_partial || reverse_complete || reverse_partial;
  }
  [[nodiscard]] bool bidirectional() const noexcept {
    return forward_complete && reverse_complete;
  }
  [[nodiscard]] Visibility classify() const noexcept {
    if (bidirectional()) return Visibility::TwoSided;
    if (!any()) return Visibility::Unknown;
    return Visibility::OneSided;
  }
  [[nodiscard]] std::string describe() const;
};

// ---------------------------------------------------------------------------
// Observation kinds.
// ---------------------------------------------------------------------------

// What happened at the instant the observation covers. The kind, not the
// counter values, decides whether completion or reset may be asserted.
enum class ObservationKind : std::uint8_t {
  Unknown = 0,
  Open = 1,           // the flow generation started
  Sample = 2,         // cumulative counters at an instant
  Progress = 3,       // a delta arrived: the flow moved data since the last
                      // observation
  Idle = 4,           // the source asserts no progress in the covered window
  Close = 5,          // the flow generation terminated cleanly
  Reset = 6,          // the flow generation was reset (RST, abort, abortive
                      // close)
  Error = 7,          // the source reports an error condition
  PathChange = 8,     // the flow moved to a different path (new generation)
  CounterReset = 9,   // the source's counters restarted
  Heartbeat = 10,     // liveness only, no accounting meaning
};

inline constexpr std::uint8_t kObservationKindCount = 11;

FLOWOBS_PUBLIC std::string_view to_string(ObservationKind k) noexcept;
FLOWOBS_PUBLIC bool parse_observation_kind(std::string_view text,
                                           ObservationKind& out) noexcept;

// True when the kind, combined with Complete evidence, may prove completion.
FLOWOBS_PUBLIC bool kind_can_prove_completion(ObservationKind k) noexcept;

// True when the kind announces that counters restart.
FLOWOBS_PUBLIC bool kind_announces_counter_restart(ObservationKind k) noexcept;

// ---------------------------------------------------------------------------
// Anomalies.
// ---------------------------------------------------------------------------

// Every refusal, fence, eviction and disagreement is recorded as an anomaly.
// Anomalies are bounded per flow and never silently dropped: the count of
// dropped anomalies is itself reported.
enum class AnomalyKind : std::uint8_t {
  None = 0,
  DuplicateEvidence = 1,
  SequenceConflict = 2,
  StaleEpochReplay = 3,
  StaleIncarnationReplay = 4,
  StaleGenerationEvidence = 5,
  StaleRevisionReplay = 6,
  LateEvidenceRejected = 7,
  CounterRegression = 8,
  CounterOverflow = 9,
  ClockSkew = 10,
  AuthorityConflict = 11,
  CapabilityUnsupported = 12,
  IdentityCollision = 13,
  LimitEviction = 14,
  UnsupportedMetric = 15,
  IncompleteEvidence = 16,
  OneSidedVisibility = 17,
  RestartGap = 18,
  MalformedEvidence = 19,
};

inline constexpr std::uint8_t kAnomalyKindCount = 20;

FLOWOBS_PUBLIC std::string_view to_string(AnomalyKind k) noexcept;
FLOWOBS_PUBLIC bool parse_anomaly_kind(std::string_view text, AnomalyKind& out) noexcept;

// True when the anomaly means "the runtime saw something and chose not to
// trust it". These are the conditions the runtime must never hide.
FLOWOBS_PUBLIC bool anomaly_is_conservative_refusal(AnomalyKind k) noexcept;

}  // namespace flowobs

#endif  // FLOWOBS_SEMANTICS_HPP
