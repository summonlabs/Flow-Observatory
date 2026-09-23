// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Integrity primitives. CRC-32C (Castagnoli) protects every persisted record
// and every wire frame; FNV-1a 64 provides cheap in-memory digests. Both are
// fully specified and test-vectored so an independent implementation agrees.

#ifndef FLOWOBS_CHECKSUM_HPP
#define FLOWOBS_CHECKSUM_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "flowobs/export.hpp"

namespace flowobs {

// CRC-32C, reflected, polynomial 0x1EDC6F41, initial value 0xFFFFFFFF, final
// xor 0xFFFFFFFF. crc32c("123456789") == 0xE3069283.
FLOWOBS_PUBLIC std::uint32_t crc32c(const void* data, std::size_t size) noexcept;
FLOWOBS_PUBLIC std::uint32_t crc32c(std::string_view text) noexcept;

// Incremental CRC-32C so a streaming reader never has to buffer a whole record.
class FLOWOBS_PUBLIC Crc32c final {
 public:
  Crc32c() noexcept = default;
  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }
  [[nodiscard]] std::uint32_t value() const noexcept;
  void reset() noexcept { state_ = 0xFFFFFFFFu; }

 private:
  std::uint32_t state_ = 0xFFFFFFFFu;
};

// FNV-1a 64. Domain separated: the domain bytes, a 0x00 separator, then the
// payload. Returns 0 only for an empty domain and empty payload; callers that
// need a non-zero result map 0 to 1 explicitly (see identity_hash).
FLOWOBS_PUBLIC std::uint64_t fnv1a64(std::string_view domain,
                                     std::string_view payload) noexcept;

// FNV-1a 64 over raw bytes with no domain separator.
FLOWOBS_PUBLIC std::uint64_t fnv1a64_raw(const void* data, std::size_t size) noexcept;

}  // namespace flowobs

#endif  // FLOWOBS_CHECKSUM_HPP
