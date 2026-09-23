// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowobs/time.hpp"

#include <cstdio>
#include <string>

namespace flowobs {

namespace {
constexpr std::int64_t kNanosPerSecond = 1000000000LL;
constexpr std::int64_t kNanosPerDay = 86400LL * kNanosPerSecond;

// Days from 1970-01-01 for a proleptic Gregorian date. Howard Hinnant's
// days_from_civil, valid for the whole range we care about.
constexpr std::int64_t days_from_civil(std::int64_t y, int m, int d) noexcept {
  y -= (m <= 2) ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = static_cast<int>(y - era * 400);
  const int doy = (153 * (m + ((m > 2) ? -1 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

constexpr void civil_from_days(std::int64_t z, std::int64_t& y, int& m, int& d) noexcept {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int doe = static_cast<int>(z - era * 146097);
  const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = static_cast<std::int64_t>(yoe) + era * 400;
  const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + ((mp < 10) ? 3 : -9);
  y += (m <= 2) ? 1 : 0;
}

void append_fixed(std::string& out, std::int64_t value, int width) {
  char buffer[32];
  // Widths used here are small and always positive; the value is non-negative.
  std::snprintf(buffer, sizeof(buffer), "%0*lld", width, value);
  out.append(buffer);
}
}  // namespace

Duration time_difference(Timestamp later, Timestamp earlier) noexcept {
  const Checked<std::int64_t> difference =
      checked_sub_signed(later.nanos(), earlier.nanos());
  if (difference.overflow) {
    return difference.value < 0 ? Duration::from_nanos(INT64_MIN) : Duration::max();
  }
  return Duration::from_nanos(difference.value);
}

Timestamp time_add(Timestamp base, Duration delta) noexcept {
  const Checked<std::int64_t> sum = checked_add_signed(base.nanos(), delta.nanos());
  if (sum.overflow) {
    return Timestamp::from_nanos(sum.value < 0 ? INT64_MIN : INT64_MAX);
  }
  return Timestamp::from_nanos(sum.value);
}

std::string format_timestamp(Timestamp ts) {
  std::string out;
  out.reserve(32);
  std::int64_t nanos = ts.nanos();
  if (nanos == 0) {
    return "1970-01-01T00:00:00.000000000Z";
  }
  const std::int64_t days = nanos / kNanosPerDay;
  std::int64_t rem = nanos % kNanosPerDay;
  if (rem < 0) {
    rem += kNanosPerDay;
  }
  std::int64_t year = 0;
  int month = 1;
  int day = 1;
  civil_from_days(days, year, month, day);

  append_fixed(out, year, 4);
  out.push_back('-');
  append_fixed(out, static_cast<std::int64_t>(month), 2);
  out.push_back('-');
  append_fixed(out, static_cast<std::int64_t>(day), 2);
  out.push_back('T');
  append_fixed(out, rem / (3600LL * kNanosPerSecond), 2);
  out.push_back(':');
  append_fixed(out, (rem / (60LL * kNanosPerSecond)) % 60LL, 2);
  out.push_back(':');
  append_fixed(out, (rem / kNanosPerSecond) % 60LL, 2);
  out.push_back('.');
  append_fixed(out, rem % kNanosPerSecond, 9);
  out.push_back('Z');
  return out;
}

bool parse_timestamp(std::string_view text, Timestamp& out) noexcept {
  // Strict grammar: YYYY-MM-DDTHH:MM:SS[.f{1,9}]Z
  if (text.size() < 20 || text.size() > 30) {
    return false;
  }
  auto digit = [](char c) { return c >= '0' && c <= '9'; };
  for (std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 11u, 12u, 14u, 15u, 17u, 18u}) {
    if (!digit(text[i])) {
      return false;
    }
  }
  if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
      text[16] != ':') {
    return false;
  }
  const auto num = [&](std::size_t at, std::size_t len) -> std::int64_t {
    std::int64_t value = 0;
    for (std::size_t i = 0; i < len; ++i) {
      value = value * 10 + (text[at + i] - '0');
    }
    return value;
  };
  const std::int64_t year = num(0, 4);
  const std::int64_t month = num(5, 2);
  const std::int64_t day = num(8, 2);
  const std::int64_t hour = num(11, 2);
  const std::int64_t minute = num(14, 2);
  const std::int64_t second = num(17, 2);
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 ||
      second > 60) {
    return false;
  }
  std::int64_t frac = 0;
  std::size_t pos = 19;
  if (pos < text.size() && text[pos] == '.') {
    ++pos;
    std::size_t digits = 0;
    while (pos < text.size() && digit(text[pos])) {
      if (digits < 9) {
        frac = frac * 10 + (text[pos] - '0');
      }
      ++digits;
      ++pos;
    }
    if (digits == 0 || digits > 9) {
      return false;
    }
    for (std::size_t i = digits; i < 9; ++i) {
      frac *= 10;
    }
  }
  if (pos >= text.size() || text[pos] != 'Z' || pos + 1 != text.size()) {
    return false;
  }
  const std::int64_t days =
      days_from_civil(year, static_cast<int>(month), static_cast<int>(day));
  const std::int64_t epoch_seconds = days * 86400 + (hour * 3600 + minute * 60 + second);
  const Checked<std::uint64_t> seconds_nanos =
      checked_mul(static_cast<std::uint64_t>(epoch_seconds),
                  static_cast<std::uint64_t>(kNanosPerSecond));
  if (seconds_nanos.overflow) {
    return false;
  }
  const Checked<std::int64_t> total = checked_add_signed(
      static_cast<std::int64_t>(seconds_nanos.value), frac);
  if (total.overflow) {
    return false;
  }
  out = Timestamp::from_nanos(total.value);
  return true;
}

std::string format_duration(Duration d) {
  std::string out;
  out.reserve(24);
  std::int64_t nanos = d.nanos();
  if (nanos < 0) {
    out.push_back('-');
    // INT64_MIN would overflow on negation; saturate explicitly.
    nanos = (nanos == INT64_MIN) ? INT64_MAX : -nanos;
  }
  out.append(std::to_string(nanos / kNanosPerSecond));
  out.push_back('.');
  append_fixed(out, nanos % kNanosPerSecond, 9);
  return out;
}

}  // namespace flowobs
