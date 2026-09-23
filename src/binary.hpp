// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Internal little-endian binary writer/reader. Every multi-byte value is
// encoded byte by byte so that the on-disk and on-wire formats do not depend on
// the host endianness, on structure padding, or on compiler layout decisions.

#ifndef FLOWOBS_SRC_BINARY_HPP
#define FLOWOBS_SRC_BINARY_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flowobs {
namespace detail {

class Writer final {
 public:
  void u8(std::uint8_t value) { buf_.push_back(static_cast<std::byte>(value)); }
  void boolean(bool value) { u8(value ? 1u : 0u); }

  void u16(std::uint16_t value) {
    u8(static_cast<std::uint8_t>(value & 0xFFu));
    u8(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
  }
  void u32(std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  }
  void u64(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  }
  void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

  void blob(const void* data, std::size_t size) {
    u32(static_cast<std::uint32_t>(size));
    const auto* bytes = static_cast<const std::byte*>(data);
    buf_.insert(buf_.end(), bytes, bytes + size);
  }
  void str(std::string_view text) { blob(text.data(), text.size()); }

  void optional_u64(const std::optional<std::uint64_t>& value) {
    boolean(value.has_value());
    if (value.has_value()) {
      u64(*value);
    }
  }
  void optional_i64(const std::optional<std::int64_t>& value) {
    boolean(value.has_value());
    if (value.has_value()) {
      i64(*value);
    }
  }
  void optional_u32(const std::optional<std::uint32_t>& value) {
    boolean(value.has_value());
    if (value.has_value()) {
      u32(*value);
    }
  }
  void optional_bool(const std::optional<bool>& value) {
    boolean(value.has_value());
    if (value.has_value()) {
      boolean(*value);
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }
  [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return buf_; }
  [[nodiscard]] std::vector<std::byte> take() { return std::move(buf_); }

 private:
  std::vector<std::byte> buf_;
};

class Reader final {
 public:
  Reader(const std::byte* data, std::size_t size) noexcept : data_(data), size_(size) {}
  explicit Reader(const std::vector<std::byte>& bytes) noexcept
      : data_(bytes.data()), size_(bytes.size()) {}

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return ok_ ? size_ - pos_ : 0; }
  [[nodiscard]] std::size_t position() const noexcept { return pos_; }

  bool u8(std::uint8_t& out) noexcept {
    if (!ok_ || remaining() < 1) {
      ok_ = false;
      return false;
    }
    out = std::to_integer<std::uint8_t>(data_[pos_++]);
    return true;
  }
  bool boolean(bool& out) noexcept {
    std::uint8_t raw = 0;
    if (!u8(raw)) return false;
    if (raw > 1) {
      ok_ = false;
      return false;
    }
    out = raw == 1;
    return true;
  }
  bool u16(std::uint16_t& out) noexcept {
    std::uint8_t a = 0;
    std::uint8_t b = 0;
    if (!u8(a) || !u8(b)) return false;
    out = static_cast<std::uint16_t>(static_cast<std::uint16_t>(a) |
                                     static_cast<std::uint16_t>(static_cast<std::uint16_t>(b) << 8u));
    return true;
  }
  bool u32(std::uint32_t& out) noexcept {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
      std::uint8_t byte = 0;
      if (!u8(byte)) return false;
      value |= static_cast<std::uint32_t>(byte) << shift;
    }
    out = value;
    return true;
  }
  bool u64(std::uint64_t& out) noexcept {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
      std::uint8_t byte = 0;
      if (!u8(byte)) return false;
      value |= static_cast<std::uint64_t>(byte) << shift;
    }
    out = value;
    return true;
  }
  bool i64(std::int64_t& out) noexcept {
    std::uint64_t raw = 0;
    if (!u64(raw)) return false;
    out = static_cast<std::int64_t>(raw);
    return true;
  }
  // Reads a length-prefixed blob. `max_size` is an absolute bound that is
  // checked before any allocation happens.
  bool blob(std::string& out, std::size_t max_size) noexcept {
    std::uint32_t length = 0;
    if (!u32(length)) return false;
    if (length > max_size || remaining() < length) {
      ok_ = false;
      return false;
    }
    out.assign(reinterpret_cast<const char*>(data_ + pos_), length);
    pos_ += length;
    return true;
  }
  bool optional_u64(std::optional<std::uint64_t>& out) noexcept {
    bool present = false;
    if (!boolean(present)) return false;
    if (!present) {
      out.reset();
      return true;
    }
    std::uint64_t value = 0;
    if (!u64(value)) return false;
    out = value;
    return true;
  }
  bool optional_i64(std::optional<std::int64_t>& out) noexcept {
    bool present = false;
    if (!boolean(present)) return false;
    if (!present) {
      out.reset();
      return true;
    }
    std::int64_t value = 0;
    if (!i64(value)) return false;
    out = value;
    return true;
  }
  bool optional_u32(std::optional<std::uint32_t>& out) noexcept {
    bool present = false;
    if (!boolean(present)) return false;
    if (!present) {
      out.reset();
      return true;
    }
    std::uint32_t value = 0;
    if (!u32(value)) return false;
    out = value;
    return true;
  }
  bool optional_bool(std::optional<bool>& out) noexcept {
    bool present = false;
    if (!boolean(present)) return false;
    if (!present) {
      out.reset();
      return true;
    }
    bool value = false;
    if (!boolean(value)) return false;
    out = value;
    return true;
  }
  // Consumes the remainder and fails when bytes are left over.
  bool finish() noexcept {
    if (!ok_) return false;
    if (remaining() != 0) {
      ok_ = false;
      return false;
    }
    return true;
  }

 private:
  const std::byte* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

}  // namespace detail
}  // namespace flowobs

#endif  // FLOWOBS_SRC_BINARY_HPP
