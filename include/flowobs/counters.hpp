// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Counter handling. The single most important rule in this file: a counter
// that is absent is not a counter that is zero, and a counter that went
// backwards is not negative consumption.

#ifndef FLOWOBS_COUNTERS_HPP
#define FLOWOBS_COUNTERS_HPP

#include <cstdint>
#include <optional>
#include <string>

#include "flowobs/export.hpp"

namespace flowobs {

// A triple of optional cumulative counters. nullopt means "not supplied by the
// source"; it is never coerced to 0.
struct FLOWOBS_PUBLIC CounterSet final {
  std::optional<std::uint64_t> bytes;
  std::optional<std::uint64_t> packets;
  std::optional<std::uint64_t> retransmissions;

  [[nodiscard]] bool any() const noexcept {
    return bytes.has_value() || packets.has_value() || retransmissions.has_value();
  }
  [[nodiscard]] bool all_absent() const noexcept { return !any(); }

  friend bool operator==(const CounterSet& a, const CounterSet& b) noexcept {
    return a.bytes == b.bytes && a.packets == b.packets &&
           a.retransmissions == b.retransmissions;
  }
  friend bool operator!=(const CounterSet& a, const CounterSet& b) noexcept {
    return !(a == b);
  }

  [[nodiscard]] std::string describe() const;
};

// Monotone accounting for one cumulative counter inside one accounting
// interval. A regression closes the interval and opens a new one whose initial
// delta is *unknown*, counted as such and never invented.
class FLOWOBS_PUBLIC CounterAccumulator final {
 public:
  CounterAccumulator() = default;

  // Feeds a newly observed absolute value. Returns a classification of what
  // happened so the caller can record the right anomaly.
  enum class Outcome : std::uint8_t {
    BaselineEstablished = 0,  // first value of the interval, no consumption
    Advanced = 1,             // value grew, delta added
    Unchanged = 2,            // value identical, no consumption
    Regressed = 3,            // value shrank: interval closed, gap unknown
    Held = 4,                 // value shrank and the policy says hold
    Saturated = 5,            // accumulation would overflow; saturated
  };

  // `treat_regression_as_reset` comes from the engine policy. When false a
  // regression holds the previous value and consumes nothing.
  Outcome observe(std::uint64_t value, bool treat_regression_as_reset) noexcept;

  // Starts a new interval without consuming anything. Used when a generation
  // advances or when the runtime restarts and refuses to bridge a gap.
  void start_new_interval(std::optional<std::uint64_t> baseline) noexcept;

  [[nodiscard]] std::optional<std::uint64_t> last_value() const noexcept {
    return last_;
  }
  [[nodiscard]] std::optional<std::uint64_t> baseline() const noexcept {
    return baseline_;
  }
  // Consumption proven by observed deltas inside the current interval.
  [[nodiscard]] std::uint64_t accumulated() const noexcept { return accumulated_; }
  // Number of interval boundaries where the delta across the boundary is not
  // knowable (counter regression, runtime restart).
  [[nodiscard]] std::uint64_t unknown_intervals() const noexcept {
    return unknown_intervals_;
  }
  [[nodiscard]] std::uint64_t regressions() const noexcept { return regressions_; }
  [[nodiscard]] bool saturated() const noexcept { return saturated_; }
  [[nodiscard]] bool have_baseline() const noexcept { return baseline_.has_value(); }

  // Durable projection of the accumulator, used by the journal codec. It is
  // the complete state: restoring a snapshot reproduces the accumulator
  // exactly, including the unknown-interval and regression counts.
  struct Snapshot final {
    std::optional<std::uint64_t> baseline;
    std::optional<std::uint64_t> last;
    std::uint64_t accumulated = 0;
    std::uint64_t unknown_intervals = 0;
    std::uint64_t regressions = 0;
    bool saturated = false;
  };
  [[nodiscard]] Snapshot snapshot() const noexcept {
    return Snapshot{baseline_, last_, accumulated_, unknown_intervals_, regressions_,
                    saturated_};
  }
  void restore(const Snapshot& snapshot) noexcept {
    baseline_ = snapshot.baseline;
    last_ = snapshot.last;
    accumulated_ = snapshot.accumulated;
    unknown_intervals_ = snapshot.unknown_intervals;
    regressions_ = snapshot.regressions;
    saturated_ = snapshot.saturated;
  }

  void reset_all() noexcept;

 private:
  std::optional<std::uint64_t> baseline_;
  std::optional<std::uint64_t> last_;
  std::uint64_t accumulated_ = 0;
  std::uint64_t unknown_intervals_ = 0;
  std::uint64_t regressions_ = 0;
  bool saturated_ = false;
};

// The accounting for one direction of one flow generation.
struct FLOWOBS_PUBLIC DirectionalAccounting final {
  CounterAccumulator bytes;
  CounterAccumulator packets;
  CounterAccumulator retransmissions;
  // True when the source explicitly reported the counter as unsupported.
  bool bytes_unsupported = false;
  bool packets_unsupported = false;
  bool retransmissions_unsupported = false;

  [[nodiscard]] bool consumed_anything_known() const noexcept {
    return bytes.accumulated() != 0 || packets.accumulated() != 0 ||
           retransmissions.accumulated() != 0;
  }
  [[nodiscard]] std::uint64_t unknown_intervals() const noexcept;
  void reset_all() noexcept;
};

// Consumption attributed to a flow, a generation, a link, a queue or an
// endpoint. `unknown_intervals > 0` means the reported totals are a lower
// bound: the runtime observed the endpoints of the gap but not the gap itself.
struct FLOWOBS_PUBLIC Consumption final {
  std::uint64_t bytes = 0;
  std::uint64_t packets = 0;
  std::uint64_t retransmissions = 0;
  std::uint64_t unknown_intervals = 0;
  bool bytes_unsupported = false;
  bool packets_unsupported = false;
  bool retransmissions_unsupported = false;
  bool saturated = false;

  [[nodiscard]] bool is_exact() const noexcept { return unknown_intervals == 0 && !saturated; }
  [[nodiscard]] std::string describe() const;
};

}  // namespace flowobs

#endif  // FLOWOBS_COUNTERS_HPP
