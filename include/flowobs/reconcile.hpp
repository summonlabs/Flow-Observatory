// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Reconciliation. The reconciled view of a flow generation is a *pure
// function* of
//
//     (accepted observation set, policy, evaluation instant)
//
// and of nothing else. It does not depend on the order in which observations
// arrived, on which worker thread applied them, or on how many times a
// duplicate was delivered. Every function in this header is deterministic and
// side-effect free; the property tests exercise exactly that.

#ifndef FLOWOBS_RECONCILE_HPP
#define FLOWOBS_RECONCILE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "flowobs/export.hpp"
#include "flowobs/flow.hpp"
#include "flowobs/observation.hpp"
#include "flowobs/policy.hpp"

namespace flowobs {

// Everything the freshness classification needs. No function reaches for a
// clock on its own.
struct FLOWOBS_PUBLIC FreshnessContext final {
  Timestamp now{};
  FreshnessPolicy policy{};
  // True when the evidence being classified was written by a previous runtime
  // epoch. Such evidence is Unknown until it is reconfirmed.
  bool from_previous_epoch = false;

  [[nodiscard]] static FreshnessContext make(Timestamp now, const FreshnessPolicy& policy) {
    FreshnessContext ctx;
    ctx.now = now;
    ctx.policy = policy;
    return ctx;
  }
};

// Classifies one observation. `observed_at` is the source's claim, `received_at`
// is when this runtime accepted it.
FLOWOBS_PUBLIC Freshness classify_freshness(Timestamp observed_at, Timestamp received_at,
                                            const FreshnessContext& ctx) noexcept;

// True when observed_at is further in the future than the tolerated skew.
FLOWOBS_PUBLIC bool exceeds_clock_skew(Timestamp observed_at, Timestamp received_at,
                                       Duration max_skew) noexcept;

// ---------------------------------------------------------------------------
// Bounded per-source ledger.
// ---------------------------------------------------------------------------

// The durable-in-memory store of accepted observations for one
// (flow, generation, source). Entries are kept in canonical order
// (incarnation, epoch, revision, sequence). When the ledger is at capacity the
// lowest entries are folded into a bounded prefix summary rather than dropped,
// so accounting is exactly the fold of the full accepted set in canonical
// order regardless of arrival order or capacity.
class FLOWOBS_PUBLIC SourceLedger final {
 public:
  // State carried forward from entries that were folded into the prefix.
  struct Prefix final {
    std::uint32_t folded_observations = 0;
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
    // Highest canonical key among the *folded* entries. Any observation at or
    // below this key belongs to the folded region and cannot be folded again,
    // so it is refused rather than applied out of order.
    IncarnationId folded_incarnation{};
    EpochId folded_epoch{};
    RevisionId folded_revision{};
    SequenceNo folded_sequence{};
    // Exact duplicate detection for the folded region: a 64 wide sliding window
    // over the source sequence of the current (incarnation, epoch). Within the
    // window a redelivery is recognised as a duplicate; outside it the runtime
    // says "already accounted for or out of order" and refuses rather than
    // re-applying.
    bool folded_seq_exact = true;
    std::uint64_t folded_seq_low = 0;
    std::uint64_t folded_seq_bits = 0;
    std::uint32_t anomaly_counts[kAnomalyKindCount] = {};
    std::uint32_t historical_incarnations = 0;
    std::uint32_t after_terminal_observations = 0;
    bool observed_progress = false;
    std::uint32_t distinct_incarnations = 0;
    EndpointId endpoint_a{};
    EndpointId endpoint_b{};
    std::optional<PathId> path;
    std::vector<LinkId> links;
    std::vector<QueueId> queues;
    std::vector<std::uint64_t> hop_capacity_units;
    bool has_queue = false;
  };

  SourceLedger() = default;
  SourceLedger(SourceId source, FlowGeneration generation, std::size_t capacity);

  void configure(SourceId source, FlowGeneration generation, std::size_t capacity);

  // Outcome of inserting one observation.
  struct InsertResult final {
    enum class Kind : std::uint8_t {
      Accepted = 0,        // new evidence, stored
      Duplicate = 1,       // exact duplicate of stored evidence: no-op
      ContentConflict = 2, // same key, different content: refused
      Refused = 3,         // structurally refused (limit, invalid)
    };
    Kind kind = Kind::Accepted;
    ErrorCode code = ErrorCode::Ok;
    std::string detail;
    std::uint32_t folded = 0;  // entries folded by this insert
    [[nodiscard]] bool accepted() const noexcept { return kind == Kind::Accepted; }
    [[nodiscard]] bool idempotent() const noexcept { return kind == Kind::Duplicate; }
  };

  // Inserts evidence. Idempotent for exact duplicates. When a key is already
  // present with different content the entry with the *smaller canonical
  // digest* is retained, so the outcome does not depend on arrival order.
  InsertResult insert(const Observation& observation);

  [[nodiscard]] SourceId source() const noexcept { return source_; }
  [[nodiscard]] FlowGeneration generation() const noexcept { return generation_; }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] const Prefix& prefix() const noexcept { return prefix_; }
  [[nodiscard]] const std::vector<Observation>& entries() const noexcept { return entries_; }
  [[nodiscard]] std::uint64_t content_conflicts() const noexcept { return content_conflicts_; }
  [[nodiscard]] std::uint64_t duplicates_suppressed() const noexcept {
    return duplicates_suppressed_;
  }
  // Evidence refused because it fell inside the compacted observation window.
  [[nodiscard]] std::uint64_t late_rejected() const noexcept { return late_rejected_; }
  [[nodiscard]] std::uint64_t total_accepted() const noexcept { return total_accepted_; }

  void clear() noexcept;

  // Replaces the ledger contents with a previously folded prefix. Used by
  // journal recovery: the folded state is exactly the accounting of every
  // observation that was accepted before the snapshot was written, so nothing
  // is lost and nothing is re-derived from a wall clock.
  void seed(const Prefix& prefix);

  // The complete folded state of every accepted observation: the existing
  // prefix with all retained entries folded on top. Persistence uses this so
  // that a snapshot records the whole accounting rather than only the part that
  // has already been compacted away.
  [[nodiscard]] Prefix folded_state() const;

  // The counter policy used when entries are folded. It is recorded when the
  // ledger is configured so that folding is reproducible.
  void set_counter_policy(const CounterPolicy& policy) { counter_policy_ = policy; }

 private:
  SourceId source_{};
  FlowGeneration generation_{};
  CounterPolicy counter_policy_{};
  std::size_t capacity_ = 0;
  std::vector<Observation> entries_;  // canonical order
  Prefix prefix_{};
  std::uint64_t content_conflicts_ = 0;
  std::uint64_t duplicates_suppressed_ = 0;
  std::uint64_t late_rejected_ = 0;
  std::uint64_t total_accepted_ = 0;
};

// ---------------------------------------------------------------------------
// Pure folds.
// ---------------------------------------------------------------------------

// Folds a ledger plus its source descriptor into a SourceView. Pure: calling it
// twice on unchanged inputs yields identical results.
FLOWOBS_PUBLIC SourceView fold_source_view(const SourceLedger& ledger,
                                           const SourceDescriptor& descriptor,
                                           const FreshnessContext& ctx,
                                           const CounterPolicy& counters);

// Folds already-folded source views into a generation view. `source_views` is
// sorted internally, so the caller's ordering is irrelevant.
FLOWOBS_PUBLIC GenerationView reconcile_generation(FlowId flow, GenerationId generation,
                                                   std::vector<SourceView> source_views,
                                                   const ReconciliationPolicy& policy,
                                                   bool is_current);

// Pure lifecycle decision for one generation. `previous` is the state the
// runtime last recorded for this generation (Unknown for a new generation).
//
// A decision whose cause is StateCause::None leaves the recorded cause
// untouched: `None` is the runtime's way of saying "the previous cause still
// stands", which is what stops a terminal generation from having its cause
// rewritten on every subsequent observation.
struct FLOWOBS_PUBLIC StateDecision final {
  FlowState state = FlowState::Unknown;
  StateCause cause = StateCause::None;
  std::string detail;
  bool changed = false;
};

FLOWOBS_PUBLIC StateDecision decide_generation_state(const GenerationView& view,
                                                     FlowState previous,
                                                     const ExpiryPolicy& expiry,
                                                     const FreshnessContext& ctx);

// Sorts source views into the canonical reconciliation order: authority
// descending, then SourceId ascending. Stable and total.
FLOWOBS_PUBLIC void sort_source_views(std::vector<SourceView>& views);

// True when a and b are within the tolerance configured by the policy.
FLOWOBS_PUBLIC bool within_tolerance(std::uint64_t a, std::uint64_t b,
                                     const ReconciliationPolicy& policy) noexcept;

}  // namespace flowobs

#endif  // FLOWOBS_RECONCILE_HPP
