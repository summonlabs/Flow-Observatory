// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowobs/checksum.hpp"

#include <array>

namespace flowobs {

namespace {

// CRC-32C table, generated at compile time from the reflected polynomial.
struct Crc32cTable final {
  std::array<std::uint32_t, 256> entries{};

  constexpr Crc32cTable() noexcept : entries() {
    constexpr std::uint32_t kPolynomial = 0x82F63B78u;  // reflected 0x1EDC6F41
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t crc = i;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1u) != 0u ? (crc >> 1u) ^ kPolynomial : (crc >> 1u);
      }
      entries[i] = crc;
    }
  }
};

constexpr Crc32cTable kCrc32cTable{};

constexpr std::uint64_t kFnvOffset = 0xCBF29CE484222325ull;
constexpr std::uint64_t kFnvPrime = 0x00000100000001B3ull;

constexpr std::uint64_t fnv_byte(std::uint64_t state, unsigned char byte) noexcept {
  return (state ^ static_cast<std::uint64_t>(byte)) * kFnvPrime;
}

}  // namespace

std::uint32_t crc32c(const void* data, std::size_t size) noexcept {
  return crc32c(std::string_view(static_cast<const char*>(data), size));
}

std::uint32_t crc32c(std::string_view text) noexcept {
  Crc32c crc;
  crc.update(text);
  return crc.value();
}

void Crc32c::update(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::uint32_t state = state_;
  for (std::size_t i = 0; i < size; ++i) {
    state = kCrc32cTable.entries[(state ^ bytes[i]) & 0xFFu] ^ (state >> 8u);
  }
  state_ = state;
}

std::uint32_t Crc32c::value() const noexcept { return state_ ^ 0xFFFFFFFFu; }

std::uint64_t fnv1a64_raw(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::uint64_t state = kFnvOffset;
  for (std::size_t i = 0; i < size; ++i) {
    state = fnv_byte(state, bytes[i]);
  }
  return state;
}

std::uint64_t fnv1a64(std::string_view domain, std::string_view payload) noexcept {
  std::uint64_t state = kFnvOffset;
  for (char c : domain) {
    state = fnv_byte(state, static_cast<unsigned char>(c));
  }
  state = fnv_byte(state, 0x00u);
  for (char c : payload) {
    state = fnv_byte(state, static_cast<unsigned char>(c));
  }
  return state;
}

}  // namespace flowobs
