// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "obs_codec.hpp"

namespace flowobs {

FlowId flow_id_from_key(std::string_view key) noexcept { return make_id<FlowTag>(key); }
EndpointId endpoint_id_from_key(std::string_view key) noexcept {
  return make_id<EndpointTag>(key);
}
PathId path_id_from_key(std::string_view key) noexcept { return make_id<PathTag>(key); }
LinkId link_id_from_key(std::string_view key) noexcept { return make_id<LinkTag>(key); }
QueueId queue_id_from_key(std::string_view key) noexcept { return make_id<QueueTag>(key); }
SourceId source_id_from_key(std::string_view key) noexcept { return make_id<SourceTag>(key); }

bool key_matches(FlowId id, std::string_view key) noexcept {
  return id.valid() && !key.empty() && make_id<FlowTag>(key) == id;
}
bool key_matches(EndpointId id, std::string_view key) noexcept {
  return id.valid() && !key.empty() && make_id<EndpointTag>(key) == id;
}
bool key_matches(PathId id, std::string_view key) noexcept {
  return id.valid() && !key.empty() && make_id<PathTag>(key) == id;
}
bool key_matches(LinkId id, std::string_view key) noexcept {
  return id.valid() && !key.empty() && make_id<LinkTag>(key) == id;
}
bool key_matches(QueueId id, std::string_view key) noexcept {
  return id.valid() && !key.empty() && make_id<QueueTag>(key) == id;
}
bool key_matches(SourceId id, std::string_view key) noexcept {
  return id.valid() && !key.empty() && make_id<SourceTag>(key) == id;
}

namespace detail {

namespace {

void write_capabilities(Writer& writer, const SourceCapabilities& capabilities) {
  writer.boolean(capabilities.asserts_completion);
  writer.boolean(capabilities.asserts_reset);
  writer.boolean(capabilities.asserts_path);
  writer.boolean(capabilities.asserts_queue);
  writer.boolean(capabilities.asserts_counters);
  writer.boolean(capabilities.authoritative_identity);
}

bool read_capabilities(Reader& reader, SourceCapabilities& out) {
  if (!reader.boolean(out.asserts_completion)) return false;
  if (!reader.boolean(out.asserts_reset)) return false;
  if (!reader.boolean(out.asserts_path)) return false;
  if (!reader.boolean(out.asserts_queue)) return false;
  if (!reader.boolean(out.asserts_counters)) return false;
  if (!reader.boolean(out.authoritative_identity)) return false;
  return true;
}

void write_optional_id(Writer& writer, std::uint64_t value, bool present) {
  writer.boolean(present);
  if (present) {
    writer.u64(value);
  }
}

bool read_optional_id(Reader& reader, std::uint64_t& value, bool& present) {
  if (!reader.boolean(present)) return false;
  if (!present) {
    value = 0;
    return true;
  }
  return reader.u64(value);
}

}  // namespace

void encode_observation(Writer& writer, const Observation& observation) {
  writer.u32(observation.schema_version);

  writer.u64(observation.key.source.value());
  writer.u64(observation.key.incarnation.value());
  writer.u64(observation.key.epoch.value());
  writer.u64(observation.key.revision.value());
  writer.u64(observation.key.sequence.value());
  write_capabilities(writer, observation.capabilities);

  writer.u64(observation.subject.flow.value());
  writer.u64(observation.subject.generation.value());
  writer.u64(observation.subject.endpoint.value());
  writer.u64(observation.subject.peer_endpoint.value());
  write_optional_id(writer, observation.subject.path.has_value()
                                ? observation.subject.path->value()
                                : 0,
                    observation.subject.path.has_value());
  write_optional_id(writer, observation.subject.queue.has_value()
                                ? observation.subject.queue->value()
                                : 0,
                    observation.subject.queue.has_value());
  write_optional_id(writer, observation.subject.link.has_value()
                                ? observation.subject.link->value()
                                : 0,
                    observation.subject.link.has_value());
  write_optional_id(writer, observation.subject.path_generation.has_value()
                                ? observation.subject.path_generation->value()
                                : 0,
                    observation.subject.path_generation.has_value());

  writer.u32(static_cast<std::uint32_t>(observation.subject.hop_capacity_units.size()));
  for (const std::uint64_t capacity : observation.subject.hop_capacity_units) {
    writer.u64(capacity);
  }
  writer.str(observation.subject.flow_key);
  writer.str(observation.subject.endpoint_key);
  writer.str(observation.subject.peer_endpoint_key);
  writer.str(observation.subject.path_key);
  writer.str(observation.subject.queue_key);
  writer.str(observation.subject.link_key);

  writer.i64(observation.observed_at.nanos());
  writer.i64(observation.received_at.nanos());

  writer.u8(static_cast<std::uint8_t>(observation.kind));
  writer.u8(static_cast<std::uint8_t>(observation.direction));
  writer.u8(static_cast<std::uint8_t>(observation.evidence));
  writer.optional_u64(observation.counters.bytes);
  writer.optional_u64(observation.counters.packets);
  writer.optional_u64(observation.counters.retransmissions);
  writer.optional_i64(observation.covered_duration.has_value()
                          ? std::optional<std::int64_t>(observation.covered_duration->nanos())
                          : std::nullopt);
  writer.optional_i64(observation.reported_rtt.has_value()
                          ? std::optional<std::int64_t>(observation.reported_rtt->nanos())
                          : std::nullopt);
}

bool decode_observation(Reader& reader, const Limits& limits, Observation& out) {
  if (!reader.u32(out.schema_version)) return false;
  std::uint64_t source = 0;
  std::uint64_t incarnation = 0;
  std::uint64_t epoch = 0;
  std::uint64_t revision = 0;
  std::uint64_t sequence = 0;
  if (!reader.u64(source)) return false;
  if (!reader.u64(incarnation)) return false;
  if (!reader.u64(epoch)) return false;
  if (!reader.u64(revision)) return false;
  if (!reader.u64(sequence)) return false;
  out.key.source = SourceId::from_value(source);
  out.key.incarnation = IncarnationId::from_value(incarnation);
  out.key.epoch = EpochId::from_value(epoch);
  out.key.revision = RevisionId::from_value(revision);
  out.key.sequence = SequenceNo::from_value(sequence);
  if (!read_capabilities(reader, out.capabilities)) return false;

  std::uint64_t flow = 0;
  std::uint64_t generation = 0;
  std::uint64_t endpoint = 0;
  std::uint64_t peer = 0;
  if (!reader.u64(flow)) return false;
  if (!reader.u64(generation)) return false;
  if (!reader.u64(endpoint)) return false;
  if (!reader.u64(peer)) return false;
  out.subject.flow = FlowId::from_value(flow);
  out.subject.generation = GenerationId::from_value(generation);
  out.subject.endpoint = EndpointId::from_value(endpoint);
  out.subject.peer_endpoint = EndpointId::from_value(peer);

  std::uint64_t path = 0;
  std::uint64_t queue = 0;
  std::uint64_t link = 0;
  std::uint64_t path_generation = 0;
  bool has_path = false;
  bool has_queue = false;
  bool has_link = false;
  bool has_path_generation = false;
  if (!read_optional_id(reader, path, has_path)) return false;
  if (!read_optional_id(reader, queue, has_queue)) return false;
  if (!read_optional_id(reader, link, has_link)) return false;
  if (!read_optional_id(reader, path_generation, has_path_generation)) return false;
  out.subject.path = has_path ? std::optional<PathId>(PathId::from_value(path)) : std::nullopt;
  out.subject.queue =
      has_queue ? std::optional<QueueId>(QueueId::from_value(queue)) : std::nullopt;
  out.subject.link = has_link ? std::optional<LinkId>(LinkId::from_value(link)) : std::nullopt;
  out.subject.path_generation =
      has_path_generation
          ? std::optional<GenerationId>(GenerationId::from_value(path_generation))
          : std::nullopt;

  std::uint32_t hop_count = 0;
  if (!reader.u32(hop_count)) return false;
  if (hop_count > limits.max_path_hops) return false;
  out.subject.hop_capacity_units.clear();
  out.subject.hop_capacity_units.reserve(hop_count);
  for (std::uint32_t i = 0; i < hop_count; ++i) {
    std::uint64_t capacity = 0;
    if (!reader.u64(capacity)) return false;
    out.subject.hop_capacity_units.push_back(capacity);
  }

  if (!reader.blob(out.subject.flow_key, limits.max_key_bytes)) return false;
  if (!reader.blob(out.subject.endpoint_key, limits.max_key_bytes)) return false;
  if (!reader.blob(out.subject.peer_endpoint_key, limits.max_key_bytes)) return false;
  if (!reader.blob(out.subject.path_key, limits.max_key_bytes)) return false;
  if (!reader.blob(out.subject.queue_key, limits.max_key_bytes)) return false;
  if (!reader.blob(out.subject.link_key, limits.max_key_bytes)) return false;

  std::int64_t observed = 0;
  std::int64_t received = 0;
  if (!reader.i64(observed)) return false;
  if (!reader.i64(received)) return false;
  out.observed_at = Timestamp::from_nanos(observed);
  out.received_at = Timestamp::from_nanos(received);

  std::uint8_t kind = 0;
  std::uint8_t direction = 0;
  std::uint8_t evidence = 0;
  if (!reader.u8(kind)) return false;
  if (!reader.u8(direction)) return false;
  if (!reader.u8(evidence)) return false;
  if (kind >= kObservationKindCount || direction >= kDirectionCount ||
      evidence >= kEvidenceCount) {
    return false;
  }
  out.kind = static_cast<ObservationKind>(kind);
  out.direction = static_cast<Direction>(direction);
  out.evidence = static_cast<Evidence>(evidence);

  if (!reader.optional_u64(out.counters.bytes)) return false;
  if (!reader.optional_u64(out.counters.packets)) return false;
  if (!reader.optional_u64(out.counters.retransmissions)) return false;
  std::optional<std::int64_t> duration;
  std::optional<std::int64_t> rtt;
  if (!reader.optional_i64(duration)) return false;
  if (!reader.optional_i64(rtt)) return false;
  if (duration.has_value()) {
    out.covered_duration = Duration::from_nanos(*duration);
  } else {
    out.covered_duration.reset();
  }
  if (rtt.has_value()) {
    out.reported_rtt = Duration::from_nanos(*rtt);
  } else {
    out.reported_rtt.reset();
  }
  return true;
}

}  // namespace detail
}  // namespace flowobs
