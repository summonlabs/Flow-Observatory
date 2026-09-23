// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Time is explicit everywhere. Every observation carries the instant the
// measuring source claims it observed the event (observed_at) and the instant
// this runtime received it (received_at). Freshness is a function of both plus
// the runtime's evaluation instant. Nothing in the runtime samples a clock
// implicitly on the ingest path.

#ifndef FLOWOBS_TIME_HPP
#define FLOWOBS_TIME_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "flowobs/checked.hpp"
#include "flowobs/export.hpp"

namespace flowobs {

class Duration;

// Nanoseconds since the Unix epoch (1970-01-01T00:00:00Z), ignoring leap
// seconds. A value of 0 means "unknown / not supplied" for every field that
// documents it as such; the epoch itself is expressed as 0 seconds and is
// therefore not a legal observed_at.
class FLOWOBS_PUBLIC Timestamp final {
 public:
  constexpr Timestamp() noexcept = default;
  constexpr explicit Timestamp(std::int64_t nanos) noexcept : nanos_(nanos) {}

  [[nodiscard]] static constexpr Timestamp from_nanos(std::int64_t nanos) noexcept {
    return Timestamp(nanos);
  }
  [[nodiscard]] static constexpr Timestamp from_seconds(std::int64_t seconds) noexcept {
    return Timestamp(seconds * 1000000000LL);
  }
  [[nodiscard]] static constexpr Timestamp unknown() noexcept { return Timestamp(); }

  [[nodiscard]] constexpr bool known() const noexcept { return nanos_ != 0; }
  [[nodiscard]] constexpr std::int64_t nanos() const noexcept { return nanos_; }

  friend constexpr bool operator==(Timestamp a, Timestamp b) noexcept {
    return a.nanos_ == b.nanos_;
  }
  friend constexpr bool operator!=(Timestamp a, Timestamp b) noexcept {
    return a.nanos_ != b.nanos_;
  }
  friend constexpr bool operator<(Timestamp a, Timestamp b) noexcept {
    return a.nanos_ < b.nanos_;
  }
  friend constexpr bool operator>(Timestamp a, Timestamp b) noexcept {
    return a.nanos_ > b.nanos_;
  }
  friend constexpr bool operator<=(Timestamp a, Timestamp b) noexcept {
    return a.nanos_ <= b.nanos_;
  }
  friend constexpr bool operator>=(Timestamp a, Timestamp b) noexcept {
    return a.nanos_ >= b.nanos_;
  }

  // Arithmetic is saturating and never wraps. The full checked implementations
  // live in time_add()/time_difference(); these operators are the readable
  // spelling of the same operation.
  friend Timestamp operator+(Timestamp base, Duration delta) noexcept;
  friend Timestamp operator-(Timestamp base, Duration delta) noexcept;
  friend Duration operator-(Timestamp later, Timestamp earlier) noexcept;

 private:
  std::int64_t nanos_ = 0;
};

// A signed span of nanoseconds. Always checked before use.
class FLOWOBS_PUBLIC Duration final {
 public:
  constexpr Duration() noexcept = default;
  constexpr explicit Duration(std::int64_t nanos) noexcept : nanos_(nanos) {}

  [[nodiscard]] static constexpr Duration from_nanos(std::int64_t nanos) noexcept {
    return Duration(nanos);
  }
  [[nodiscard]] static constexpr Duration from_micros(std::int64_t micros) noexcept {
    return Duration(micros * 1000LL);
  }
  [[nodiscard]] static constexpr Duration from_millis(std::int64_t millis) noexcept {
    return Duration(millis * 1000000LL);
  }
  [[nodiscard]] static constexpr Duration from_seconds(std::int64_t seconds) noexcept {
    return Duration(seconds * 1000000000LL);
  }
  [[nodiscard]] static constexpr Duration from_minutes(std::int64_t minutes) noexcept {
    return from_seconds(minutes * 60LL);
  }
  [[nodiscard]] static constexpr Duration from_hours(std::int64_t hours) noexcept {
    return from_seconds(hours * 3600LL);
  }
  [[nodiscard]] static constexpr Duration zero() noexcept { return Duration(); }
  [[nodiscard]] static constexpr Duration max() noexcept {
    return Duration(INT64_MAX);
  }

  [[nodiscard]] constexpr std::int64_t nanos() const noexcept { return nanos_; }
  [[nodiscard]] constexpr std::int64_t millis() const noexcept { return nanos_ / 1000000LL; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return nanos_ == 0; }
  [[nodiscard]] constexpr bool is_negative() const noexcept { return nanos_ < 0; }

  friend constexpr bool operator==(Duration a, Duration b) noexcept {
    return a.nanos_ == b.nanos_;
  }
  friend constexpr bool operator!=(Duration a, Duration b) noexcept {
    return a.nanos_ != b.nanos_;
  }
  friend constexpr bool operator<(Duration a, Duration b) noexcept {
    return a.nanos_ < b.nanos_;
  }
  friend constexpr bool operator>(Duration a, Duration b) noexcept {
    return a.nanos_ > b.nanos_;
  }
  friend constexpr bool operator<=(Duration a, Duration b) noexcept {
    return a.nanos_ <= b.nanos_;
  }
  friend constexpr bool operator>=(Duration a, Duration b) noexcept {
    return a.nanos_ >= b.nanos_;
  }

 private:
  std::int64_t nanos_ = 0;
};

// Signed difference b - a, saturating on overflow.
[[nodiscard]] FLOWOBS_PUBLIC Duration time_difference(Timestamp later,
                                                      Timestamp earlier) noexcept;

// a + d, saturating on overflow.
[[nodiscard]] FLOWOBS_PUBLIC Timestamp time_add(Timestamp base, Duration delta) noexcept;

inline Timestamp operator+(Timestamp base, Duration delta) noexcept {
  return time_add(base, delta);
}
inline Timestamp operator-(Timestamp base, Duration delta) noexcept {
  return time_add(base, Duration::from_nanos(-delta.nanos()));
}
inline Duration operator-(Timestamp later, Timestamp earlier) noexcept {
  return time_difference(later, earlier);
}

// Formats a timestamp as RFC 3339 UTC with nanosecond precision, e.g.
// "2026-01-02T03:04:05.123456789Z". Deterministic and locale independent.
[[nodiscard]] FLOWOBS_PUBLIC std::string format_timestamp(Timestamp ts);

// Parses RFC 3339 UTC with optional fractional seconds (1..9 digits). Returns
// false when the text is not a valid UTC instant in this exact grammar.
[[nodiscard]] FLOWOBS_PUBLIC bool parse_timestamp(std::string_view text,
                                                  Timestamp& out) noexcept;

// Formats a duration as "<sign><seconds>.<nanos>" with exactly nine fractional
// digits, e.g. "-1.500000000". Deterministic.
[[nodiscard]] FLOWOBS_PUBLIC std::string format_duration(Duration d);

}  // namespace flowobs

#endif  // FLOWOBS_TIME_HPP
