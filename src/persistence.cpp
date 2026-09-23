// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowobs/persistence.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>

#include "binary.hpp"
#include "flowobs/checksum.hpp"
#include "obs_codec.hpp"

#if defined(_WIN32)
#include <io.h>
#include <share.h>
#else
#include <sys/file.h>
#include <unistd.h>
#endif

namespace flowobs {

namespace {

constexpr char kMagic[8] = {'F', 'O', 'B', 'S', 'J', 'N', 'L', '1'};
constexpr std::size_t kHeaderBytes = 64;
constexpr std::size_t kRecordHeaderBytes = 12;

void put_u16(std::byte* out, std::uint16_t value) {
  out[0] = static_cast<std::byte>(value & 0xFFu);
  out[1] = static_cast<std::byte>((value >> 8u) & 0xFFu);
}
void put_u32(std::byte* out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out[shift / 8u] = static_cast<std::byte>((value >> shift) & 0xFFu);
  }
}
void put_u64(std::byte* out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out[shift / 8u] = static_cast<std::byte>((value >> shift) & 0xFFu);
  }
}
std::uint16_t get_u16(const std::byte* in) {
  return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(in[0]) |
                                    (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(in[1])) << 8u));
}
std::uint32_t get_u32(const std::byte* in) {
  std::uint32_t value = 0;
  for (unsigned shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[shift / 8u])) << shift;
  }
  return value;
}
std::uint64_t get_u64(const std::byte* in) {
  std::uint64_t value = 0;
  for (unsigned shift = 0; shift < 64; shift += 8) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(in[shift / 8u])) << shift;
  }
  return value;
}

Status io_error(const std::string& what) {
  return Status(ErrorCode::IoError, what);
}

// Opens the journal file, denying other writers when the caller intends to
// write. Two runtimes appending to one journal would interleave records and
// corrupt it, so the second writer is refused with a clear error instead.
// Readers are still allowed: an observer watching a live journal sees a possibly
// torn final record, which the reader already treats as a torn tail.
//
// The exclusion is enforced by the operating system, so it is released even if a
// process dies without cleaning up.
std::FILE* open_journal_file(const std::string& path, const char* mode, bool exclusive) {
#if defined(_WIN32)
  return _fsopen(path.c_str(), mode, exclusive ? _SH_DENYWR : _SH_DENYNO);
#else
  std::FILE* file = std::fopen(path.c_str(), mode);
  if (file != nullptr && exclusive) {
    if (::flock(::fileno(file), LOCK_EX | LOCK_NB) != 0) {
      std::fclose(file);
      return nullptr;
    }
  }
  return file;
#endif
}

void sync_file(std::FILE* file) {
  if (file == nullptr) {
    return;
  }
  std::fflush(file);
#if defined(_WIN32)
  const int descriptor = _fileno(file);
  if (descriptor >= 0) {
    (void)_commit(descriptor);
  }
#else
  const int descriptor = fileno(file);
  if (descriptor >= 0) {
    (void)::fsync(descriptor);
  }
#endif
}

// Flushing a stream that is not open for writing (a read-only journal, for
// instance) makes _commit() fail on an invalid descriptor on Windows, so the
// sync is explicit rather than unconditional.
void close_file(std::FILE* file, bool sync_before_close = true) {
  if (file != nullptr) {
    if (sync_before_close) {
      sync_file(file);
    }
    std::fclose(file);
  }
}

Status atomic_replace(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::error_code ec;
#if defined(_WIN32)
  // MoveFileEx with replace-existing is the only atomic rename on Windows.
  std::filesystem::remove(to, ec);
  ec.clear();
#endif
  std::filesystem::rename(from, to, ec);
  if (ec) {
    return io_error("cannot replace " + to.string() + ": " + ec.message());
  }
  return make_ok();
}

// ---------------------------------------------------------------------------
// Field codecs
// ---------------------------------------------------------------------------

void write_optional_duration(detail::Writer& writer, const std::optional<Duration>& value) {
  writer.optional_i64(value.has_value() ? std::optional<std::int64_t>(value->nanos())
                                        : std::nullopt);
}

bool read_optional_duration(detail::Reader& reader, std::optional<Duration>& out) {
  std::optional<std::int64_t> raw;
  if (!reader.optional_i64(raw)) {
    return false;
  }
  if (raw.has_value()) {
    out = Duration::from_nanos(*raw);
  } else {
    out.reset();
  }
  return true;
}

void write_accumulator(detail::Writer& writer, const CounterAccumulator& accumulator) {
  const CounterAccumulator::Snapshot snapshot = accumulator.snapshot();
  writer.optional_u64(snapshot.baseline);
  writer.optional_u64(snapshot.last);
  writer.u64(snapshot.accumulated);
  writer.u64(snapshot.unknown_intervals);
  writer.u64(snapshot.regressions);
  writer.boolean(snapshot.saturated);
}

bool read_accumulator(detail::Reader& reader, CounterAccumulator& out) {
  CounterAccumulator::Snapshot snapshot;
  if (!reader.optional_u64(snapshot.baseline)) return false;
  if (!reader.optional_u64(snapshot.last)) return false;
  if (!reader.u64(snapshot.accumulated)) return false;
  if (!reader.u64(snapshot.unknown_intervals)) return false;
  if (!reader.u64(snapshot.regressions)) return false;
  if (!reader.boolean(snapshot.saturated)) return false;
  out.restore(snapshot);
  return true;
}

void write_directional(detail::Writer& writer, const DirectionalAccounting& accounting) {
  write_accumulator(writer, accounting.bytes);
  write_accumulator(writer, accounting.packets);
  write_accumulator(writer, accounting.retransmissions);
  writer.boolean(accounting.bytes_unsupported);
  writer.boolean(accounting.packets_unsupported);
  writer.boolean(accounting.retransmissions_unsupported);
}

bool read_directional(detail::Reader& reader, DirectionalAccounting& out) {
  if (!read_accumulator(reader, out.bytes)) return false;
  if (!read_accumulator(reader, out.packets)) return false;
  if (!read_accumulator(reader, out.retransmissions)) return false;
  if (!reader.boolean(out.bytes_unsupported)) return false;
  if (!reader.boolean(out.packets_unsupported)) return false;
  if (!reader.boolean(out.retransmissions_unsupported)) return false;
  return true;
}

void write_consumption(detail::Writer& writer, const Consumption& value) {
  writer.u64(value.bytes);
  writer.u64(value.packets);
  writer.u64(value.retransmissions);
  writer.u64(value.unknown_intervals);
  writer.boolean(value.bytes_unsupported);
  writer.boolean(value.packets_unsupported);
  writer.boolean(value.retransmissions_unsupported);
  writer.boolean(value.saturated);
}

bool read_consumption(detail::Reader& reader, Consumption& out) {
  if (!reader.u64(out.bytes)) return false;
  if (!reader.u64(out.packets)) return false;
  if (!reader.u64(out.retransmissions)) return false;
  if (!reader.u64(out.unknown_intervals)) return false;
  if (!reader.boolean(out.bytes_unsupported)) return false;
  if (!reader.boolean(out.packets_unsupported)) return false;
  if (!reader.boolean(out.retransmissions_unsupported)) return false;
  if (!reader.boolean(out.saturated)) return false;
  return true;
}

void write_coverage(detail::Writer& writer, const Coverage& coverage) {
  writer.boolean(coverage.forward_complete);
  writer.boolean(coverage.forward_partial);
  writer.boolean(coverage.reverse_complete);
  writer.boolean(coverage.reverse_partial);
}

bool read_coverage(detail::Reader& reader, Coverage& out) {
  if (!reader.boolean(out.forward_complete)) return false;
  if (!reader.boolean(out.forward_partial)) return false;
  if (!reader.boolean(out.reverse_complete)) return false;
  if (!reader.boolean(out.reverse_partial)) return false;
  return true;
}

void write_capabilities(detail::Writer& writer, const SourceCapabilities& capabilities) {
  writer.boolean(capabilities.asserts_completion);
  writer.boolean(capabilities.asserts_reset);
  writer.boolean(capabilities.asserts_path);
  writer.boolean(capabilities.asserts_queue);
  writer.boolean(capabilities.asserts_counters);
  writer.boolean(capabilities.authoritative_identity);
}

bool read_capabilities(detail::Reader& reader, SourceCapabilities& out) {
  if (!reader.boolean(out.asserts_completion)) return false;
  if (!reader.boolean(out.asserts_reset)) return false;
  if (!reader.boolean(out.asserts_path)) return false;
  if (!reader.boolean(out.asserts_queue)) return false;
  if (!reader.boolean(out.asserts_counters)) return false;
  if (!reader.boolean(out.authoritative_identity)) return false;
  return true;
}

void write_source_descriptor(detail::Writer& writer, const SourceDescriptor& source) {
  writer.u64(source.id.value());
  writer.str(source.canonical_key);
  writer.u32(source.authority);
  write_capabilities(writer, source.capabilities);
  writer.boolean(source.advisory_only);
}

bool read_source_descriptor(detail::Reader& reader, SourceDescriptor& out,
                            std::size_t max_key_bytes) {
  std::uint64_t id = 0;
  if (!reader.u64(id)) return false;
  if (!reader.blob(out.canonical_key, max_key_bytes)) return false;
  if (!reader.u32(out.authority)) return false;
  if (!read_capabilities(reader, out.capabilities)) return false;
  if (!reader.boolean(out.advisory_only)) return false;
  out.id = SourceId::from_value(id);
  return true;
}

void write_persisted_source(detail::Writer& writer, const PersistedSource& source) {
  writer.u64(source.source.value());
  writer.str(source.canonical_key);
  writer.u32(source.authority);
  write_capabilities(writer, source.capabilities);
  writer.boolean(source.advisory_only);

  writer.u64(source.incarnation.value());
  writer.u64(source.epoch.value());
  writer.u64(source.high_revision.value());
  writer.u64(source.high_sequence.value());

  writer.i64(source.first_observed_at.nanos());
  writer.i64(source.last_observed_at.nanos());
  writer.i64(source.last_received_at.nanos());
  writer.i64(source.last_progress_at.nanos());

  write_coverage(writer, source.coverage);
  writer.u32(static_cast<std::uint32_t>(source.hop_capacity_units.size()));
  for (const std::uint64_t capacity : source.hop_capacity_units) {
    writer.u64(capacity);
  }
  write_directional(writer, source.forward);
  write_directional(writer, source.reverse);
  write_directional(writer, source.aggregate);
  write_consumption(writer, source.forward_totals);
  write_consumption(writer, source.reverse_totals);
  write_consumption(writer, source.aggregate_totals);
  writer.boolean(source.observed_progress);

  writer.u8(static_cast<std::uint8_t>(source.last_kind));
  writer.u8(static_cast<std::uint8_t>(source.last_direction));
  writer.u8(static_cast<std::uint8_t>(source.last_evidence));
  writer.optional_u32(source.declared_terminal.has_value()
                          ? std::optional<std::uint32_t>(
                                static_cast<std::uint32_t>(*source.declared_terminal))
                          : std::nullopt);
  writer.u8(static_cast<std::uint8_t>(source.declared_cause));

  writer.u32(source.accepted_observations);
  writer.u32(source.refused_observations);
  writer.u32(source.dropped_observations);
  writer.u32(source.duplicates_suppressed);
  writer.u32(source.content_conflicts);
  writer.u32(source.historical_incarnations);
  writer.u32(source.after_terminal_observations);
}

bool read_persisted_source(detail::Reader& reader, PersistedSource& out,
                           std::size_t max_key_bytes) {
  std::uint64_t raw_id = 0;
  if (!reader.u64(raw_id)) return false;
  out.source = SourceId::from_value(raw_id);
  if (!reader.blob(out.canonical_key, max_key_bytes)) return false;
  if (!reader.u32(out.authority)) return false;
  if (!read_capabilities(reader, out.capabilities)) return false;
  if (!reader.boolean(out.advisory_only)) return false;

  std::uint64_t incarnation = 0;
  std::uint64_t epoch = 0;
  std::uint64_t revision = 0;
  std::uint64_t sequence = 0;
  if (!reader.u64(incarnation)) return false;
  if (!reader.u64(epoch)) return false;
  if (!reader.u64(revision)) return false;
  if (!reader.u64(sequence)) return false;
  out.incarnation = IncarnationId::from_value(incarnation);
  out.epoch = EpochId::from_value(epoch);
  out.high_revision = RevisionId::from_value(revision);
  out.high_sequence = SequenceNo::from_value(sequence);

  std::int64_t first = 0;
  std::int64_t last = 0;
  std::int64_t received = 0;
  std::int64_t progress = 0;
  if (!reader.i64(first)) return false;
  if (!reader.i64(last)) return false;
  if (!reader.i64(received)) return false;
  if (!reader.i64(progress)) return false;
  out.first_observed_at = Timestamp::from_nanos(first);
  out.last_observed_at = Timestamp::from_nanos(last);
  out.last_received_at = Timestamp::from_nanos(received);
  out.last_progress_at = Timestamp::from_nanos(progress);

  if (!read_coverage(reader, out.coverage)) return false;
  std::uint32_t hop_count = 0;
  if (!reader.u32(hop_count)) return false;
  if (hop_count > 4096u) return false;
  out.hop_capacity_units.clear();
  out.hop_capacity_units.reserve(hop_count);
  for (std::uint32_t i = 0; i < hop_count; ++i) {
    std::uint64_t capacity = 0;
    if (!reader.u64(capacity)) return false;
    out.hop_capacity_units.push_back(capacity);
  }
  if (!read_directional(reader, out.forward)) return false;
  if (!read_directional(reader, out.reverse)) return false;
  if (!read_directional(reader, out.aggregate)) return false;
  if (!read_consumption(reader, out.forward_totals)) return false;
  if (!read_consumption(reader, out.reverse_totals)) return false;
  if (!read_consumption(reader, out.aggregate_totals)) return false;
  if (!reader.boolean(out.observed_progress)) return false;

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
  out.last_kind = static_cast<ObservationKind>(kind);
  out.last_direction = static_cast<Direction>(direction);
  out.last_evidence = static_cast<Evidence>(evidence);

  std::optional<std::uint32_t> terminal;
  if (!reader.optional_u32(terminal)) return false;
  if (terminal.has_value()) {
    if (*terminal >= kFlowStateCount) return false;
    out.declared_terminal = static_cast<FlowState>(*terminal);
  } else {
    out.declared_terminal.reset();
  }
  std::uint8_t cause = 0;
  if (!reader.u8(cause)) return false;
  if (cause >= kStateCauseCount) return false;
  out.declared_cause = static_cast<StateCause>(cause);

  if (!reader.u32(out.accepted_observations)) return false;
  if (!reader.u32(out.refused_observations)) return false;
  if (!reader.u32(out.dropped_observations)) return false;
  if (!reader.u32(out.duplicates_suppressed)) return false;
  if (!reader.u32(out.content_conflicts)) return false;
  if (!reader.u32(out.historical_incarnations)) return false;
  if (!reader.u32(out.after_terminal_observations)) return false;
  return true;
}

void write_persisted_generation(detail::Writer& writer, const PersistedGeneration& generation) {
  writer.u64(generation.generation.value());
  writer.boolean(generation.is_current);
  writer.i64(generation.first_observed_at.nanos());
  writer.i64(generation.last_observed_at.nanos());
  writer.i64(generation.last_received_at.nanos());
  writer.i64(generation.last_progress_at.nanos());
  writer.i64(generation.closed_at.nanos());
  writer.u8(static_cast<std::uint8_t>(generation.state));
  writer.u8(static_cast<std::uint8_t>(generation.cause));
  write_coverage(writer, generation.coverage);

  writer.optional_u64(generation.path.has_value()
                          ? std::optional<std::uint64_t>(generation.path->value())
                          : std::nullopt);
  writer.u32(static_cast<std::uint32_t>(generation.links.size()));
  for (const LinkId link : generation.links) {
    writer.u64(link.value());
  }
  writer.u32(static_cast<std::uint32_t>(generation.queues.size()));
  for (const QueueId queue : generation.queues) {
    writer.u64(queue.value());
  }
  writer.u64(generation.endpoint_a.value());
  writer.u64(generation.endpoint_b.value());

  write_consumption(writer, generation.forward);
  write_consumption(writer, generation.reverse);
  write_consumption(writer, generation.aggregate);
  write_consumption(writer, generation.total);

  writer.boolean(generation.unresolved_conflict);
  writer.str(generation.conflict_detail);

  writer.u32(static_cast<std::uint32_t>(generation.sources.size()));
  for (const PersistedSource& source : generation.sources) {
    write_persisted_source(writer, source);
  }
}

bool read_persisted_generation(detail::Reader& reader, PersistedGeneration& out,
                               std::size_t max_key_bytes, std::uint32_t limits_max_hops,
                               std::uint32_t limits_max_queues) {
  std::uint64_t generation = 0;
  if (!reader.u64(generation)) return false;
  out.generation = GenerationId::from_value(generation);
  if (!reader.boolean(out.is_current)) return false;
  std::int64_t first = 0;
  std::int64_t last = 0;
  std::int64_t received = 0;
  std::int64_t progress = 0;
  std::int64_t closed = 0;
  if (!reader.i64(first)) return false;
  if (!reader.i64(last)) return false;
  if (!reader.i64(received)) return false;
  if (!reader.i64(progress)) return false;
  if (!reader.i64(closed)) return false;
  out.first_observed_at = Timestamp::from_nanos(first);
  out.last_observed_at = Timestamp::from_nanos(last);
  out.last_received_at = Timestamp::from_nanos(received);
  out.last_progress_at = Timestamp::from_nanos(progress);
  out.closed_at = Timestamp::from_nanos(closed);
  std::uint8_t state = 0;
  std::uint8_t cause = 0;
  if (!reader.u8(state)) return false;
  if (!reader.u8(cause)) return false;
  if (state >= kFlowStateCount || cause >= kStateCauseCount) return false;
  out.state = static_cast<FlowState>(state);
  out.cause = static_cast<StateCause>(cause);
  if (!read_coverage(reader, out.coverage)) return false;

  std::optional<std::uint64_t> path;
  if (!reader.optional_u64(path)) return false;
  if (path.has_value()) {
    out.path = PathId::from_value(*path);
  } else {
    out.path.reset();
  }
  std::uint32_t link_count = 0;
  if (!reader.u32(link_count)) return false;
  if (link_count > limits_max_hops) return false;
  out.links.clear();
  out.links.reserve(link_count);
  for (std::uint32_t i = 0; i < link_count; ++i) {
    std::uint64_t link = 0;
    if (!reader.u64(link)) return false;
    out.links.push_back(LinkId::from_value(link));
  }
  std::uint32_t queue_count = 0;
  if (!reader.u32(queue_count)) return false;
  if (queue_count > limits_max_queues) return false;
  out.queues.clear();
  out.queues.reserve(queue_count);
  for (std::uint32_t i = 0; i < queue_count; ++i) {
    std::uint64_t queue = 0;
    if (!reader.u64(queue)) return false;
    out.queues.push_back(QueueId::from_value(queue));
  }
  std::uint64_t endpoint_a = 0;
  std::uint64_t endpoint_b = 0;
  if (!reader.u64(endpoint_a)) return false;
  if (!reader.u64(endpoint_b)) return false;
  out.endpoint_a = EndpointId::from_value(endpoint_a);
  out.endpoint_b = EndpointId::from_value(endpoint_b);

  if (!read_consumption(reader, out.forward)) return false;
  if (!read_consumption(reader, out.reverse)) return false;
  if (!read_consumption(reader, out.aggregate)) return false;
  if (!read_consumption(reader, out.total)) return false;

  if (!reader.boolean(out.unresolved_conflict)) return false;
  if (!reader.blob(out.conflict_detail, 4096)) return false;

  std::uint32_t source_count = 0;
  if (!reader.u32(source_count)) return false;
  out.sources.clear();
  out.sources.reserve(source_count);
  for (std::uint32_t i = 0; i < source_count; ++i) {
    PersistedSource source;
    if (!read_persisted_source(reader, source, max_key_bytes)) return false;
    out.sources.push_back(std::move(source));
  }
  return true;
}

void write_lifecycle_event(detail::Writer& writer, const LifecycleEvent& event) {
  writer.i64(event.at.nanos());
  writer.i64(event.observed_at.nanos());
  writer.u64(event.generation.flow().value());
  writer.u64(event.generation.generation().value());
  writer.u8(static_cast<std::uint8_t>(event.from));
  writer.u8(static_cast<std::uint8_t>(event.to));
  writer.u8(static_cast<std::uint8_t>(event.cause));
  writer.u64(event.source.value());
  writer.str(event.detail);
}

bool read_lifecycle_event(detail::Reader& reader, LifecycleEvent& out,
                          std::size_t max_detail_bytes) {
  std::int64_t at = 0;
  std::int64_t observed = 0;
  std::uint64_t flow = 0;
  std::uint64_t generation = 0;
  if (!reader.i64(at)) return false;
  if (!reader.i64(observed)) return false;
  if (!reader.u64(flow)) return false;
  if (!reader.u64(generation)) return false;
  out.at = Timestamp::from_nanos(at);
  out.observed_at = Timestamp::from_nanos(observed);
  out.generation = FlowGeneration(FlowId::from_value(flow), GenerationId::from_value(generation));
  std::uint8_t from = 0;
  std::uint8_t to = 0;
  std::uint8_t cause = 0;
  if (!reader.u8(from)) return false;
  if (!reader.u8(to)) return false;
  if (!reader.u8(cause)) return false;
  if (from >= kFlowStateCount || to >= kFlowStateCount || cause >= kStateCauseCount) {
    return false;
  }
  out.from = static_cast<FlowState>(from);
  out.to = static_cast<FlowState>(to);
  out.cause = static_cast<StateCause>(cause);
  std::uint64_t source = 0;
  if (!reader.u64(source)) return false;
  out.source = SourceId::from_value(source);
  if (!reader.blob(out.detail, max_detail_bytes)) return false;
  return true;
}

void write_anomaly(detail::Writer& writer, const Anomaly& anomaly) {
  writer.u8(static_cast<std::uint8_t>(anomaly.kind));
  writer.i64(anomaly.at.nanos());
  writer.i64(anomaly.observed_at.nanos());
  writer.u64(anomaly.generation.flow().value());
  writer.u64(anomaly.generation.generation().value());
  writer.u64(anomaly.source.value());
  writer.str(anomaly.detail);
}

bool read_anomaly(detail::Reader& reader, Anomaly& out, std::size_t max_detail_bytes) {
  std::uint8_t kind = 0;
  if (!reader.u8(kind)) return false;
  if (kind >= kAnomalyKindCount) return false;
  out.kind = static_cast<AnomalyKind>(kind);
  std::int64_t at = 0;
  std::int64_t observed = 0;
  if (!reader.i64(at)) return false;
  if (!reader.i64(observed)) return false;
  out.at = Timestamp::from_nanos(at);
  out.observed_at = Timestamp::from_nanos(observed);
  std::uint64_t flow = 0;
  std::uint64_t generation = 0;
  if (!reader.u64(flow)) return false;
  if (!reader.u64(generation)) return false;
  out.generation = FlowGeneration(FlowId::from_value(flow), GenerationId::from_value(generation));
  std::uint64_t source = 0;
  if (!reader.u64(source)) return false;
  out.source = SourceId::from_value(source);
  if (!reader.blob(out.detail, max_detail_bytes)) return false;
  return true;
}

void write_persisted_flow(detail::Writer& writer, const PersistedFlow& flow) {
  writer.u64(flow.id.value());
  writer.str(flow.canonical_key);
  writer.u64(flow.key_collisions);
  writer.u64(flow.current_generation.value());
  writer.u64(flow.revision.value());
  writer.u8(static_cast<std::uint8_t>(flow.state));
  writer.u8(static_cast<std::uint8_t>(flow.cause));
  write_coverage(writer, flow.coverage);
  writer.i64(flow.created_at.nanos());
  writer.i64(flow.state_changed_at.nanos());
  writer.i64(flow.first_observed_at.nanos());
  writer.i64(flow.last_observed_at.nanos());
  writer.i64(flow.last_received_at.nanos());
  writer.i64(flow.last_progress_at.nanos());
  writer.u64(flow.endpoint_a.value());
  writer.u64(flow.endpoint_b.value());

  writer.u32(static_cast<std::uint32_t>(flow.generations.size()));
  for (const PersistedGeneration& generation : flow.generations) {
    write_persisted_generation(writer, generation);
  }

  writer.u32(static_cast<std::uint32_t>(flow.history.size()));
  for (const LifecycleEvent& event : flow.history) {
    write_lifecycle_event(writer, event);
  }
  writer.u64(flow.history_dropped);

  writer.u32(static_cast<std::uint32_t>(flow.anomalies.size()));
  for (const Anomaly& anomaly : flow.anomalies) {
    write_anomaly(writer, anomaly);
  }
  writer.u64(flow.anomalies_dropped);
}

bool read_persisted_flow(detail::Reader& reader, PersistedFlow& out, const Limits& limits) {
  std::uint64_t id = 0;
  if (!reader.u64(id)) return false;
  out.id = FlowId::from_value(id);
  if (!reader.blob(out.canonical_key, limits.max_key_bytes)) return false;
  if (!reader.u64(out.key_collisions)) return false;
  std::uint64_t generation = 0;
  std::uint64_t revision = 0;
  if (!reader.u64(generation)) return false;
  if (!reader.u64(revision)) return false;
  out.current_generation = GenerationId::from_value(generation);
  out.revision = RevisionId::from_value(revision);
  std::uint8_t state = 0;
  std::uint8_t cause = 0;
  if (!reader.u8(state)) return false;
  if (!reader.u8(cause)) return false;
  if (state >= kFlowStateCount || cause >= kStateCauseCount) return false;
  out.state = static_cast<FlowState>(state);
  out.cause = static_cast<StateCause>(cause);
  if (!read_coverage(reader, out.coverage)) return false;
  std::int64_t created = 0;
  std::int64_t changed = 0;
  std::int64_t first = 0;
  std::int64_t last = 0;
  std::int64_t received = 0;
  std::int64_t progress = 0;
  if (!reader.i64(created)) return false;
  if (!reader.i64(changed)) return false;
  if (!reader.i64(first)) return false;
  if (!reader.i64(last)) return false;
  if (!reader.i64(received)) return false;
  if (!reader.i64(progress)) return false;
  out.created_at = Timestamp::from_nanos(created);
  out.state_changed_at = Timestamp::from_nanos(changed);
  out.first_observed_at = Timestamp::from_nanos(first);
  out.last_observed_at = Timestamp::from_nanos(last);
  out.last_received_at = Timestamp::from_nanos(received);
  out.last_progress_at = Timestamp::from_nanos(progress);
  std::uint64_t endpoint_a = 0;
  std::uint64_t endpoint_b = 0;
  if (!reader.u64(endpoint_a)) return false;
  if (!reader.u64(endpoint_b)) return false;
  out.endpoint_a = EndpointId::from_value(endpoint_a);
  out.endpoint_b = EndpointId::from_value(endpoint_b);

  std::uint32_t generation_count = 0;
  if (!reader.u32(generation_count)) return false;
  if (generation_count > limits.max_generations_per_flow) return false;
  out.generations.clear();
  out.generations.reserve(generation_count);
  for (std::uint32_t i = 0; i < generation_count; ++i) {
    PersistedGeneration item;
    if (!read_persisted_generation(reader, item, limits.max_key_bytes, limits.max_path_hops,
                                   limits.max_queues_per_flow)) {
      return false;
    }
    out.generations.push_back(std::move(item));
  }

  std::uint32_t history_count = 0;
  if (!reader.u32(history_count)) return false;
  if (history_count > limits.max_history_per_flow) return false;
  out.history.clear();
  out.history.reserve(history_count);
  for (std::uint32_t i = 0; i < history_count; ++i) {
    LifecycleEvent event;
    if (!read_lifecycle_event(reader, event, limits.max_detail_bytes)) return false;
    out.history.push_back(std::move(event));
  }
  if (!reader.u64(out.history_dropped)) return false;

  std::uint32_t anomaly_count = 0;
  if (!reader.u32(anomaly_count)) return false;
  if (anomaly_count > limits.max_anomalies_per_flow) return false;
  out.anomalies.clear();
  out.anomalies.reserve(anomaly_count);
  for (std::uint32_t i = 0; i < anomaly_count; ++i) {
    Anomaly anomaly;
    if (!read_anomaly(reader, anomaly, limits.max_detail_bytes)) return false;
    out.anomalies.push_back(std::move(anomaly));
  }
  if (!reader.u64(out.anomalies_dropped)) return false;
  return true;
}

void encode_into(detail::Writer& writer, const PersistedState& state) {
  writer.u16(state.format_version);
  writer.u32(state.identity_scheme);
  writer.u32(state.schema_version);
  writer.u64(state.policy_digest);
  writer.u64(state.runtime_epoch.value());
  writer.i64(state.created_at.nanos());
  writer.i64(state.snapshot_at.nanos());
  writer.u64(state.snapshot_sequence);
  writer.u32(static_cast<std::uint32_t>(state.sources.size()));
  for (const SourceDescriptor& source : state.sources) {
    write_source_descriptor(writer, source);
  }
  writer.u32(static_cast<std::uint32_t>(state.flows.size()));
  for (const PersistedFlow& flow : state.flows) {
    write_persisted_flow(writer, flow);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Journal
// ---------------------------------------------------------------------------

namespace journal {

std::string_view to_string(RecordKind kind) noexcept {
  switch (kind) {
    case RecordKind::SnapshotBegin: return "snapshot-begin";
    case RecordKind::SourceDescriptor: return "source-descriptor";
    case RecordKind::Flow: return "flow";
    case RecordKind::SnapshotEnd: return "snapshot-end";
    case RecordKind::LimitsRecord: return "limits";
    case RecordKind::EngineCounters: return "engine-counters";
    case RecordKind::Tombstone: return "tombstone";
    case RecordKind::ObservationRecord: return "observation";
  }
  return "invalid";
}

bool is_known_record_kind(std::uint16_t raw) noexcept {
  switch (static_cast<RecordKind>(raw)) {
    case RecordKind::SnapshotBegin:
    case RecordKind::SourceDescriptor:
    case RecordKind::Flow:
    case RecordKind::SnapshotEnd:
    case RecordKind::LimitsRecord:
    case RecordKind::EngineCounters:
    case RecordKind::Tombstone:
    case RecordKind::ObservationRecord:
      return true;
  }
  return false;
}

}  // namespace journal

Journal::~Journal() { (void)close(); }

Result<std::unique_ptr<Journal>> Journal::open(const std::filesystem::path& path,
                                               const JournalOpenOptions& options) {
  Status s = options.limits.validate();
  if (s.failed()) {
    return s;
  }
  s = options.recovery.validate();
  if (s.failed()) {
    return s;
  }

  auto handle = std::unique_ptr<Journal>(new Journal());
  handle->path_ = path;
  handle->limits_ = options.limits;
  handle->recovery_ = options.recovery;
  handle->read_only_ = options.read_only;

  std::error_code ec;
  const bool exists = std::filesystem::exists(path, ec);
  if (ec) {
    return io_error("cannot stat " + path.string() + ": " + ec.message());
  }
  if (!exists && !options.create_if_missing) {
    return not_found("journal does not exist: " + path.string());
  }

  if (exists && options.truncate_existing && !options.read_only) {
    std::filesystem::remove(path, ec);
    if (ec) {
      return io_error("cannot truncate " + path.string() + ": " + ec.message());
    }
  }

  const bool fresh = !std::filesystem::exists(path, ec);
  const char* mode = options.read_only ? "rb" : (fresh ? "w+b" : "r+b");
  std::FILE* file = open_journal_file(path.string(), mode, !options.read_only);
  if (file == nullptr) {
    if (!options.read_only && std::filesystem::exists(path, ec)) {
      return Status(ErrorCode::AlreadyOpen,
                    "journal is already open for writing by another runtime: " + path.string());
    }
    return io_error("cannot open " + path.string());
  }
  handle->file_ = file;

  if (fresh) {
    handle->header_ = JournalHeader{};
    handle->header_.created_at = Timestamp::from_nanos(0);
    s = handle->write_header();
    if (s.failed()) {
      return s;
    }
    return handle;
  }

  // Validate the existing header.
  std::byte raw[kHeaderBytes];
  if (std::fseek(file, 0, SEEK_SET) != 0 || std::fread(raw, 1, kHeaderBytes, file) != kHeaderBytes) {
    return Status(ErrorCode::TruncatedRecord, "journal header is truncated: " + path.string());
  }
  if (std::memcmp(raw, kMagic, sizeof(kMagic)) != 0) {
    return Status(ErrorCode::FormatMismatch, "journal magic does not match");
  }
  const std::uint32_t stored_crc = get_u32(raw + 52);
  std::vector<std::byte> header_body(raw, raw + 52);
  if (crc32c(header_body.data(), header_body.size()) != stored_crc) {
    return Status(ErrorCode::IntegrityFailure, "journal header checksum mismatch");
  }
  JournalHeader header;
  header.format_version = get_u16(raw + 8);
  header.identity_scheme = get_u16(raw + 10);
  header.flags = get_u32(raw + 12);
  header.runtime_epoch = RuntimeEpoch::from_value(get_u64(raw + 16));
  header.created_at = Timestamp::from_nanos(static_cast<std::int64_t>(get_u64(raw + 24)));
  header.record_count = get_u64(raw + 32);

  if (options.recovery.enforce_format_version) {
    if (header.format_version > kJournalFormatVersion ||
        header.format_version < kJournalMinReadableVersion) {
      return Status(ErrorCode::VersionMismatch,
                    "journal format version " + std::to_string(header.format_version) +
                        " is outside the supported range [" +
                        std::to_string(kJournalMinReadableVersion) + ", " +
                        std::to_string(kJournalFormatVersion) + "]");
    }
  }
  if (options.recovery.enforce_identity_scheme) {
    if (header.identity_scheme != static_cast<std::uint16_t>(kIdentitySchemeVersion)) {
      return Status(ErrorCode::VersionMismatch,
                    "journal identity scheme " + std::to_string(header.identity_scheme) +
                        " does not match this build (" +
                        std::to_string(kIdentitySchemeVersion) + ")");
    }
  }
  handle->header_ = header;
  handle->stats_.file_size = static_cast<std::size_t>(std::filesystem::file_size(path, ec));

  if (!options.read_only) {
    // Position at end of file for appends.
    if (std::fseek(file, 0, SEEK_END) != 0) {
      return io_error("cannot seek journal");
    }
  }
  return handle;
}

Status Journal::write_header() {
  std::byte raw[kHeaderBytes];
  std::memset(raw, 0, sizeof(raw));
  std::memcpy(raw, kMagic, sizeof(kMagic));
  put_u16(raw + 8, header_.format_version);
  put_u16(raw + 10, header_.identity_scheme);
  put_u32(raw + 12, header_.flags);
  put_u64(raw + 16, header_.runtime_epoch.value());
  put_u64(raw + 24, static_cast<std::uint64_t>(header_.created_at.nanos()));
  put_u64(raw + 32, header_.record_count);
  put_u64(raw + 40, 0);
  put_u32(raw + 48, 0);
  const std::uint32_t crc = crc32c(raw, 52);
  put_u32(raw + 52, crc);
  if (std::fseek(static_cast<std::FILE*>(file_), 0, SEEK_SET) != 0) {
    return io_error("cannot seek journal header");
  }
  if (std::fwrite(raw, 1, kHeaderBytes, static_cast<std::FILE*>(file_)) != kHeaderBytes) {
    return io_error("cannot write journal header");
  }
  dirty_ = true;
  return make_ok();
}

Status Journal::append(const journal::Record& record) {
  return append(std::vector<journal::Record>{record});
}

Status Journal::append(const std::vector<journal::Record>& records) {
  if (file_ == nullptr) {
    return Status(ErrorCode::NotOpen, "journal is not open");
  }
  if (read_only_) {
    return Status(ErrorCode::NotOpen, "journal is open read-only");
  }
  std::size_t pending = 0;
  for (const journal::Record& record : records) {
    pending += kRecordHeaderBytes + record.payload.size();
  }
  const Checked<std::uint64_t> projected =
      checked_add(static_cast<std::uint64_t>(stats_.file_size), static_cast<std::uint64_t>(pending));
  if (projected.overflow || projected.value > limits_.max_journal_bytes) {
    return limit_exceeded("journal would exceed limits.max_journal_bytes");
  }
  for (const journal::Record& record : records) {
    if (record.payload.size() > limits_.max_frame_bytes * 16u) {
      return limit_exceeded("record payload exceeds the configured bound");
    }
    std::vector<std::byte> header(kRecordHeaderBytes);
    put_u16(header.data(), static_cast<std::uint16_t>(record.kind));
    put_u16(header.data() + 2, 0);
    put_u32(header.data() + 4, static_cast<std::uint32_t>(record.payload.size()));
    Crc32c crc;
    crc.update(header.data(), 8);
    if (!record.payload.empty()) {
      crc.update(record.payload.data(), record.payload.size());
    }
    put_u32(header.data() + 8, crc.value());
    if (std::fwrite(header.data(), 1, header.size(), static_cast<std::FILE*>(file_)) !=
        header.size()) {
      return io_error("cannot write journal record header");
    }
    if (!record.payload.empty() &&
        std::fwrite(record.payload.data(), 1, record.payload.size(),
                    static_cast<std::FILE*>(file_)) != record.payload.size()) {
      return io_error("cannot write journal record payload");
    }
    stats_.records_written += 1;
    stats_.bytes_written += header.size() + record.payload.size();
    header_.record_count += 1;
  }
  stats_.file_size += pending;
  dirty_ = true;
  return make_ok();
}

Status Journal::flush() {
  if (file_ == nullptr) {
    return Status(ErrorCode::NotOpen, "journal is not open");
  }
  if (read_only_) {
    return make_ok();
  }
  if (dirty_) {
    sync_file(static_cast<std::FILE*>(file_));
    std::fseek(static_cast<std::FILE*>(file_), 0, SEEK_SET);
    const Status s = write_header();
    if (s.failed()) {
      return s;
    }
    sync_file(static_cast<std::FILE*>(file_));
    if (std::fseek(static_cast<std::FILE*>(file_), 0, SEEK_END) != 0) {
      return io_error("cannot seek journal");
    }
    dirty_ = false;
  }
  return make_ok();
}

Status Journal::close() {
  if (file_ == nullptr) {
    return make_ok();
  }
  Status s = flush();
  close_file(static_cast<std::FILE*>(file_), !read_only_);
  file_ = nullptr;
  return s;
}

Result<std::vector<journal::Record>> Journal::read_all() const {
  if (file_ == nullptr) {
    return Status(ErrorCode::NotOpen, "journal is not open");
  }
  std::vector<journal::Record> out;
  auto* file = static_cast<std::FILE*>(file_);
  if (std::fseek(file, 0, SEEK_END) != 0) {
    return io_error("cannot seek journal");
  }
  const long end = std::ftell(file);
  if (end < 0) {
    return io_error("cannot size journal");
  }
  if (static_cast<std::uint64_t>(end) > limits_.max_journal_bytes) {
    return limit_exceeded("journal exceeds limits.max_journal_bytes");
  }
  if (std::fseek(file, static_cast<long>(kHeaderBytes), SEEK_SET) != 0) {
    return io_error("cannot seek past journal header");
  }

  const std::uint64_t record_cap =
      static_cast<std::uint64_t>(limits_.max_result_set) * 1024ull + 1024ull;
  std::vector<std::byte> buffer;

  for (;;) {
    std::byte header[kRecordHeaderBytes];
    const std::size_t got = std::fread(header, 1, kRecordHeaderBytes, file);
    if (got == 0) {
      break;
    }
    if (got != kRecordHeaderBytes) {
      // Torn tail: an incomplete record header at the very end of the file.
      if (recovery_.torn_tail == RecoveryPolicy::TornTail::Reject) {
        return Status(ErrorCode::TruncatedRecord,
                      "journal ends with an incomplete record header");
      }
      break;
    }
    const std::uint16_t kind_raw = get_u16(header);
    const std::uint32_t length = get_u32(header + 4);
    const std::uint32_t stored_crc = get_u32(header + 8);
    if (!journal::is_known_record_kind(kind_raw)) {
      return Status(ErrorCode::FormatMismatch,
                    "unknown journal record kind " + std::to_string(kind_raw));
    }
    if (length > limits_.max_frame_bytes * 16u) {
      return limit_exceeded("journal record length exceeds the configured bound");
    }
    buffer.resize(length);
    if (length > 0) {
      const std::size_t payload_got = std::fread(buffer.data(), 1, length, file);
      if (payload_got != length) {
        if (recovery_.torn_tail == RecoveryPolicy::TornTail::Reject) {
          return Status(ErrorCode::TruncatedRecord,
                        "journal ends with an incomplete record payload");
        }
        break;
      }
    }
    Crc32c crc;
    crc.update(header, 8);
    if (length > 0) {
      crc.update(buffer.data(), length);
    }
    if (crc.value() != stored_crc) {
      // A complete record with a bad checksum is corruption, never a torn
      // tail. Refusing here is the conservative choice.
      return Status(ErrorCode::IntegrityFailure,
                    "journal record checksum mismatch for kind " + std::to_string(kind_raw));
    }
    if (out.size() >= record_cap) {
      return limit_exceeded("journal record count exceeds the configured bound");
    }
    journal::Record record;
    record.kind = static_cast<journal::RecordKind>(kind_raw);
    record.payload = buffer;
    out.push_back(std::move(record));
  }
  return out;
}

Status Journal::verify() const {
  Result<std::vector<journal::Record>> records = read_all();
  if (!records.ok()) {
    return records.status();
  }
  return make_ok();
}

Status Journal::rewrite(const std::vector<journal::Record>& records, RuntimeEpoch new_epoch,
                        Timestamp created_at) {
  if (read_only_) {
    return Status(ErrorCode::NotOpen, "journal is open read-only");
  }
  std::filesystem::path temp = path_;
  temp += ".compact";
  std::FILE* file = std::fopen(temp.string().c_str(), "w+b");
  if (file == nullptr) {
    return io_error("cannot create " + temp.string());
  }
  JournalHeader header;
  header.format_version = header_.format_version;
  header.identity_scheme = header_.identity_scheme;
  header.flags = header_.flags;
  header.runtime_epoch = new_epoch;
  header.created_at = created_at;

  std::byte raw[kHeaderBytes];
  std::memset(raw, 0, sizeof(raw));
  std::memcpy(raw, kMagic, sizeof(kMagic));
  put_u16(raw + 8, header.format_version);
  put_u16(raw + 10, header.identity_scheme);
  put_u32(raw + 12, header.flags);
  put_u64(raw + 16, header.runtime_epoch.value());
  put_u64(raw + 24, static_cast<std::uint64_t>(created_at.nanos()));
  put_u64(raw + 32, static_cast<std::uint64_t>(records.size()));
  put_u32(raw + 52, crc32c(raw, 52));
  if (std::fwrite(raw, 1, kHeaderBytes, file) != kHeaderBytes) {
    close_file(file);
    std::error_code ec;
    std::filesystem::remove(temp, ec);
    return io_error("cannot write compacted header");
  }
  std::size_t total = kHeaderBytes;
  for (const journal::Record& record : records) {
    std::vector<std::byte> record_header(kRecordHeaderBytes);
    put_u16(record_header.data(), static_cast<std::uint16_t>(record.kind));
    put_u32(record_header.data() + 4, static_cast<std::uint32_t>(record.payload.size()));
    Crc32c crc;
    crc.update(record_header.data(), 8);
    if (!record.payload.empty()) {
      crc.update(record.payload.data(), record.payload.size());
    }
    put_u32(record_header.data() + 8, crc.value());
    if (std::fwrite(record_header.data(), 1, record_header.size(), file) != record_header.size() ||
        (!record.payload.empty() &&
         std::fwrite(record.payload.data(), 1, record.payload.size(), file) !=
             record.payload.size())) {
      close_file(file);
      std::error_code ec;
      std::filesystem::remove(temp, ec);
      return io_error("cannot write compacted record");
    }
    total += record_header.size() + record.payload.size();
  }
  sync_file(file);
  std::fclose(file);

  if (file_ != nullptr) {
    close_file(static_cast<std::FILE*>(file_));
    file_ = nullptr;
  }
  Status s = atomic_replace(temp, path_);
  if (s.failed()) {
    return s;
  }
  std::FILE* reopened = std::fopen(path_.string().c_str(), "r+b");
  if (reopened == nullptr) {
    return io_error("cannot reopen " + path_.string());
  }
  if (std::fseek(reopened, 0, SEEK_END) != 0) {
    close_file(reopened);
    return io_error("cannot seek compacted journal");
  }
  file_ = reopened;
  header_ = header;
  stats_.file_size = total;
  stats_.compactions += 1;
  return make_ok();
}

// ---------------------------------------------------------------------------
// State codec
// ---------------------------------------------------------------------------

Status encode_state(const PersistedState& state, const Limits& limits,
                    std::vector<journal::Record>& out) {
  Status s = limits.validate();
  if (s.failed()) {
    return s;
  }
  if (state.flows.size() > limits.max_flows) {
    return limit_exceeded("snapshot contains more flows than limits.max_flows");
  }
  if (state.sources.size() > limits.max_sources) {
    return limit_exceeded("snapshot contains more sources than limits.max_sources");
  }

  out.clear();
  detail::Writer header;
  header.u16(state.format_version);
  header.u32(state.identity_scheme);
  header.u32(state.schema_version);
  header.u64(state.policy_digest);
  header.u64(state.runtime_epoch.value());
  header.i64(state.created_at.nanos());
  header.i64(state.snapshot_at.nanos());
  header.u64(state.snapshot_sequence);
  journal::Record begin;
  begin.kind = journal::RecordKind::SnapshotBegin;
  begin.payload = header.take();
  out.push_back(std::move(begin));

  detail::Writer limits_writer;
  limits_writer.str(limits.describe());
  journal::Record limits_record;
  limits_record.kind = journal::RecordKind::LimitsRecord;
  limits_record.payload = limits_writer.take();
  out.push_back(std::move(limits_record));

  for (const SourceDescriptor& source : state.sources) {
    detail::Writer writer;
    write_source_descriptor(writer, source);
    journal::Record record;
    record.kind = journal::RecordKind::SourceDescriptor;
    record.payload = writer.take();
    out.push_back(std::move(record));
  }
  for (const PersistedFlow& flow : state.flows) {
    detail::Writer writer;
    write_persisted_flow(writer, flow);
    journal::Record record;
    record.kind = journal::RecordKind::Flow;
    record.payload = writer.take();
    out.push_back(std::move(record));
  }

  // The digest covers every record written before SnapshotEnd, in order. A
  // reordered, duplicated or truncated group therefore fails even when each
  // individual CRC is intact.
  std::uint64_t running = 0;
  for (const journal::Record& record : out) {
    running = crc32c(record.payload.data(), record.payload.size()) ^ (running * 1099511628211ull);
  }

  detail::Writer end;
  end.u64(running);
  end.u32(static_cast<std::uint32_t>(state.sources.size()));
  end.u32(static_cast<std::uint32_t>(state.flows.size()));
  journal::Record end_record;
  end_record.kind = journal::RecordKind::SnapshotEnd;
  end_record.payload = end.take();
  out.push_back(std::move(end_record));

  std::size_t total = 0;
  for (const journal::Record& record : out) {
    total += record.payload.size() + kRecordHeaderBytes;
  }
  if (static_cast<std::uint64_t>(total) > limits.max_journal_bytes / 2ull) {
    return limit_exceeded("encoded snapshot exceeds half of limits.max_journal_bytes");
  }
  return make_ok();
}

Result<PersistedState> decode_state(const std::vector<journal::Record>& records,
                                    const Limits& limits, const RecoveryPolicy& recovery) {
  if (records.empty()) {
    return Status(ErrorCode::IntegrityFailure, "snapshot record group is empty");
  }
  if (records.front().kind != journal::RecordKind::SnapshotBegin) {
    return Status(ErrorCode::FormatMismatch, "snapshot group does not begin with SnapshotBegin");
  }
  if (records.back().kind != journal::RecordKind::SnapshotEnd) {
    return Status(ErrorCode::IntegrityFailure,
                  "snapshot group is incomplete: missing SnapshotEnd");
  }

  detail::Reader begin(records.front().payload);
  PersistedState state;
  if (!begin.u16(state.format_version)) return Status(ErrorCode::FormatMismatch, "bad begin payload");
  if (!begin.u32(state.identity_scheme)) return Status(ErrorCode::FormatMismatch, "bad begin payload");
  if (!begin.u32(state.schema_version)) return Status(ErrorCode::FormatMismatch, "bad begin payload");
  if (!begin.u64(state.policy_digest)) return Status(ErrorCode::FormatMismatch, "bad begin payload");
  std::uint64_t epoch = 0;
  if (!begin.u64(epoch)) return Status(ErrorCode::FormatMismatch, "bad begin payload");
  state.runtime_epoch = RuntimeEpoch::from_value(epoch);
  std::int64_t created = 0;
  std::int64_t snapshot_at = 0;
  if (!begin.i64(created)) return Status(ErrorCode::FormatMismatch, "bad begin payload");
  if (!begin.i64(snapshot_at)) return Status(ErrorCode::FormatMismatch, "bad begin payload");
  state.created_at = Timestamp::from_nanos(created);
  state.snapshot_at = Timestamp::from_nanos(snapshot_at);
  if (!begin.u64(state.snapshot_sequence)) {
    return Status(ErrorCode::FormatMismatch, "bad begin payload");
  }
  if (!begin.finish()) {
    return Status(ErrorCode::FormatMismatch, "SnapshotBegin payload has trailing bytes");
  }

  if (recovery.enforce_format_version) {
    if (state.format_version > kJournalFormatVersion ||
        state.format_version < kJournalMinReadableVersion) {
      return Status(ErrorCode::VersionMismatch, "snapshot format version is not supported");
    }
  }
  if (recovery.enforce_identity_scheme && state.identity_scheme != kIdentitySchemeVersion) {
    return Status(ErrorCode::VersionMismatch, "snapshot identity scheme does not match");
  }

  // Compute the running digest over every payload except SnapshotEnd so that a
  // truncated or reordered group is detected even when every CRC is valid.
  std::uint64_t running = 0;
  for (std::size_t i = 0; i + 1 < records.size(); ++i) {
    running = crc32c(records[i].payload.data(), records[i].payload.size()) ^
              (running * 1099511628211ull);
  }

  std::uint64_t expected = 0;
  std::uint32_t source_count = 0;
  std::uint32_t flow_count = 0;
  detail::Reader end(records.back().payload);
  if (!end.u64(expected)) return Status(ErrorCode::FormatMismatch, "bad SnapshotEnd payload");
  if (!end.u32(source_count)) return Status(ErrorCode::FormatMismatch, "bad SnapshotEnd payload");
  if (!end.u32(flow_count)) return Status(ErrorCode::FormatMismatch, "bad SnapshotEnd payload");
  if (!end.finish()) {
    return Status(ErrorCode::FormatMismatch, "SnapshotEnd payload has trailing bytes");
  }
  if (expected != running) {
    return Status(ErrorCode::IntegrityFailure,
                  "snapshot group digest mismatch: the record group is not the one that was written");
  }

  for (std::size_t i = 1; i + 1 < records.size(); ++i) {
    const journal::Record& record = records[i];
    switch (record.kind) {
      case journal::RecordKind::LimitsRecord: {
        detail::Reader reader(record.payload);
        std::string text;
        if (!reader.blob(text, 8192) || !reader.finish()) {
          return Status(ErrorCode::FormatMismatch, "bad limits record");
        }
        break;
      }
      case journal::RecordKind::SourceDescriptor: {
        detail::Reader reader(record.payload);
        SourceDescriptor source;
        if (!read_source_descriptor(reader, source, limits.max_key_bytes) || !reader.finish()) {
          return Status(ErrorCode::FormatMismatch, "bad source descriptor record");
        }
        if (state.sources.size() >= limits.max_sources) {
          return limit_exceeded("snapshot contains more sources than limits.max_sources");
        }
        state.sources.push_back(std::move(source));
        break;
      }
      case journal::RecordKind::Flow: {
        detail::Reader reader(record.payload);
        PersistedFlow flow;
        if (!read_persisted_flow(reader, flow, limits) || !reader.finish()) {
          return Status(ErrorCode::FormatMismatch, "bad flow record");
        }
        if (state.flows.size() >= limits.max_flows) {
          return limit_exceeded("snapshot contains more flows than limits.max_flows");
        }
        state.flows.push_back(std::move(flow));
        break;
      }
      case journal::RecordKind::EngineCounters:
      case journal::RecordKind::Tombstone:
      case journal::RecordKind::ObservationRecord:
        // An observation record inside a bracketed snapshot group would make
        // the group's flow set depend on replay order; the writer never emits
        // one, so its presence is a format error.
        return Status(ErrorCode::FormatMismatch,
                      "observation record inside a snapshot group");
      case journal::RecordKind::SnapshotBegin:
      case journal::RecordKind::SnapshotEnd:
        return Status(ErrorCode::FormatMismatch,
                      "snapshot group contains a nested begin/end record");
    }
  }

  if (state.sources.size() != source_count || state.flows.size() != flow_count) {
    return Status(ErrorCode::IntegrityFailure,
                  "snapshot record counts do not match the SnapshotEnd declaration");
  }
  return state;
}

Result<ReplayResult> replay_records(const std::vector<journal::Record>& records,
                                    const Limits& limits, const RecoveryPolicy& recovery) {
  ReplayResult result;
  std::size_t index = 0;
  while (index < records.size()) {
    const journal::Record& record = records[index];
    if (record.kind == journal::RecordKind::SnapshotBegin) {
      std::size_t end = index + 1;
      while (end < records.size() && records[end].kind != journal::RecordKind::SnapshotEnd) {
        if (records[end].kind == journal::RecordKind::SnapshotBegin) {
          return Status(ErrorCode::FormatMismatch,
                        "snapshot group contains a nested SnapshotBegin");
        }
        ++end;
      }
      if (end >= records.size()) {
        return Status(ErrorCode::IntegrityFailure,
                      "snapshot group is not closed by SnapshotEnd");
      }
      std::vector<journal::Record> group(records.begin() + static_cast<std::ptrdiff_t>(index),
                                         records.begin() + static_cast<std::ptrdiff_t>(end + 1));
      Result<PersistedState> decoded = decode_state(group, limits, recovery);
      if (!decoded.ok()) {
        return decoded.status();
      }
      result.state = std::move(decoded.value());
      result.observations.clear();
      result.saw_snapshot = true;
      result.snapshots += 1;
      index = end + 1;
      continue;
    }
    if (record.kind == journal::RecordKind::SnapshotEnd) {
      return Status(ErrorCode::FormatMismatch, "SnapshotEnd without SnapshotBegin");
    }
    if (record.kind == journal::RecordKind::SourceDescriptor) {
      detail::Reader reader(record.payload);
      SourceDescriptor source;
      if (!read_source_descriptor(reader, source, limits.max_key_bytes) || !reader.finish()) {
        return Status(ErrorCode::IntegrityFailure, "malformed source descriptor record");
      }
      bool replaced = false;
      for (SourceDescriptor& existing : result.state.sources) {
        if (existing.id == source.id) {
          existing = source;
          replaced = true;
          break;
        }
      }
      if (!replaced) {
        if (result.state.sources.size() >= limits.max_sources) {
          return limit_exceeded("journal declares more sources than limits.max_sources");
        }
        result.state.sources.push_back(std::move(source));
      }
      result.source_records += 1;
      ++index;
      continue;
    }
    if (record.kind == journal::RecordKind::ObservationRecord) {
      detail::Reader reader(record.payload);
      Observation observation;
      if (!detail::decode_observation(reader, limits, observation) || !reader.finish()) {
        return Status(ErrorCode::IntegrityFailure, "malformed observation record");
      }
      if (result.observations.size() >= limits.max_ingest_records) {
        return limit_exceeded("journal holds more observations than limits.max_ingest_records");
      }
      result.observations.push_back(std::move(observation));
      result.observation_records += 1;
      ++index;
      continue;
    }
    if (record.kind == journal::RecordKind::Flow) {
      detail::Reader reader(record.payload);
      PersistedFlow flow;
      if (!read_persisted_flow(reader, flow, limits) || !reader.finish()) {
        return Status(ErrorCode::IntegrityFailure, "malformed flow record");
      }
      bool replaced = false;
      for (PersistedFlow& existing : result.state.flows) {
        if (existing.id == flow.id) {
          existing = std::move(flow);
          replaced = true;
          break;
        }
      }
      if (!replaced) {
        if (result.state.flows.size() >= limits.max_flows) {
          return limit_exceeded("journal declares more flows than limits.max_flows");
        }
        result.state.flows.push_back(std::move(flow));
      }
      ++index;
      continue;
    }
    if (record.kind == journal::RecordKind::LimitsRecord ||
        record.kind == journal::RecordKind::EngineCounters ||
        record.kind == journal::RecordKind::Tombstone) {
      ++index;
      continue;
    }
    return Status(ErrorCode::FormatMismatch, "unexpected record kind during replay");
  }
  return result;
}

std::uint64_t digest_state(const PersistedState& state) noexcept {
  detail::Writer writer;
  for (const SourceDescriptor& source : state.sources) {
    write_source_descriptor(writer, source);
  }
  for (const PersistedFlow& flow : state.flows) {
    write_persisted_flow(writer, flow);
  }
  detail::Writer header;
  header.u16(state.format_version);
  header.u32(state.identity_scheme);
  header.u32(state.schema_version);
  header.u64(state.policy_digest);
  header.u64(state.runtime_epoch.value());
  header.i64(state.created_at.nanos());
  header.i64(state.snapshot_at.nanos());
  header.u64(state.snapshot_sequence);
  const std::vector<std::byte>& a = header.bytes();
  const std::vector<std::byte>& b = writer.bytes();
  Crc32c crc;
  crc.update(a.data(), a.size());
  crc.update(b.data(), b.size());
  return static_cast<std::uint64_t>(crc.value()) << 32u |
         static_cast<std::uint64_t>(fnv1a64_raw(b.data(), b.size()) & 0xFFFFFFFFull);
}

}  // namespace flowobs
