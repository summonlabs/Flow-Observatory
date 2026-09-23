// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Checked arithmetic for every size, counter or duration that originates
// outside the runtime (wire frames, journals, observation payloads). The
// runtime never silently wraps.

#ifndef FLOWOBS_CHECKED_HPP
#define FLOWOBS_CHECKED_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "flowobs/error.hpp"
#include "flowobs/export.hpp"

namespace flowobs {

// Result of a checked operation. `overflow` is true when the mathematical
// result did not fit; `value` then holds a saturated value that is safe to
// store but must not be treated as the true result.
template <class T>
struct Checked final {
  static_assert(std::is_integral_v<T>, "Checked is defined for integral types");
  T value{};
  bool overflow = false;

  [[nodiscard]] constexpr bool ok() const noexcept { return !overflow; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return !overflow; }
};

template <class T>
constexpr Checked<T> checked_ok(T value) noexcept {
  return Checked<T>{value, false};
}

template <class T>
constexpr Checked<T> checked_saturated() noexcept {
  return Checked<T>{std::numeric_limits<T>::max(), true};
}

// Sum of two unsigned values with saturation on overflow.
template <class T>
constexpr Checked<T> checked_add(T a, T b) noexcept {
  static_assert(std::is_unsigned_v<T>, "checked_add is defined for unsigned types");
  const T max = std::numeric_limits<T>::max();
  if (a > static_cast<T>(max - b)) {
    return checked_saturated<T>();
  }
  return checked_ok(static_cast<T>(a + b));
}

// Difference of two unsigned values. Underflow is reported, never wrapped.
template <class T>
constexpr Checked<T> checked_sub(T a, T b) noexcept {
  static_assert(std::is_unsigned_v<T>, "checked_sub is defined for unsigned types");
  if (b > a) {
    return Checked<T>{T{0}, true};
  }
  return checked_ok(static_cast<T>(a - b));
}

// Product of two unsigned values with saturation on overflow.
template <class T>
constexpr Checked<T> checked_mul(T a, T b) noexcept {
  static_assert(std::is_unsigned_v<T>, "checked_mul is defined for unsigned types");
  if (a == 0 || b == 0) {
    return checked_ok(T{0});
  }
  const T max = std::numeric_limits<T>::max();
  if (a > static_cast<T>(max / b)) {
    return checked_saturated<T>();
  }
  return checked_ok(static_cast<T>(a * b));
}

// Narrowing conversion with a range check. Used when a value read from the
// wire is stored into a smaller field.
template <class To, class From>
constexpr Checked<To> checked_narrow(From value) noexcept {
  static_assert(std::is_unsigned_v<To>, "checked_narrow targets unsigned types");
  static_assert(std::is_integral_v<From>, "checked_narrow sources integral types");
  if constexpr (std::is_signed_v<From>) {
    if (value < 0) {
      return Checked<To>{To{0}, true};
    }
  }
  using UnsignedFrom = std::make_unsigned_t<From>;
  const UnsignedFrom u = static_cast<UnsignedFrom>(value);
  if (u > static_cast<UnsignedFrom>(std::numeric_limits<To>::max())) {
    return checked_saturated<To>();
  }
  return checked_ok(static_cast<To>(u));
}

// Magnitude of a signed 64-bit value as an unsigned 64-bit value. Written so
// that INT64_MIN is handled without overflow.
constexpr std::uint64_t signed_magnitude(std::int64_t value) noexcept {
  if (value >= 0) {
    return static_cast<std::uint64_t>(value);
  }
  return static_cast<std::uint64_t>(-(value + 1)) + 1u;
}

// Re-encodes a signed result from a sign flag and a magnitude, saturating when
// the magnitude does not fit into int64.
constexpr Checked<std::int64_t> signed_from(bool negative,
                                            std::uint64_t magnitude) noexcept {
  constexpr std::uint64_t kMaxPositive =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  constexpr std::uint64_t kMaxNegative = kMaxPositive + 1u;
  if (negative) {
    if (magnitude > kMaxNegative) {
      return Checked<std::int64_t>{std::numeric_limits<std::int64_t>::min(), true};
    }
    if (magnitude == kMaxNegative) {
      return Checked<std::int64_t>{std::numeric_limits<std::int64_t>::min(), false};
    }
    return Checked<std::int64_t>{-static_cast<std::int64_t>(magnitude), false};
  }
  if (magnitude > kMaxPositive) {
    return Checked<std::int64_t>{std::numeric_limits<std::int64_t>::max(), true};
  }
  return Checked<std::int64_t>{static_cast<std::int64_t>(magnitude), false};
}

// Signed 64-bit add used for timestamps and durations. Total: every input pair
// produces either the exact signed result or a saturated one with \`overflow\` set.
// The arithmetic is carried out on magnitudes so that INT64_MIN is handled
// without ever forming an unrepresentable intermediate.
constexpr Checked<std::int64_t> checked_add_signed(std::int64_t a, std::int64_t b) noexcept {
  const bool a_negative = a < 0;
  const bool b_negative = b < 0;
  const std::uint64_t magnitude_a = signed_magnitude(a);
  const std::uint64_t magnitude_b = signed_magnitude(b);
  if (a_negative == b_negative) {
    const Checked<std::uint64_t> sum = checked_add(magnitude_a, magnitude_b);
    if (sum.overflow) {
      return Checked<std::int64_t>{a_negative ? std::numeric_limits<std::int64_t>::min()
                                              : std::numeric_limits<std::int64_t>::max(),
                                   true};
    }
    return signed_from(a_negative, sum.value);
  }
  if (magnitude_a >= magnitude_b) {
    return signed_from(a_negative, magnitude_a - magnitude_b);
  }
  return signed_from(b_negative, magnitude_b - magnitude_a);
}

// Signed 64-bit a - b. Total, and safe for INT64_MIN on either side.
constexpr Checked<std::int64_t> checked_sub_signed(std::int64_t a, std::int64_t b) noexcept {
  const bool a_negative = a < 0;
  const bool negated_b_negative = b >= 0;
  const std::uint64_t magnitude_a = signed_magnitude(a);
  const std::uint64_t magnitude_b = signed_magnitude(b);
  if (a_negative == negated_b_negative) {
    const Checked<std::uint64_t> sum = checked_add(magnitude_a, magnitude_b);
    if (sum.overflow) {
      return Checked<std::int64_t>{a_negative ? std::numeric_limits<std::int64_t>::min()
                                              : std::numeric_limits<std::int64_t>::max(),
                                   true};
    }
    return signed_from(a_negative, sum.value);
  }
  if (magnitude_a >= magnitude_b) {
    return signed_from(a_negative, magnitude_a - magnitude_b);
  }
  return signed_from(negated_b_negative, magnitude_b - magnitude_a);
}

// Narrowing an unsigned value into a signed 64-bit timestamp.
constexpr Checked<std::int64_t> checked_to_i64(std::uint64_t value) noexcept {
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return Checked<std::int64_t>{std::numeric_limits<std::int64_t>::max(), true};
  }
  return checked_ok(static_cast<std::int64_t>(value));
}

// Saturating multiply-add used by rate and throughput estimators.
constexpr Checked<std::uint64_t> checked_mul_div(std::uint64_t value, std::uint64_t num,
                                                 std::uint64_t den) noexcept {
  if (den == 0) {
    return Checked<std::uint64_t>{std::uint64_t{0}, true};
  }
  const Checked<std::uint64_t> product = checked_mul(value, num);
  if (product.overflow) {
    return product;
  }
  return checked_ok(product.value / den);
}

}  // namespace flowobs

#endif  // FLOWOBS_CHECKED_HPP
