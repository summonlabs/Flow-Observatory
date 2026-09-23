// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowobs/clock.hpp"

#include <chrono>

namespace flowobs {

Clock::~Clock() = default;

Timestamp SystemClock::now() const {
  const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  const auto nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
  return Timestamp::from_nanos(static_cast<std::int64_t>(nanos));
}

std::int64_t SystemClock::monotonic_nanos() const {
  const auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
  const auto nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
  return static_cast<std::int64_t>(nanos);
}

const SystemClock& SystemClock::instance() noexcept {
  static const SystemClock clock;
  return clock;
}

}  // namespace flowobs
