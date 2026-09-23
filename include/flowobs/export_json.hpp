// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Deterministic JSON writer. Key order is fixed by the emitting code, integers
// are emitted exactly (no floating point ever appears in a deterministic
// output), and no locale-dependent formatting is used.

#ifndef FLOWOBS_EXPORT_JSON_HPP
#define FLOWOBS_EXPORT_JSON_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "flowobs/export.hpp"

namespace flowobs {
namespace json {

class FLOWOBS_PUBLIC Writer final {
 public:
  explicit Writer(bool pretty) : pretty_(pretty) {}

  void begin_object();
  void end_object();
  void begin_array();
  void end_array();

  void key(std::string_view name);
  void value_string(std::string_view value);
  void value_uint(std::uint64_t value);
  void value_int(std::int64_t value);
  void value_bool(bool value);
  void value_null();
  // Exact fixed-point rendering of numerator/denominator with `scale` decimal
  // places, using integer arithmetic only. Emitted as a JSON string so that no
  // consumer can lose precision.
  void value_ratio(std::uint64_t numerator, std::uint64_t denominator, unsigned scale);

  void field(std::string_view name, std::string_view value);
  void field(std::string_view name, const char* value) {
    key(name);
    value_string(std::string_view(value));
  }
  // Arithmetic overload: resolves bool, signed and unsigned without an
  // ambiguity between the 64-bit overloads, so a caller can pass a std::uint32_t
  // or a std::int32_t field directly.
  template <class T, class = std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>>>
  void field(std::string_view name, T value) {
    key(name);
    if constexpr (std::is_unsigned_v<T>) {
      value_uint(static_cast<std::uint64_t>(value));
    } else {
      value_int(static_cast<std::int64_t>(value));
    }
  }
  void field(std::string_view name, bool value) {
    key(name);
    value_bool(value);
  }
  void field_null(std::string_view name);
  void field_ratio(std::string_view name, std::uint64_t numerator,
                   std::uint64_t denominator, unsigned scale);

  [[nodiscard]] const std::string& str() const noexcept { return out_; }
  [[nodiscard]] std::string take() { return std::move(out_); }

 private:
  void indent();
  void separate();

  std::string out_;
  bool pretty_ = true;
  int depth_ = 0;
  bool need_comma_ = false;
  // One entry per open container: true when it already holds a value, so that
  // an empty object renders as {} and not as a newline.
  std::vector<bool> stack_;
};

// Escapes a string into valid JSON with a bounded output size.
FLOWOBS_PUBLIC std::string escape(std::string_view input);

}  // namespace json
}  // namespace flowobs

#endif  // FLOWOBS_EXPORT_JSON_HPP
