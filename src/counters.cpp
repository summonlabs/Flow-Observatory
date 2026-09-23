// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowobs/counters.hpp"

#include <algorithm>
#include <string>

#include "flowobs/checked.hpp"

namespace flowobs {

namespace {
void append_counter(std::string& out, const char* name,
                    const std::optional<std::uint64_t>& value) {
  out.push_back(' ');
  out.append(name);
  out.push_back('=');
  if (value.has_value()) {
    out.append(std::to_string(*value));
  } else {
    out.push_back('-');
  }
}
}  // namespace

std::string CounterSet::describe() const {
  std::string out;
  out.reserve(64);
  out.push_back('{');
  append_counter(out, "bytes", bytes);
  append_counter(out, "packets", packets);
  append_counter(out, "retransmissions", retransmissions);
  out.append(" }");
  return out;
}

CounterAccumulator::Outcome CounterAccumulator::observe(std::uint64_t value,
                                                        bool treat_regression_as_reset) noexcept {
  if (!baseline_.has_value()) {
    baseline_ = value;
    last_ = value;
    return Outcome::BaselineEstablished;
  }
  const std::uint64_t previous = last_.value_or(0);
  if (value > previous) {
    const Checked<std::uint64_t> sum = checked_add(accumulated_, value - previous);
    last_ = value;
    if (sum.overflow) {
      saturated_ = true;
      accumulated_ = std::numeric_limits<std::uint64_t>::max();
      return Outcome::Saturated;
    }
    accumulated_ = sum.value;
    return Outcome::Advanced;
  }
  if (value == previous) {
    return Outcome::Unchanged;
  }
  // Regression: the source's cumulative counter moved backwards.
  ++regressions_;
  if (!treat_regression_as_reset) {
    return Outcome::Held;
  }
  ++unknown_intervals_;
  baseline_ = value;
  last_ = value;
  return Outcome::Regressed;
}

void CounterAccumulator::start_new_interval(std::optional<std::uint64_t> baseline) noexcept {
  // Only a genuine break in observation produces an unknown interval.
  if (last_.has_value()) {
    ++unknown_intervals_;
  }
  baseline_ = baseline;
  last_ = baseline;
}

void CounterAccumulator::reset_all() noexcept {
  baseline_.reset();
  last_.reset();
  accumulated_ = 0;
  unknown_intervals_ = 0;
  regressions_ = 0;
  saturated_ = false;
}

std::uint64_t DirectionalAccounting::unknown_intervals() const noexcept {
  // One gap in observation produces one unknown interval for every counter
  // that was being tracked. Reporting the maximum keeps the number meaningful
  // as "how many stretches of this flow were not observed", rather than
  // triple-counting the same gap.
  return std::max({bytes.unknown_intervals(), packets.unknown_intervals(),
                   retransmissions.unknown_intervals()});
}

void DirectionalAccounting::reset_all() noexcept {
  bytes.reset_all();
  packets.reset_all();
  retransmissions.reset_all();
  bytes_unsupported = false;
  packets_unsupported = false;
  retransmissions_unsupported = false;
}

std::string Consumption::describe() const {
  std::string out;
  out.reserve(128);
  out.append("bytes=");
  if (bytes_unsupported) {
    out.push_back('-');
  } else {
    out.append(std::to_string(bytes));
  }
  out.append(" packets=");
  if (packets_unsupported) {
    out.push_back('-');
  } else {
    out.append(std::to_string(packets));
  }
  out.append(" retransmissions=");
  if (retransmissions_unsupported) {
    out.push_back('-');
  } else {
    out.append(std::to_string(retransmissions));
  }
  out.append(" unknown_intervals=");
  out.append(std::to_string(unknown_intervals));
  out.append(saturated ? " saturated=true" : " saturated=false");
  return out;
}

}  // namespace flowobs
