// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowobs/version.hpp"

#include "flowobs/identity.hpp"

#include <string>

namespace flowobs {

std::string_view version_string() noexcept { return "1.0.0"; }

bool compatible_with(std::uint32_t api_version) noexcept { return api_version == kApiVersion; }

std::string describe_build() {
  std::string out;
  out.reserve(256);
  out += "flow-observatory ";
  out += version_string();
  out += "\napi_version=";
  out += std::to_string(kApiVersion);
  out += "\njournal_format=";
  out += std::to_string(kJournalFormatVersion);
  out += "\nidentity_scheme=";
  out += std::to_string(kIdentitySchemeVersion);
  out += "\nwire_protocol=";
  out += std::to_string(kWireProtocolVersion);
  out += "\ntext_format=FO1/";
  out += std::to_string(kTextObservationVersion);
  out += "\nobservation_schema=";
  out += std::to_string(kObservationSchemaVersion);
#if defined(__cplusplus) && __cplusplus >= 202002L
  out += "\ncxx_standard=20";
#else
  out += "\ncxx_standard=unknown";
#endif
  out += "\n";
  return out;
}

}  // namespace flowobs
