// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FLOWOBS_VERSION_HPP
#define FLOWOBS_VERSION_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "flowobs/export.hpp"

namespace flowobs {

// Semantic version of the Flow Observatory runtime.
inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;

// Packed (major << 16 | minor << 8 | patch) for cheap comparisons.
inline constexpr std::uint32_t kVersionPacked =
    (kVersionMajor << 16) | (kVersionMinor << 8) | kVersionPatch;

// Binary interface revision of the public C++ surface. Bumped only on
// source-incompatible changes; consumers should compare with
// flowobs::compatible_with().
inline constexpr std::uint32_t kApiVersion = 1;

// On-disk journal format version. A journal whose recorded version differs
// from this value is refused unless the reader is explicitly told to accept
// older revisions.
inline constexpr std::uint16_t kJournalFormatVersion = 1;

// Minimum journal format version this build can still read.
inline constexpr std::uint16_t kJournalMinReadableVersion = 1;

// Wire protocol version used by the loopback ingest transport.
inline constexpr std::uint16_t kWireProtocolVersion = 1;

// Canonical text observation format version (the "FO1" line format).
inline constexpr std::uint16_t kTextObservationVersion = 1;

// Observation schema version understood by this runtime.
inline constexpr std::uint32_t kObservationSchemaVersion = 1;

FLOWOBS_PUBLIC std::string_view version_string() noexcept;

// True when a component compiled against api_version can interoperate with
// this runtime.
FLOWOBS_PUBLIC bool compatible_with(std::uint32_t api_version) noexcept;

// Multi-line human readable build description. Deterministic: it never
// contains timestamps, host names or paths.
FLOWOBS_PUBLIC std::string describe_build();

}  // namespace flowobs

#endif  // FLOWOBS_VERSION_HPP
