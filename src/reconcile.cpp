// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Reconciliation and the pure folds. Read docs/semantics.md alongside this
// file: the invariants implemented here are the ones the property tests check.

#include "flowobs/reconcile.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "flowobs/checked.hpp"

namespace flowobs {

namespace {

constexpr std::int64_t kNanosPerSecond = 1000000000LL;

// --- fold state -----------------------------------------------------------
//
// One fold state per (flow, generation, source). It is advanced strictly in
// canonical order: incarnation, then epoch, then revision, then sequence.
// Because the ledger always folds the lowest entries first, and because the
// fold is a left fold, the result is exactly the left fold of the entire
// accepted set in canonical order, independent of arrival order.
struct FoldState final {
  DirectionalAccounting forward{};
  DirectionalAccounting reverse{};
  DirectionalAccounting aggregate{};
  Coverage coverage{};
  std::optional<FlowState> declared_terminal{};
  StateCause declared_cause = StateCause::None;
  ObservationKind last_kind = ObservationKind::Unknown;
  Direction last_direction = Direction::Unspecified;
  Evidence last_evidence = Evidence::Unknown;
  CounterSet last_counters{};
  Timestamp first_observed_at{};
  Timestamp last_observed_at{};
  Timestamp last_received_at{};
  Timestamp last_progress_at{};
  IncarnationId current_incarnation{};
  EpochId current_epoch{};
  RevisionId high_revision{};
  SequenceNo high_sequence{};
  IncarnationId folded_incarnation{};
  EpochId folded_epoch{};
  RevisionId folded_revision{};
  SequenceNo folded_sequence{};
  bool folded_seq_exact = true;
  std::uint64_t folded_seq_low = 0;
  std::uint64_t folded_seq_bits = 0;
  std::uint32_t anomaly_counts[kAnomalyKindCount] = {};
  std::uint32_t historical_incarnations = 0;
  std::uint32_t distinct_incarnations = 0;
  std::uint32_t after_terminal_observations = 0;
  bool observed_progress = false;
  EndpointId endpoint_a{};
  EndpointId endpoint_b{};
  std::optional<PathId> path;
  std::vector<LinkId> links;
  std::vector<QueueId> queues;
  std::vector<std::uint64_t> hop_capacity_units;
  bool has_queue = false;
};

FoldState from_prefix(const SourceLedger::Prefix& prefix) {
  FoldState state;
  state.forward = prefix.forward;
  state.reverse = prefix.reverse;
  state.aggregate = prefix.aggregate;
  state.coverage = prefix.coverage;
  state.declared_terminal = prefix.declared_terminal;
  state.declared_cause = prefix.declared_cause;
  state.last_kind = prefix.last_kind;
  state.last_direction = prefix.last_direction;
  state.last_evidence = prefix.last_evidence;
  state.last_counters = prefix.last_counters;
  state.first_observed_at = prefix.first_observed_at;
  state.last_observed_at = prefix.last_observed_at;
  state.last_received_at = prefix.last_received_at;
  state.last_progress_at = prefix.last_progress_at;
  state.current_incarnation = prefix.current_incarnation;
  state.current_epoch = prefix.current_epoch;
  state.high_revision = prefix.high_revision;
  state.high_sequence = prefix.high_sequence;
  state.folded_incarnation = prefix.folded_incarnation;
  state.folded_epoch = prefix.folded_epoch;
  state.folded_revision = prefix.folded_revision;
  state.folded_sequence = prefix.folded_sequence;
  state.folded_seq_exact = prefix.folded_seq_exact;
  state.folded_seq_low = prefix.folded_seq_low;
  state.folded_seq_bits = prefix.folded_seq_bits;
  for (std::size_t i = 0; i < kAnomalyKindCount; ++i) {
    state.anomaly_counts[i] = prefix.anomaly_counts[i];
  }
  state.historical_incarnations = prefix.historical_incarnations;
  state.distinct_incarnations = prefix.distinct_incarnations;
  state.after_terminal_observations = prefix.after_terminal_observations;
  state.observed_progress = prefix.observed_progress;
  state.endpoint_a = prefix.endpoint_a;
  state.endpoint_b = prefix.endpoint_b;
  state.path = prefix.path;
  state.links = prefix.links;
  state.queues = prefix.queues;
  state.hop_capacity_units = prefix.hop_capacity_units;
  state.has_queue = prefix.has_queue;
  return state;
}

void to_prefix(FoldState&& state, SourceLedger::Prefix& prefix) {
  prefix.forward = std::move(state.forward);
  prefix.reverse = std::move(state.reverse);
  prefix.aggregate = std::move(state.aggregate);
  prefix.coverage = state.coverage;
  prefix.declared_terminal = state.declared_terminal;
  prefix.declared_cause = state.declared_cause;
  prefix.last_kind = state.last_kind;
  prefix.last_direction = state.last_direction;
  prefix.last_evidence = state.last_evidence;
  prefix.last_counters = state.last_counters;
  prefix.first_observed_at = state.first_observed_at;
  prefix.last_observed_at = state.last_observed_at;
  prefix.last_received_at = state.last_received_at;
  prefix.last_progress_at = state.last_progress_at;
  prefix.current_incarnation = state.current_incarnation;
  prefix.current_epoch = state.current_epoch;
  prefix.high_revision = state.high_revision;
  prefix.high_sequence = state.high_sequence;
  prefix.folded_incarnation = state.folded_incarnation;
  prefix.folded_epoch = state.folded_epoch;
  prefix.folded_revision = state.folded_revision;
  prefix.folded_sequence = state.folded_sequence;
  prefix.folded_seq_exact = state.folded_seq_exact;
  prefix.folded_seq_low = state.folded_seq_low;
  prefix.folded_seq_bits = state.folded_seq_bits;
  for (std::size_t i = 0; i < kAnomalyKindCount; ++i) {
    prefix.anomaly_counts[i] = state.anomaly_counts[i];
  }
  prefix.historical_incarnations = state.historical_incarnations;
  prefix.distinct_incarnations = state.distinct_incarnations;
  prefix.after_terminal_observations = state.after_terminal_observations;
  prefix.observed_progress = state.observed_progress;
  prefix.endpoint_a = state.endpoint_a;
  prefix.endpoint_b = state.endpoint_b;
  prefix.path = state.path;
  prefix.links = std::move(state.links);
  prefix.queues = std::move(state.queues);
  prefix.hop_capacity_units = std::move(state.hop_capacity_units);
  prefix.has_queue = state.has_queue;
}

void fold_mark(FoldState& state, const ObservationKey& key) {
  if (state.folded_incarnation != key.incarnation || state.folded_epoch != key.epoch) {
    state.folded_incarnation = key.incarnation;
    state.folded_epoch = key.epoch;
    state.folded_seq_low = key.sequence.value();
    state.folded_seq_bits = 1ull;
    state.folded_seq_exact = true;
    return;
  }
  const std::uint64_t low = state.folded_seq_low;
  const std::uint64_t sequence = key.sequence.value();
  if (sequence >= low && sequence < low + 64ull) {
    state.folded_seq_bits |= (1ull << (sequence - low));
    return;
  }
  if (sequence >= low + 64ull) {
    const std::uint64_t shift = sequence - low - 63ull;
    state.folded_seq_bits = shift >= 64ull ? 0ull : (state.folded_seq_bits << shift);
    state.folded_seq_low = sequence - 63ull;
    state.folded_seq_bits |= 1ull;
    return;
  }
  // Below the window: the exact answer for that range is no longer available.
  state.folded_seq_exact = false;
}

[[nodiscard]] bool fold_contains(bool exact, IncarnationId incarnation, EpochId epoch,
                                 std::uint64_t low, std::uint64_t bits,
                                 const ObservationKey& key) {
  if (!exact) {
    return false;
  }
  if (incarnation != key.incarnation || epoch != key.epoch) {
    return false;
  }
  const std::uint64_t sequence = key.sequence.value();
  if (sequence < low || sequence >= low + 64ull) {
    return false;
  }
  return ((bits >> (sequence - low)) & 1ull) != 0ull;
}

[[nodiscard]] bool fold_contains(const FoldState& state, const ObservationKey& key) {
  return fold_contains(state.folded_seq_exact, state.folded_incarnation, state.folded_epoch,
                       state.folded_seq_low, state.folded_seq_bits, key);
}

[[nodiscard]] bool fold_contains(const SourceLedger::Prefix& prefix,
                                 const ObservationKey& key) {
  return fold_contains(prefix.folded_seq_exact, prefix.folded_incarnation,
                       prefix.folded_epoch, prefix.folded_seq_low, prefix.folded_seq_bits,
                       key);
}

void note(FoldState& state, AnomalyKind kind) {
  const auto index = static_cast<std::size_t>(kind);
  if (index < kAnomalyKindCount) {
    ++state.anomaly_counts[index];
  }
}

std::uint32_t anomaly_count(const FoldState& state, AnomalyKind kind) {
  const auto index = static_cast<std::size_t>(kind);
  return index < kAnomalyKindCount ? state.anomaly_counts[index] : 0u;
}

DirectionalAccounting& accounting_for(FoldState& state, Direction direction) {
  switch (direction) {
    case Direction::Forward: return state.forward;
    case Direction::Reverse: return state.reverse;
    case Direction::Aggregate:
    case Direction::Unspecified:
    default: return state.aggregate;
  }
}

const DirectionalAccounting& accounting_for(const FoldState& state, Direction direction) {
  switch (direction) {
    case Direction::Forward: return state.forward;
    case Direction::Reverse: return state.reverse;
    case Direction::Aggregate:
    case Direction::Unspecified:
    default: return state.aggregate;
  }
}

// Starts a new accounting interval on every direction, because an incarnation
// or epoch boundary is not direction specific: the source restarted, so nothing
// about its counters transfers.
void break_interval(FoldState& state) {
  state.forward.bytes.start_new_interval(std::nullopt);
  state.forward.packets.start_new_interval(std::nullopt);
  state.forward.retransmissions.start_new_interval(std::nullopt);
  state.reverse.bytes.start_new_interval(std::nullopt);
  state.reverse.packets.start_new_interval(std::nullopt);
  state.reverse.retransmissions.start_new_interval(std::nullopt);
  state.aggregate.bytes.start_new_interval(std::nullopt);
  state.aggregate.packets.start_new_interval(std::nullopt);
  state.aggregate.retransmissions.start_new_interval(std::nullopt);
}

void apply_observation(FoldState& state, const Observation& obs, const CounterPolicy& policy) {
  if (!state.first_observed_at.known() ||
      (obs.observed_at.known() && obs.observed_at < state.first_observed_at)) {
    state.first_observed_at = obs.observed_at;
  }
  if (obs.observed_at > state.last_observed_at) {
    state.last_observed_at = obs.observed_at;
  }
  if (obs.received_at > state.last_received_at) {
    state.last_received_at = obs.received_at;
  }
  if (obs.key.revision > state.high_revision) {
    state.high_revision = obs.key.revision;
  }
  if (obs.key.sequence > state.high_sequence) {
    state.high_sequence = obs.key.sequence;
  }

  // --- incarnation and epoch fences --------------------------------------
  if (obs.key.incarnation != state.current_incarnation) {
    if (state.current_incarnation.valid()) {
      // The producing process restarted. The delta across the restart is not
      // observable, so it is counted as unknown rather than bridged.
      break_interval(state);
      note(state, AnomalyKind::RestartGap);
    }
    state.current_incarnation = obs.key.incarnation;
    state.current_epoch = obs.key.epoch;
    ++state.distinct_incarnations;
  } else if (obs.key.epoch != state.current_epoch) {
    // A new epoch always leaves a gap: the delta between the last value of the
    // previous epoch and the first value of the new one was never observed. An
    // announced restart does not invent it, it only explains it.
    break_interval(state);
    if (!(policy.honor_explicit_counter_reset && kind_announces_counter_restart(obs.kind))) {
      // An unexplained epoch change with advancing values is a counter
      // regression from the runtime's point of view.
      note(state, AnomalyKind::CounterRegression);
    }
    state.current_epoch = obs.key.epoch;
  }

  // Evidence that arrives after a terminal declaration cannot reopen the
  // generation; it is counted and ignored so that the outcome stays a pure
  // function of the accepted set.
  if (state.declared_terminal.has_value() && is_terminal(*state.declared_terminal)) {
    ++state.after_terminal_observations;
    return;
  }

  state.last_kind = obs.kind;
  state.last_direction = obs.direction;
  state.last_evidence = obs.evidence;
  if (obs.counters.any()) {
    state.last_counters = obs.counters;
  }

  // --- coverage ----------------------------------------------------------
  const bool complete = obs.evidence == Evidence::Complete;
  switch (obs.direction) {
    case Direction::Forward:
      if (complete) {
        state.coverage.forward_complete = true;
      } else {
        state.coverage.forward_partial = true;
      }
      break;
    case Direction::Reverse:
      if (complete) {
        state.coverage.reverse_complete = true;
      } else {
        state.coverage.reverse_partial = true;
      }
      break;
    case Direction::Aggregate:
    case Direction::Unspecified:
      // Deliberately does not set either directional flag: an aggregate or
      // unattributed reading is not bidirectional truth.
      note(state, AnomalyKind::IncompleteEvidence);
      break;
  }

  // --- declared terminal state -------------------------------------------
  if (obs.kind == ObservationKind::Close) {
    state.declared_terminal = FlowState::Completed;
    state.declared_cause = StateCause::CompletionObserved;
  } else if (obs.kind == ObservationKind::Reset) {
    state.declared_terminal = FlowState::Reset;
    state.declared_cause = StateCause::ResetObserved;
  } else if (obs.kind == ObservationKind::Error) {
    // An error is evidence that something went wrong, never that the flow
    // ended. Nothing terminal is declared.
    note(state, AnomalyKind::MalformedEvidence);
  }

  // --- correlation -------------------------------------------------------
  // Only evidence that names a resource attaches it. Absence of a path in one
  // observation never detaches a path that another observation supplied.
  if (obs.subject.endpoint.valid()) {
    if (!state.endpoint_a.valid()) {
      state.endpoint_a = obs.subject.endpoint;
    } else if (state.endpoint_a != obs.subject.endpoint && !state.endpoint_b.valid()) {
      state.endpoint_b = obs.subject.endpoint;
    }
  }
  if (obs.subject.peer_endpoint.valid() && !state.endpoint_b.valid() &&
      state.endpoint_a != obs.subject.peer_endpoint) {
    state.endpoint_b = obs.subject.peer_endpoint;
  }
  if (obs.subject.path.has_value() && !state.path.has_value()) {
    state.path = obs.subject.path;
  }
  if (obs.subject.link.has_value()) {
    if (std::find(state.links.begin(), state.links.end(), *obs.subject.link) ==
        state.links.end()) {
      state.links.push_back(*obs.subject.link);
    }
  }
  if (obs.subject.queue.has_value()) {
    state.has_queue = true;
    if (std::find(state.queues.begin(), state.queues.end(), *obs.subject.queue) ==
        state.queues.end()) {
      state.queues.push_back(*obs.subject.queue);
    }
  }
  // Capacity metadata is attached once, by the first observation in canonical
  // order that supplies it. Later observations never overwrite it, so the
  // weight vector a report shows is the same one the fold saw.
  if (state.hop_capacity_units.empty() && !obs.subject.hop_capacity_units.empty()) {
    state.hop_capacity_units = obs.subject.hop_capacity_units;
  }

  // --- counters ----------------------------------------------------------
  DirectionalAccounting& accounting = accounting_for(state, obs.direction);
  const bool regression_as_reset = policy.treat_regression_as_reset;
  bool advanced = false;
  bool any_counter = false;

  const auto apply_counter = [&](CounterAccumulator& accumulator,
                                 const std::optional<std::uint64_t>& value,
                                 bool& unsupported_flag) {
    if (!value.has_value()) {
      if (obs.evidence == Evidence::Unsupported) {
        unsupported_flag = true;
        note(state, AnomalyKind::UnsupportedMetric);
      }
      return;
    }
    any_counter = true;
    switch (accumulator.observe(*value, regression_as_reset)) {
      case CounterAccumulator::Outcome::Advanced:
        advanced = true;
        break;
      case CounterAccumulator::Outcome::Regressed:
        note(state, AnomalyKind::CounterRegression);
        break;
      case CounterAccumulator::Outcome::Saturated:
        note(state, AnomalyKind::CounterOverflow);
        break;
      case CounterAccumulator::Outcome::Held:
        note(state, AnomalyKind::CounterRegression);
        break;
      case CounterAccumulator::Outcome::BaselineEstablished:
      case CounterAccumulator::Outcome::Unchanged:
        break;
    }
  };

  apply_counter(accounting.bytes, obs.counters.bytes, accounting.bytes_unsupported);
  apply_counter(accounting.packets, obs.counters.packets, accounting.packets_unsupported);
  apply_counter(accounting.retransmissions, obs.counters.retransmissions,
                accounting.retransmissions_unsupported);

  if (advanced || obs.kind == ObservationKind::Progress ||
      obs.kind == ObservationKind::Open || obs.kind == ObservationKind::PathChange) {
    if (obs.observed_at > state.last_progress_at) {
      state.last_progress_at = obs.observed_at;
    }
    state.observed_progress = true;
  }

  if (any_counter && obs.direction == Direction::Unspecified) {
    note(state, AnomalyKind::IncompleteEvidence);
  }
}

Consumption to_consumption(const DirectionalAccounting& accounting) {
  Consumption out;
  out.bytes = accounting.bytes.accumulated();
  out.packets = accounting.packets.accumulated();
  out.retransmissions = accounting.retransmissions.accumulated();
  out.unknown_intervals = accounting.unknown_intervals();
  out.bytes_unsupported = accounting.bytes_unsupported;
  out.packets_unsupported = accounting.packets_unsupported;
  out.retransmissions_unsupported = accounting.retransmissions_unsupported;
  out.saturated = accounting.bytes.saturated() || accounting.packets.saturated() ||
                  accounting.retransmissions.saturated();
  return out;
}

// Ordering used by the ledger and by every deterministic iteration.
bool canonical_less(const Observation& a, const Observation& b) {
  if (a.key.incarnation != b.key.incarnation) return a.key.incarnation < b.key.incarnation;
  if (a.key.epoch != b.key.epoch) return a.key.epoch < b.key.epoch;
  if (a.key.revision != b.key.revision) return a.key.revision < b.key.revision;
  return a.key.sequence < b.key.sequence;
}

bool same_canonical_key(const Observation& a, const Observation& b) {
  return a.key.incarnation == b.key.incarnation && a.key.epoch == b.key.epoch &&
         a.key.revision == b.key.revision && a.key.sequence == b.key.sequence;
}

}  // namespace

// ---------------------------------------------------------------------------
// Freshness
// ---------------------------------------------------------------------------

Freshness classify_freshness(Timestamp observed_at, Timestamp received_at,
                             const FreshnessContext& ctx) noexcept {
  if (ctx.from_previous_epoch && ctx.policy.require_reconfirmation_after_restart) {
    // Evidence carried over from a previous runtime epoch is not knowledge
    // about the present. It becomes Unknown until fresh evidence arrives.
    return Freshness::Unknown;
  }
  if (!observed_at.known()) {
    return Freshness::Unknown;
  }
  Timestamp effective = observed_at;
  if (received_at.known() && observed_at > received_at) {
    const Duration skew = time_difference(observed_at, received_at);
    if (skew > Duration::zero()) {
      // A source clock ahead of ours. Clamping is the conservative choice: the
      // evidence is never treated as newer than the instant we received it.
      effective = received_at;
    }
  }
  const Timestamp reference = ctx.now.known() ? ctx.now : received_at;
  if (!reference.known()) {
    return Freshness::Unknown;
  }
  if (effective > reference) {
    // The evaluation instant is before the evidence; the evidence cannot yet be
    // judged against a window, and calling it fresh would overstate it.
    return Freshness::Unknown;
  }
  const Duration age = time_difference(reference, effective);
  if (age <= ctx.policy.fresh_window) {
    return Freshness::Fresh;
  }
  if (age <= ctx.policy.expire_window) {
    return Freshness::Stale;
  }
  return Freshness::Expired;
}

bool exceeds_clock_skew(Timestamp observed_at, Timestamp received_at,
                        Duration max_skew) noexcept {
  if (!observed_at.known() || !received_at.known()) {
    return false;
  }
  if (observed_at <= received_at) {
    return false;
  }
  return time_difference(observed_at, received_at) > max_skew;
}

// ---------------------------------------------------------------------------
// SourceLedger
// ---------------------------------------------------------------------------

SourceLedger::SourceLedger(SourceId source, FlowGeneration generation, std::size_t capacity) {
  configure(source, generation, capacity);
}

void SourceLedger::configure(SourceId source, FlowGeneration generation, std::size_t capacity) {
  source_ = source;
  generation_ = generation;
  capacity_ = capacity == 0 ? 1 : capacity;
  counter_policy_ = CounterPolicy{};
  entries_.clear();
  prefix_ = Prefix{};
  content_conflicts_ = 0;
  duplicates_suppressed_ = 0;
  total_accepted_ = 0;
}

SourceLedger::InsertResult SourceLedger::insert(const Observation& observation) {
  InsertResult result;
  if (!observation.valid()) {
    result.kind = InsertResult::Kind::Refused;
    result.code = ErrorCode::InvalidArgument;
    result.detail = "observation is missing a key or a subject";
    return result;
  }
  if (observation.key.source != source_) {
    result.kind = InsertResult::Kind::Refused;
    result.code = ErrorCode::InvalidArgument;
    result.detail = "observation source does not match the ledger source";
    return result;
  }
  if (observation.subject.flow != generation_.flow() ||
      observation.subject.generation != generation_.generation()) {
    result.kind = InsertResult::Kind::Refused;
    result.code = ErrorCode::InvalidArgument;
    result.detail = "observation subject does not match the ledger generation";
    return result;
  }

  if (prefix_.folded_observations > 0) {
    // Anything at or below the highest folded key is inside the folded region.
    // Re-folding it would require the entries that were already compacted away,
    // so the runtime refuses it and says so instead of applying it out of
    // order.
    Observation bound;
    bound.key.incarnation = prefix_.folded_incarnation;
    bound.key.epoch = prefix_.folded_epoch;
    bound.key.revision = prefix_.folded_revision;
    bound.key.sequence = prefix_.folded_sequence;
    if (!canonical_less(bound, observation)) {
      // Inside the folded region. A redelivery inside the dedupe window is
      // recognised exactly and costs nothing; anything else is refused, counted
      // and never re-applied.
      if (fold_contains(prefix_, observation.key)) {
        ++duplicates_suppressed_;
        result.kind = InsertResult::Kind::Duplicate;
        result.code = ErrorCode::DuplicateEvidence;
        result.detail = "evidence was already folded into the durable accounting";
        return result;
      }
      ++late_rejected_;
      result.kind = InsertResult::Kind::Refused;
      result.code = ErrorCode::StaleRevision;
      result.detail = "evidence falls inside the compacted observation window";
      return result;
    }
  }

  const auto position = std::lower_bound(entries_.begin(), entries_.end(), observation,
                                         [](const Observation& a, const Observation& b) {
                                           return canonical_less(a, b);
                                         });

  const std::uint64_t incoming = content_digest(observation);
  if (position != entries_.end() && same_canonical_key(*position, observation)) {
    if (content_digest(*position) == incoming) {
      ++duplicates_suppressed_;
      result.kind = InsertResult::Kind::Duplicate;
      result.code = ErrorCode::DuplicateEvidence;
      result.detail = "identical evidence already stored";
      return result;
    }
    ++content_conflicts_;
    result.kind = InsertResult::Kind::ContentConflict;
    result.code = ErrorCode::EvidenceConflict;
    result.detail = "the same source key carried different content";
    // Deterministic tie break: retain the lexicographically smaller canonical
    // digest, so the retained set - and therefore the reconciled view - does
    // not depend on which copy arrived first.
    if (incoming < content_digest(*position)) {
      *position = observation;
      result.detail += " (larger digest refused)";
    } else {
      result.detail += " (smaller digest already retained)";
    }
    return result;
  }

  Observation stored = observation;
  entries_.insert(position, std::move(stored));
  ++total_accepted_;
  result.kind = InsertResult::Kind::Accepted;
  result.code = ErrorCode::Ok;

  if (entries_.size() > capacity_) {
    // Fold the lowest entries into the bounded prefix instead of dropping them.
    // Folding a prefix of the canonical order and then folding the remainder is
    // exactly the fold of the whole set, so nothing is lost.
    const std::size_t excess = entries_.size() - capacity_;
    FoldState state = from_prefix(prefix_);
    for (std::size_t i = 0; i < excess; ++i) {
      apply_observation(state, entries_[i], counter_policy_);
      fold_mark(state, entries_[i].key);
      state.folded_revision = entries_[i].key.revision;
      state.folded_sequence = entries_[i].key.sequence;
    }
    to_prefix(std::move(state), prefix_);
    prefix_.folded_observations += static_cast<std::uint32_t>(excess);
    entries_.erase(entries_.begin(),
                   entries_.begin() + static_cast<std::ptrdiff_t>(excess));
    result.folded = static_cast<std::uint32_t>(excess);
  }
  return result;
}

void SourceLedger::clear() noexcept {
  entries_.clear();
  prefix_ = Prefix{};
  content_conflicts_ = 0;
  duplicates_suppressed_ = 0;
  late_rejected_ = 0;
  total_accepted_ = 0;
}

SourceLedger::Prefix SourceLedger::folded_state() const {
  if (entries_.empty()) {
    return prefix_;
  }
  FoldState state = from_prefix(prefix_);
  for (const Observation& observation : entries_) {
    apply_observation(state, observation, counter_policy_);
    fold_mark(state, observation.key);
  }
  state.folded_incarnation = state.current_incarnation;
  state.folded_epoch = state.current_epoch;
  if (!entries_.empty()) {
    state.folded_revision = entries_.back().key.revision;
    state.folded_sequence = entries_.back().key.sequence;
  }
  Prefix folded;
  to_prefix(std::move(state), folded);
  folded.folded_observations =
      prefix_.folded_observations + static_cast<std::uint32_t>(entries_.size());
  return folded;
}

void SourceLedger::seed(const Prefix& prefix) {
  entries_.clear();
  prefix_ = prefix;
  content_conflicts_ = 0;
  duplicates_suppressed_ = 0;
  late_rejected_ = 0;
  total_accepted_ = prefix.folded_observations;
}

// ---------------------------------------------------------------------------
// Fold a ledger into a SourceView
// ---------------------------------------------------------------------------

SourceView fold_source_view(const SourceLedger& ledger, const SourceDescriptor& descriptor,
                            const FreshnessContext& ctx, const CounterPolicy& counters) {
  FoldState state = from_prefix(ledger.prefix());
  for (const Observation& observation : ledger.entries()) {
    apply_observation(state, observation, counters);
  }

  SourceView view;
  view.source = ledger.source();
  view.canonical_key = descriptor.canonical_key;
  view.authority = descriptor.authority;
  view.advisory_only = descriptor.advisory_only;
  view.capabilities = descriptor.capabilities;
  view.incarnation = state.current_incarnation;
  view.epoch = state.current_epoch;
  view.high_revision = state.high_revision;
  view.high_sequence = state.high_sequence;
  view.first_observed_at = state.first_observed_at;
  view.last_observed_at = state.last_observed_at;
  view.last_received_at = state.last_received_at;
  view.last_progress_at = state.last_progress_at;
  view.last_evidence = state.last_evidence;
  view.last_kind = state.last_kind;
  view.last_direction = state.last_direction;
  view.coverage = state.coverage;
  view.forward = state.forward;
  view.reverse = state.reverse;
  view.aggregate = state.aggregate;
  view.forward_totals = to_consumption(state.forward);
  view.reverse_totals = to_consumption(state.reverse);
  view.aggregate_totals = to_consumption(state.aggregate);
  view.observed_progress = state.observed_progress;

  if (state.declared_terminal.has_value() &&
      !is_terminal(state.declared_terminal.value_or(FlowState::Unknown))) {
    state.declared_terminal.reset();
  }
  view.declared_terminal = state.declared_terminal;
  view.declared_cause = state.declared_cause;

  view.accepted_observations = static_cast<std::uint32_t>(ledger.total_accepted());
  view.dropped_observations = ledger.prefix().folded_observations;
  view.duplicates_suppressed = static_cast<std::uint32_t>(ledger.duplicates_suppressed());
  view.content_conflicts = static_cast<std::uint32_t>(ledger.content_conflicts());
  view.historical_incarnations = state.historical_incarnations;
  view.distinct_incarnations = state.distinct_incarnations;
  view.after_terminal_observations = state.after_terminal_observations;
  view.distinct_incarnations = state.distinct_incarnations;
  view.endpoint_a = state.endpoint_a;
  view.endpoint_b = state.endpoint_b;
  view.path = state.path;
  view.links = state.links;
  view.queues = state.queues;
  view.hop_capacity_units = state.hop_capacity_units;
  view.has_queue = state.has_queue;

  for (std::size_t i = 0; i < kAnomalyKindCount; ++i) {
    view.anomaly_counts[i] = state.anomaly_counts[i];
  }
  // Every derived scalar is a projection of the same counts, so the summary
  // numbers and the per-kind table can never disagree.
  view.regressions = anomaly_count(state, AnomalyKind::CounterRegression);
  view.clock_skew_events = anomaly_count(state, AnomalyKind::ClockSkew);
  view.restart_gaps = anomaly_count(state, AnomalyKind::RestartGap);
  view.stale_epoch_events = anomaly_count(state, AnomalyKind::StaleEpochReplay);
  view.stale_incarnation_events = anomaly_count(state, AnomalyKind::StaleIncarnationReplay);
  view.unsupported_events = anomaly_count(state, AnomalyKind::UnsupportedMetric);
  view.incomplete_events = anomaly_count(state, AnomalyKind::IncompleteEvidence);
  view.saturated = view.forward.bytes.saturated() || view.forward.packets.saturated() ||
                   view.forward.retransmissions.saturated() ||
                   view.reverse.bytes.saturated() || view.reverse.packets.saturated() ||
                   view.reverse.retransmissions.saturated() ||
                   view.aggregate.bytes.saturated() || view.aggregate.packets.saturated() ||
                   view.aggregate.retransmissions.saturated();

  // Freshness is computed from the retained evidence, never stored.
  view.freshness = classify_freshness(view.last_observed_at, view.last_received_at, ctx);
  view.restored_from_journal = ctx.from_previous_epoch;
  view.reconfirmed = !ctx.from_previous_epoch;
  return view;
}

// ---------------------------------------------------------------------------
// Reconcile source views into a generation view
// ---------------------------------------------------------------------------

void sort_source_views(std::vector<SourceView>& views) {
  std::stable_sort(views.begin(), views.end(), [](const SourceView& a, const SourceView& b) {
    if (a.authority != b.authority) {
      return a.authority > b.authority;  // highest authority first
    }
    return a.source < b.source;          // then lowest identity, for determinism
  });
}

bool within_tolerance(std::uint64_t a, std::uint64_t b,
                      const ReconciliationPolicy& policy) noexcept {
  const std::uint64_t larger = a > b ? a : b;
  const std::uint64_t smaller = a > b ? b : a;
  const std::uint64_t absolute = larger - smaller;
  if (absolute <= policy.absolute_tolerance_bytes) {
    return true;
  }
  const Checked<std::uint64_t> relative =
      checked_mul_div(larger, policy.relative_tolerance_ppm, 1000000ull);
  if (relative.overflow) {
    return false;
  }
  return absolute <= relative.value;
}

namespace {

bool source_is_usable(const SourceView& view) {
  return !view.advisory_only && is_usable_for_liveness(view.freshness) && view.reconfirmed;
}

void accumulate_coverage(Coverage& target, const Coverage& source) {
  target.forward_complete = target.forward_complete || source.forward_complete;
  target.forward_partial = target.forward_partial || source.forward_partial;
  target.reverse_complete = target.reverse_complete || source.reverse_complete;
  target.reverse_partial = target.reverse_partial || source.reverse_partial;
}

Consumption sum_consumption(const Consumption& a, const Consumption& b) {
  Consumption out;
  const Checked<std::uint64_t> bytes = checked_add(a.bytes, b.bytes);
  const Checked<std::uint64_t> packets = checked_add(a.packets, b.packets);
  const Checked<std::uint64_t> retx = checked_add(a.retransmissions, b.retransmissions);
  const Checked<std::uint64_t> gaps = checked_add(a.unknown_intervals, b.unknown_intervals);
  out.bytes = bytes.value;
  out.packets = packets.value;
  out.retransmissions = retx.value;
  out.unknown_intervals = gaps.value;
  out.saturated = a.saturated || b.saturated || bytes.overflow || packets.overflow ||
                  retx.overflow || gaps.overflow;
  out.bytes_unsupported = a.bytes_unsupported && b.bytes_unsupported;
  out.packets_unsupported = a.packets_unsupported && b.packets_unsupported;
  out.retransmissions_unsupported = a.retransmissions_unsupported && b.retransmissions_unsupported;
  return out;
}

Consumption maximum_consumption(const Consumption& a, const Consumption& b) {
  Consumption out;
  out.bytes = a.bytes > b.bytes ? a.bytes : b.bytes;
  out.packets = a.packets > b.packets ? a.packets : b.packets;
  out.retransmissions = a.retransmissions > b.retransmissions ? a.retransmissions
                                                             : b.retransmissions;
  out.unknown_intervals =
      a.unknown_intervals < b.unknown_intervals ? a.unknown_intervals : b.unknown_intervals;
  out.saturated = a.saturated || b.saturated;
  out.bytes_unsupported = a.bytes_unsupported && b.bytes_unsupported;
  out.packets_unsupported = a.packets_unsupported && b.packets_unsupported;
  out.retransmissions_unsupported = a.retransmissions_unsupported && b.retransmissions_unsupported;
  return out;
}

Consumption minimum_consumption(const Consumption& a, const Consumption& b) {
  Consumption out;
  out.bytes = a.bytes < b.bytes ? a.bytes : b.bytes;
  out.packets = a.packets < b.packets ? a.packets : b.packets;
  out.retransmissions = a.retransmissions < b.retransmissions ? a.retransmissions
                                                             : b.retransmissions;
  out.unknown_intervals =
      a.unknown_intervals > b.unknown_intervals ? a.unknown_intervals : b.unknown_intervals;
  out.saturated = a.saturated || b.saturated;
  out.bytes_unsupported = a.bytes_unsupported || b.bytes_unsupported;
  out.packets_unsupported = a.packets_unsupported || b.packets_unsupported;
  out.retransmissions_unsupported = a.retransmissions_unsupported || b.retransmissions_unsupported;
  return out;
}

}  // namespace

GenerationView reconcile_generation(FlowId flow, GenerationId generation,
                                    std::vector<SourceView> source_views,
                                    const ReconciliationPolicy& policy, bool is_current) {
  // Every derived total is recomputed from the accumulators, so a caller can
  // never hand in a SourceView whose scalar totals disagree with its accounting.
  for (SourceView& source : source_views) {
    source.forward_totals = to_consumption(source.forward);
    source.reverse_totals = to_consumption(source.reverse);
    source.aggregate_totals = to_consumption(source.aggregate);
  }
  sort_source_views(source_views);

  GenerationView view;
  view.generation = FlowGeneration(flow, generation);
  view.is_current = is_current;
  view.sources = std::move(source_views);

  Coverage all_time;
  Coverage usable_coverage;
  std::vector<const SourceView*> usable;
  usable.reserve(view.sources.size());

  for (const SourceView& source : view.sources) {
    if (!source.advisory_only) {
      accumulate_coverage(all_time, source.coverage);
    }
    if (source_is_usable(source)) {
      usable.push_back(&source);
      accumulate_coverage(usable_coverage, source.coverage);
    } else if (source.advisory_only) {
      ++view.excluded_advisory_sources;
    } else {
      ++view.excluded_stale_sources;
    }

    // Timing roll-ups are taken over every source so that the generation knows
    // when it was last seen even if the evidence has since gone stale.
    if (!view.first_observed_at.known() || (source.first_observed_at.known() &&
                                            source.first_observed_at < view.first_observed_at)) {
      view.first_observed_at = source.first_observed_at;
    }
    if (source.last_observed_at > view.last_observed_at) {
      view.last_observed_at = source.last_observed_at;
    }
    if (source.last_received_at > view.last_received_at) {
      view.last_received_at = source.last_received_at;
    }
    if (source.last_progress_at > view.last_progress_at) {
      view.last_progress_at = source.last_progress_at;
    }
  }

  view.coverage = all_time;
  view.visibility = usable_coverage.classify();

  // Endpoint correlation only from sources that have declared endpoints.
  for (const SourceView& source : view.sources) {
    if (source.endpoint_a.valid() && !view.endpoint_a.valid()) {
      view.endpoint_a = source.endpoint_a;
    }
    if (source.endpoint_b.valid() && !view.endpoint_b.valid()) {
      view.endpoint_b = source.endpoint_b;
    }
    if (source.path.has_value() && !view.path.has_value()) {
      view.path = source.path;
    }
    if (source.has_queue && !view.has_queue_correlation) {
      view.has_queue_correlation = true;
    }
  }
  view.has_path_correlation = view.path.has_value();

  const std::size_t queue_budget = static_cast<std::size_t>(view.sources.size()) * 4u + 4u;
  for (const SourceView& source : view.sources) {
    for (const QueueId queue : source.queues) {
      if (view.queues.size() >= queue_budget) {
        break;
      }
      if (std::find(view.queues.begin(), view.queues.end(), queue) == view.queues.end()) {
        view.queues.push_back(queue);
      }
    }
    for (const LinkId link : source.links) {
      if (std::find(view.links.begin(), view.links.end(), link) == view.links.end()) {
        view.links.push_back(link);
      }
    }
    if (view.hop_capacity_units.empty() && !source.hop_capacity_units.empty()) {
      view.hop_capacity_units = source.hop_capacity_units;
    }
  }

  // --- consistency and totals -------------------------------------------
  const SourceView* primary = nullptr;
  if (!view.sources.empty()) {
    for (const SourceView& source : view.sources) {
      if (!source.advisory_only) {
        primary = &source;
        break;
      }
    }
  }
  if (primary != nullptr) {
    view.primary_source = primary->source;
  }

  const auto totals_of = [](const SourceView& source) {
    Consumption total = sum_consumption(source.forward_totals, source.reverse_totals);
    return sum_consumption(total, source.aggregate_totals);
  };

  if (!usable.empty()) {
    Consumption chosen = totals_of(*usable.front());
    switch (policy.method) {
      case ReconciliationPolicy::Method::PrimaryAuthoritative:
        break;
      case ReconciliationPolicy::Method::MaximumObserved:
        for (const SourceView* source : usable) {
          chosen = maximum_consumption(chosen, totals_of(*source));
        }
        break;
      case ReconciliationPolicy::Method::ConservativeMinimum:
        for (const SourceView* source : usable) {
          chosen = minimum_consumption(chosen, totals_of(*source));
        }
        break;
    }
    view.forward = usable.front()->forward_totals;
    view.reverse = usable.front()->reverse_totals;
    view.aggregate = usable.front()->aggregate_totals;
    view.total = chosen;
  } else if (primary != nullptr) {
    // Nothing is currently trustworthy, but the durable accounting is still
    // reported so that history is not erased by staleness.
    view.forward = primary->forward_totals;
    view.reverse = primary->reverse_totals;
    view.aggregate = primary->aggregate_totals;
    view.total = totals_of(*primary);
  }

  // --- disagreement ------------------------------------------------------
  // Reconciliation must be a total, order independent decision. The rules are:
  //   * no two usable sources disagree            -> Corroborated
  //   * every disagreement involves an authority  -> Resolved
  //     strictly above the dissenting source
  //   * some disagreement is between equals with
  //     no higher authority above them            -> Conflicting
  const auto strictly_above = [&usable](std::uint32_t authority) {
    for (const SourceView* source : usable) {
      if (source->authority > authority) {
        return true;
      }
    }
    return false;
  };

  view.consistency = Consistency::Unknown;
  if (usable.size() >= 2) {
    bool any_disagreement = false;
    bool any_unresolved = false;
    const SourceView* left = nullptr;
    const SourceView* right = nullptr;
    for (std::size_t i = 0; i < usable.size(); ++i) {
      for (std::size_t j = i + 1; j < usable.size(); ++j) {
        const Consumption a = totals_of(*usable[i]);
        const Consumption b = totals_of(*usable[j]);
        if (within_tolerance(a.bytes, b.bytes, policy)) {
          continue;
        }
        any_disagreement = true;
        if (left == nullptr) {
          left = usable[i];
          right = usable[j];
        }
        // The list is sorted by authority descending, so usable[i] is never
        // less authoritative than usable[j].
        const bool settled =
            usable[i]->authority > usable[j]->authority ||
            (policy.higher_authority_resolves &&
             strictly_above(usable[i]->authority));
        if (!settled) {
          any_unresolved = true;
        }
      }
    }
    if (!any_disagreement) {
      view.consistency = Consistency::Corroborated;
    } else if (!any_unresolved) {
      view.consistency = Consistency::Resolved;
    } else {
      view.consistency = Consistency::Conflicting;
      view.unresolved_conflict = true;
      view.conflict_detail = "equal-authority sources disagree: " +
                             flowobs::to_string(left->source) + " reports " +
                             std::to_string(totals_of(*left).bytes) + " bytes, " +
                             flowobs::to_string(right->source) + " reports " +
                             std::to_string(totals_of(*right).bytes) +
                             " bytes, and no higher authority exists to resolve it";
    }
  }

  // A path whose generation is not the one the evidence was collected under is
  // not usable for current attribution; the flag is surfaced rather than
  // silently trusted.
  view.freshness = Freshness::Unknown;
  for (const SourceView& source : view.sources) {
    if (source_is_usable(source)) {
      view.freshness = Freshness::Fresh;
      break;
    }
  }
  if (view.freshness == Freshness::Unknown) {
    for (const SourceView& source : view.sources) {
      if (source.freshness != Freshness::Unknown) {
        view.freshness = source.freshness;
        break;
      }
    }
  }
  return view;
}

// ---------------------------------------------------------------------------
// Lifecycle decision
// ---------------------------------------------------------------------------

StateDecision decide_generation_state(const GenerationView& view, FlowState previous,
                                      const ExpiryPolicy& expiry,
                                      const FreshnessContext& ctx) {
  StateDecision decision;
  decision.state = previous;
  decision.cause = StateCause::None;

  const auto finish = [&decision, previous](FlowState state, StateCause cause, std::string detail) {
    decision.state = state;
    decision.cause = cause;
    decision.detail = std::move(detail);
    decision.changed = state != previous;
    return decision;
  };

  // A terminal generation stays terminal: nothing in this design reopens it.
  if (is_terminal(previous)) {
    return finish(previous, StateCause::None, "generation is terminal");
  }

  std::vector<const SourceView*> usable;
  usable.reserve(view.sources.size());
  for (const SourceView& source : view.sources) {
    if (!source.advisory_only && is_usable_for_liveness(source.freshness) &&
        source.reconfirmed) {
      usable.push_back(&source);
    }
  }

  if (view.unresolved_conflict) {
    return finish(FlowState::Conflicting, StateCause::AuthorityConflict, view.conflict_detail);
  }

  // Completion and reset require both an explicit declaration and the
  // capability to make it. A source without the capability is recorded, and the
  // declaration is refused rather than honoured.
  //
  // A *proven* termination is a historical fact, not a statement about the
  // present: it keeps its force even once the evidence has aged out, which is
  // why this check considers every non-advisory source and not only the fresh
  // ones. Liveness and currency still require fresh, reconfirmed evidence.
  // A declaration the source is not entitled to make is ignored here; the
  // caller records it as a CapabilityUnsupported anomaly so that the refusal is
  // visible rather than silent.
  for (const SourceView& source : view.sources) {
    if (source.advisory_only || !source.declared_terminal.has_value()) {
      continue;
    }
    const FlowState declared = *source.declared_terminal;
    if (declared == FlowState::Completed) {
      if (source.capabilities.asserts_completion) {
        return finish(
            FlowState::Completed, StateCause::CompletionObserved,
            "completion declared by " + flowobs::to_string(source.source) +
                (source.reconfirmed ? "" : " (evidence carried over from a previous epoch)"));
      }
    } else if (declared == FlowState::Reset) {
      if (source.capabilities.asserts_reset) {
        return finish(FlowState::Reset, StateCause::ResetObserved,
                      "reset declared by " + flowobs::to_string(source.source));
      }
    }
  }

  const Timestamp reference = ctx.now;

  if (usable.empty()) {
    // No source currently holds trustworthy evidence.
    const bool has_restored = [&view] {
      for (const SourceView& source : view.sources) {
        if (source.restored_from_journal) {
          return true;
        }
      }
      return false;
    }();
    if (has_restored && expiry.enabled) {
      // A flow restored from a journal is not live. It is only Observed until
      // fresh evidence arrives in this runtime epoch, even if the persisted
      // record said Active.
      if (view.last_observed_at.known() && reference.known() &&
          time_difference(reference, view.last_observed_at) > expiry.expire_after) {
        return finish(FlowState::Expired, StateCause::ExpiryPolicy,
                      "restored evidence aged past expiry.expire_after");
      }
      return finish(FlowState::Observed, StateCause::RecoveredFromJournal,
                    "restored from journal; awaiting evidence in the current runtime epoch");
    }
    if (expiry.enabled && view.last_observed_at.known() && reference.known() &&
        time_difference(reference, view.last_observed_at) > expiry.expire_after) {
      return finish(FlowState::Expired, StateCause::ExpiryPolicy,
                    "no accepted evidence within expiry.expire_after");
    }
    if (expiry.enabled && previous == FlowState::Active && view.last_progress_at.known() &&
        reference.known() && time_difference(reference, view.last_progress_at) > expiry.idle_after) {
      return finish(FlowState::Idle, StateCause::IdleThresholdReached,
                    "no usable evidence for longer than expiry.idle_after");
    }
    if (previous == FlowState::Unknown) {
      return finish(FlowState::Observed, StateCause::FirstObservation,
                    "evidence exists but is not currently trustworthy");
    }
    return finish(previous, StateCause::None, "no change");
  }

  // Usable evidence exists.
  const bool any_progress = [&usable] {
    for (const SourceView* source : usable) {
      if (source->observed_progress) {
        return true;
      }
    }
    return false;
  }();

  Timestamp last_progress = view.last_progress_at;
  for (const SourceView* source : usable) {
    if (source->last_progress_at > last_progress) {
      last_progress = source->last_progress_at;
    }
  }

  if (!any_progress) {
    return finish(FlowState::Observed, StateCause::FirstObservation,
                  "usable evidence carries no progress");
  }
  if (expiry.enabled && last_progress.known() && reference.known() &&
      time_difference(reference, last_progress) > expiry.idle_after) {
    return finish(FlowState::Idle, StateCause::IdleThresholdReached,
                  "no progress within expiry.idle_after");
  }
  return finish(FlowState::Active, StateCause::ProgressObserved,
                "fresh evidence shows progress");
}

}  // namespace flowobs
