// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowobs/observation.hpp"

#include <string>

#include "flowobs/checksum.hpp"

namespace flowobs {

namespace {

void append_optional_u64(std::string& out, const char* name,
                         const std::optional<std::uint64_t>& value) {
  out.push_back(' ');
  out.append(name);
  out.push_back('=');
  if (value.has_value()) {
    out.append(std::to_string(*value));
  } else {
    out.push_back('-');
  }
}

void append_optional_duration(std::string& out, const char* name,
                              const std::optional<Duration>& value) {
  out.push_back(' ');
  out.append(name);
  out.push_back('=');
  if (value.has_value()) {
    out.append(std::to_string(value->nanos()));
  } else {
    out.push_back('-');
  }
}

}  // namespace

std::string ObservationKey::describe() const {
  std::string out;
  out.reserve(96);
  out.append(flowobs::to_string(source));
  out.append("/inc:");
  out.append(std::to_string(incarnation.value()));
  out.append("/epoch:");
  out.append(std::to_string(epoch.value()));
  out.append("/rev:");
  out.append(std::to_string(revision.value()));
  out.append("/seq:");
  out.append(std::to_string(sequence.value()));
  return out;
}

std::string SourceCapabilities::describe() const {
  std::string out;
  out.reserve(96);
  bool first = true;
  const auto add = [&out, &first](const char* name) {
    if (!first) out.push_back(',');
    first = false;
    out.append(name);
  };
  if (asserts_completion) add("completion");
  if (asserts_reset) add("reset");
  if (asserts_path) add("path");
  if (asserts_queue) add("queue");
  if (asserts_counters) add("counters");
  if (authoritative_identity) add("identity");
  if (first) out.append("none");
  return out;
}

std::string Observation::canonical_form(bool include_receive_time) const {
  std::string out;
  out.reserve(320);
  out.append("obs v=");
  out.append(std::to_string(schema_version));
  out.append(" src=");
  out.append(flowobs::to_string(key.source));
  out.append(" inc=");
  out.append(std::to_string(key.incarnation.value()));
  out.append(" epoch=");
  out.append(std::to_string(key.epoch.value()));
  out.append(" rev=");
  out.append(std::to_string(key.revision.value()));
  out.append(" seq=");
  out.append(std::to_string(key.sequence.value()));
  out.append(" flow=");
  out.append(flowobs::to_string(subject.flow));
  out.append(" gen=");
  out.append(std::to_string(subject.generation.value()));
  out.append(" ep=");
  out.append(flowobs::to_string(subject.endpoint));
  out.append(" peer=");
  out.append(flowobs::to_string(subject.peer_endpoint));
  out.append(" path=");
  out.append(subject.path.has_value() ? flowobs::to_string(*subject.path) : std::string("-"));
  out.append(" queue=");
  out.append(subject.queue.has_value() ? flowobs::to_string(*subject.queue) : std::string("-"));
  out.append(" link=");
  out.append(subject.link.has_value() ? flowobs::to_string(*subject.link) : std::string("-"));
  out.append(" pathgen=");
  out.append(subject.path_generation.has_value()
                 ? std::to_string(subject.path_generation->value())
                 : std::string("-"));
  out.append(" hopcap=");
  if (subject.hop_capacity_units.empty()) {
    out.push_back('-');
  } else {
    for (std::size_t i = 0; i < subject.hop_capacity_units.size(); ++i) {
      if (i != 0) out.push_back(',');
      out.append(std::to_string(subject.hop_capacity_units[i]));
    }
  }
  out.append(" kind=");
  out.append(flowobs::to_string(kind));
  out.append(" dir=");
  out.append(flowobs::to_string(direction));
  out.append(" ev=");
  out.append(flowobs::to_string(evidence));
  out.append(" obs_at=");
  out.append(std::to_string(observed_at.nanos()));
  if (include_receive_time) {
    out.append(" rx_at=");
    out.append(std::to_string(received_at.nanos()));
  }
  append_optional_u64(out, "bytes", counters.bytes);
  append_optional_u64(out, "packets", counters.packets);
  append_optional_u64(out, "retx", counters.retransmissions);
  append_optional_duration(out, "dur", covered_duration);
  append_optional_duration(out, "rtt", reported_rtt);
  out.append(" caps=");
  out.append(capabilities.describe());
  return out;
}

std::uint64_t Observation::digest() const noexcept {
  const std::string text = canonical_form(true);
  return fnv1a64_raw(text.data(), text.size());
}

std::uint64_t Observation::evidence_digest() const noexcept {
  const std::string text = canonical_form(false);
  return fnv1a64_raw(text.data(), text.size());
}

std::uint64_t content_digest(const Observation& obs) noexcept {
  // Evidence identity is what the source said, not when this runtime happened
  // to receive it: a redelivery must be recognised as a redelivery even though
  // its receive instant differs, and it must not be able to refresh the age of
  // the evidence either.
  return obs.evidence_digest();
}

bool same_content(const Observation& a, const Observation& b) noexcept {
  return content_digest(a) == content_digest(b);
}

std::string Observation::describe() const {
  std::string out;
  out.reserve(256);
  out.append("key{");
  out.append(key.describe());
  out.append("} ");
  out.append(flowobs::to_string(subject.flow));
  out.push_back('/');
  out.append(flowobs::to_string(subject.generation));
  out.append(" kind=");
  out.append(flowobs::to_string(kind));
  out.append(" dir=");
  out.append(flowobs::to_string(direction));
  out.append(" ev=");
  out.append(flowobs::to_string(evidence));
  out.append(" obs_at=");
  out.append(std::to_string(observed_at.nanos()));
  out.append(" counters");
  out.append(counters.describe());
  return out;
}

}  // namespace flowobs
