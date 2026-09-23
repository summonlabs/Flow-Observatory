// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Policy. Every decision the runtime makes about state, freshness,
// reconciliation and attribution is driven by an explicit, versioned policy
// object. Two runtimes configured with equal policies and fed the same
// evidence in any order produce byte-identical exports.

#ifndef FLOWOBS_POLICY_HPP
#define FLOWOBS_POLICY_HPP

#include <cstdint>
#include <string>

#include "flowobs/error.hpp"
#include "flowobs/export.hpp"
#include "flowobs/limits.hpp"
#include "flowobs/time.hpp"

namespace flowobs {

// How the runtime decides that evidence stopped being usable.
struct FLOWOBS_PUBLIC FreshnessPolicy final {
  // Evidence newer than this (relative to the evaluation instant) is Fresh.
  Duration fresh_window = Duration::from_seconds(30);
  // Evidence newer than this but older than fresh_window is Stale. Older than
  // this is Expired. Must be >= fresh_window.
  Duration expire_window = Duration::from_minutes(10);
  // When true, an observation whose observed_at is in the future relative to
  // the evaluation instant by more than Limits::max_clock_skew is clamped to
  // the receive instant and recorded as a ClockSkew anomaly. When false such
  // an observation is refused outright.
  bool clamp_future_timestamps = true;
  // When true, evidence restored from a journal written by a previous runtime
  // epoch is classified Unknown rather than Fresh/Stale until new evidence for
  // the same flow arrives in the current epoch. This is the mechanism that
  // prevents a restart from resurrecting active-flow liveness.
  bool require_reconfirmation_after_restart = true;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string describe() const;
};

// How the runtime closes generations that stopped producing evidence.
struct FLOWOBS_PUBLIC ExpiryPolicy final {
  // When false no generation is ever Expired by the runtime; only explicit
  // evidence moves state. Default true.
  bool enabled = true;
  // A generation with no progress evidence for this long leaves Active and
  // becomes Idle.
  Duration idle_after = Duration::from_seconds(30);
  // A generation with no accepted evidence at all for this long is closed as
  // Expired. Must be >= idle_after.
  Duration expire_after = Duration::from_minutes(15);

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string describe() const;
};

// How a counter regression (the source's cumulative counter went backwards) is
// handled. Neither choice ever produces negative or invented consumption.
struct FLOWOBS_PUBLIC CounterPolicy final {
  // true  -> the interval closes; the delta across the boundary is unknown and
  //          counted as an unknown interval.
  // false -> the previous value is held and nothing is consumed until the
  //          counter catches up or passes the held value.
  bool treat_regression_as_reset = true;
  // When true, and the source announces a reset (kind == CounterReset or
  // Reset), the interval closes without an unknown-interval penalty for a
  // clean reopen.
  bool honor_explicit_counter_reset = true;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string describe() const;
};

// How the runtime reconciles more than one source describing the same flow
// generation.
struct FLOWOBS_PUBLIC ReconciliationPolicy final {
  enum class Method : std::uint8_t {
    // Use the single highest-authority fresh source; tie broken by the lowest
    // SourceId. Deterministic and the default.
    PrimaryAuthoritative = 0,
    // Use the largest value any fresh source reports. An upper bound of
    // observed consumption; never smaller than any single reading.
    MaximumObserved = 1,
    // Use the smallest value any fresh source reports. A lower bound.
    ConservativeMinimum = 2,
  };

  Method method = Method::PrimaryAuthoritative;
  // Two fresh sources of equal authority are corroborated when their byte
  // counters differ by at most this many bytes (or by at most
  // relative_tolerance_ppm parts per million of the larger value, whichever is
  // greater).
  std::uint64_t absolute_tolerance_bytes = 0;
  std::uint64_t relative_tolerance_ppm = 0;
  // When true, a strictly higher-authority source resolves a disagreement and
  // the flow stays out of Conflicting.
  bool higher_authority_resolves = true;
  // When true, evidence from stale sources is shown in explanations but
  // excluded from every reconciled number.
  bool exclude_stale_from_totals = true;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string describe() const;
};

// How consumption is attributed to the resources a flow traverses.
struct FLOWOBS_PUBLIC AttributionPolicy final {
  enum class Method : std::uint8_t {
    // Attribute the whole observed consumption to the flow and its endpoints
    // only. The default: it invents nothing.
    EndpointOnly = 0,
    // Split evenly across the hop and queue records of the current generation,
    // with the integer remainder assigned deterministically to the lowest
    // index. Requires no extra metadata.
    UniformAcrossHops = 1,
    // Split in proportion to source-supplied capacity metadata. When any hop
    // lacks capacity the whole attribution is reported Unsupported rather than
    // guessed.
    ProportionalToCapacity = 2,
  };

  Method method = Method::EndpointOnly;
  // Generation staleness gate. When true, attribution for a generation whose
  // evidence is not Fresh is refused with ErrorCode::StaleGeneration rather
  // than reported as current.
  bool require_fresh_generation = true;
  // When true, a generation that is not the flow's current generation is
  // refused for "current" attribution.
  bool require_current_generation = true;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string describe() const;
};

// How the runtime behaves when it restarts against an existing journal.
struct FLOWOBS_PUBLIC RecoveryPolicy final {
  enum class TornTail : std::uint8_t {
    // Refuse to load a journal with a truncated final record.
    Reject = 0,
    // Drop a truncated final record and load the rest. Only the *final*
    // record may be torn; a corrupt record anywhere else is always an
    // integrity failure.
    TruncateTornTail = 1,
  };

  TornTail torn_tail = TornTail::TruncateTornTail;
  // Refuse to load a journal whose format version is outside
  // [kJournalMinReadableVersion, kJournalFormatVersion].
  bool enforce_format_version = true;
  // Refuse to load a journal written with a different identity scheme.
  bool enforce_identity_scheme = true;
  // When true, restored generations with a non-terminal state are reset to
  // Observed with cause RecoveredFromJournal and are not treated as live until
  // fresh evidence arrives in the current runtime epoch.
  bool demote_restored_liveness = true;
  // When true, the first observation after a restart for a restored generation
  // starts a new accounting interval instead of bridging the unobserved gap.
  bool new_accounting_interval_after_restart = true;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string describe() const;
};

// Bounds on a single query.
struct FLOWOBS_PUBLIC QueryPolicy final {
  std::size_t max_rows = 10000;
  Duration max_window = Duration::from_hours(1);
  bool include_anomalies = true;
  bool include_sources = true;
  bool include_generations = true;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string describe() const;
};

}  // namespace flowobs

#endif  // FLOWOBS_POLICY_HPP
