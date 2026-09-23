// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowobs/identity.hpp"

#include <string>

#include "flowobs/checksum.hpp"

namespace flowobs {

namespace {
constexpr char kHexDigits[] = "0123456789abcdef";

constexpr int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
}  // namespace

void append_hex(std::string& out, std::uint64_t value) {
  if (value == 0) {
    out.push_back('0');
    return;
  }
  char buffer[16];
  std::size_t n = 0;
  while (value != 0 && n < 16) {
    buffer[n++] = kHexDigits[value & 0xFu];
    value >>= 4u;
  }
  while (n > 0) {
    out.push_back(buffer[--n]);
  }
}

void append_hex16(std::string& out, std::uint64_t value) {
  for (int shift = 60; shift >= 0; shift -= 4) {
    out.push_back(kHexDigits[(value >> static_cast<unsigned>(shift)) & 0xFu]);
  }
}

bool parse_hex16(std::string_view text, std::uint64_t& out) noexcept {
  if (text.size() != 16) {
    return false;
  }
  std::uint64_t value = 0;
  for (char c : text) {
    const int digit = hex_value(c);
    if (digit < 0) {
      return false;
    }
    value = (value << 4u) | static_cast<std::uint64_t>(digit);
  }
  out = value;
  return true;
}

std::uint64_t identity_hash(std::string_view domain, std::string_view key) noexcept {
  const std::uint64_t hashed = fnv1a64(domain, key);
  // Zero is reserved for "invalid". Mapping it to one keeps the invariant that
  // a successfully constructed identity is always valid.
  return hashed == 0 ? 1u : hashed;
}

std::string to_string(FlowId id) { return id_to_string<FlowTag>(id); }
std::string to_string(EndpointId id) { return id_to_string<EndpointTag>(id); }
std::string to_string(PathId id) { return id_to_string<PathTag>(id); }
std::string to_string(LinkId id) { return id_to_string<LinkTag>(id); }
std::string to_string(QueueId id) { return id_to_string<QueueTag>(id); }
std::string to_string(SourceId id) { return id_to_string<SourceTag>(id); }
std::string to_string(EpochId id) { return id_to_string<EpochTag>(id); }
std::string to_string(IncarnationId id) { return id_to_string<IncarnationTag>(id); }
std::string to_string(GenerationId id) { return id_to_string<GenerationTag>(id); }
std::string to_string(RevisionId id) { return id_to_string<RevisionTag>(id); }
std::string to_string(SequenceNo id) { return id_to_string<SequenceTag>(id); }

std::string FlowGeneration::to_string() const {
  std::string out;
  out.reserve(48);
  out.append(flowobs::to_string(flow_));
  out.push_back('/');
  out.append(flowobs::to_string(generation_));
  return out;
}

}  // namespace flowobs
