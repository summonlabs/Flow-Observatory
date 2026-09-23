// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Minimal self-contained test harness.
//
// Deliberate properties:
//   * No timeouts of any kind. A test either completes or the process does not
//     terminate; nothing is ever "passed" because a deadline expired.
//   * No third-party dependency, so the suite builds in any environment that
//     can build the runtime.
//   * Deterministic output ordering: tests run in registration order and the
//     exit code is the number of failing tests (capped at 125).

#ifndef FLOWOBS_TESTS_FOTEST_HPP
#define FLOWOBS_TESTS_FOTEST_HPP

#include <cstdint>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace fotest {

struct TestAbort final {
  std::string message;
};

struct Context final {
  std::string suite;
  std::string name;
  std::size_t checks = 0;
  std::size_t failures = 0;
  std::uint64_t seed = 0;
  std::vector<std::string> messages;

  void fail(const std::string& message) {
    ++failures;
    if (messages.size() < 32) {
      // Long values (a JSON export, for instance) are clipped: the point of a
      // failure message is to locate the failure, not to dump the world.
      constexpr std::size_t kMaxMessage = 900;
      if (message.size() > kMaxMessage) {
        messages.push_back(message.substr(0, kMaxMessage) + "... [truncated]");
      } else {
        messages.push_back(message);
      }
    }
  }
};

using TestFn = void (*)(Context&);

struct Entry final {
  std::string suite;
  std::string name;
  TestFn fn;
};

std::vector<Entry>& registry();
bool register_test(const char* suite, const char* name, TestFn fn);
int run_all(int argc, char** argv);

// ---------------------------------------------------------------------------
// Value rendering used by the equality checks.
// ---------------------------------------------------------------------------

template <class T, class = void>
struct has_stream_operator : std::false_type {};

template <class T>
struct has_stream_operator<
    T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <class T>
std::string to_text(const T& value) {
  if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (has_stream_operator<T>::value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else {
    return "<unprintable>";
  }
}

inline std::string to_text(const std::string& value) { return value; }
inline std::string to_text(const char* value) { return value == nullptr ? "<null>" : value; }

}  // namespace fotest

#define FOTEST_DETAIL_CAT_(a, b) a##b
#define FOTEST_DETAIL_CAT(a, b) FOTEST_DETAIL_CAT_(a, b)

#define FOTEST(suite_name, test_name)                                            \
  static void FOTEST_DETAIL_CAT(fotest_body_, __LINE__)(fotest::Context& ctx);   \
  [[maybe_unused]] static const bool FOTEST_DETAIL_CAT(fotest_reg_, __LINE__) =  \
      fotest::register_test(suite_name, test_name,                               \
                            &FOTEST_DETAIL_CAT(fotest_body_, __LINE__));         \
  static void FOTEST_DETAIL_CAT(fotest_body_, __LINE__)(fotest::Context& ctx)

#define FO_CHECK(condition)                                                      \
  do {                                                                           \
    ++ctx.checks;                                                                \
    if (!(condition)) {                                                          \
      ctx.fail(std::string(__FILE__) + ":" + std::to_string(__LINE__) +          \
               ": CHECK failed: " #condition);                                   \
    }                                                                            \
  } while (false)

#define FO_REQUIRE(condition)                                                    \
  do {                                                                           \
    ++ctx.checks;                                                                \
    if (!(condition)) {                                                          \
      const std::string message = std::string(__FILE__) + ":" +                  \
                                  std::to_string(__LINE__) +                     \
                                  ": REQUIRE failed: " #condition;               \
      ctx.fail(message);                                                         \
      throw fotest::TestAbort{message};                                          \
    }                                                                            \
  } while (false)

// The operands are captured by value, not by reference: `std::min(a, b)` and
// friends return a reference to one of their arguments, so binding a reference
// to the result of such an expression can dangle as soon as the full expression
// ends. AddressSanitizer caught exactly that here.
#define FO_CHECK_EQ(actual, expected)                                            \
  do {                                                                           \
    ++ctx.checks;                                                                \
    const auto fotest_actual = (actual);                                         \
    const auto fotest_expected = (expected);                                     \
    if (!(fotest_actual == fotest_expected)) {                                   \
      ctx.fail(std::string(__FILE__) + ":" + std::to_string(__LINE__) +          \
               ": CHECK_EQ failed: " #actual " == " #expected " (got " +         \
               fotest::to_text(fotest_actual) + ", expected " +                  \
               fotest::to_text(fotest_expected) + ")");                          \
    }                                                                            \
  } while (false)

#define FO_CHECK_NE(actual, unexpected)                                          \
  do {                                                                           \
    ++ctx.checks;                                                                \
    const auto fotest_actual = (actual);                                         \
    const auto fotest_unexpected = (unexpected);                                 \
    if (fotest_actual == fotest_unexpected) {                                    \
      ctx.fail(std::string(__FILE__) + ":" + std::to_string(__LINE__) +          \
               ": CHECK_NE failed: " #actual " != " #unexpected);                \
    }                                                                            \
  } while (false)

#endif  // FLOWOBS_TESTS_FOTEST_HPP
