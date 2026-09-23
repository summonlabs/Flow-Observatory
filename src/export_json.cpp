// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowobs/export_json.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include "flowobs/checked.hpp"
#include "flowobs/version.hpp"
#include "render.hpp"

namespace flowobs {
namespace json {

std::string escape(std::string_view input) {
  std::string out;
  out.reserve(input.size() + 8u);
  for (const char raw : input) {
    const auto byte = static_cast<unsigned char>(raw);
    switch (raw) {
      case '"': out.append("\\\""); break;
      case '\\': out.append("\\\\"); break;
      case '\b': out.append("\\b"); break;
      case '\f': out.append("\\f"); break;
      case '\n': out.append("\\n"); break;
      case '\r': out.append("\\r"); break;
      case '\t': out.append("\\t"); break;
      default:
        if (byte < 0x20u) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(byte));
          out.append(buffer);
        } else {
          out.push_back(raw);
        }
        break;
    }
  }
  return out;
}

void Writer::indent() {
  if (!pretty_) {
    return;
  }
  out_.push_back('\n');
  for (int i = 0; i < depth_; ++i) {
    out_.append("  ");
  }
}

void Writer::separate() {
  if (need_comma_) {
    out_.push_back(',');
  }
  if (depth_ > 0) {
    indent();
  }
  need_comma_ = true;
}

void Writer::begin_object() {
  separate();
  out_.push_back('{');
  ++depth_;
  need_comma_ = false;
  stack_.push_back(false);
}

void Writer::end_object() {
  const bool had_content = !stack_.empty() && stack_.back();
  if (!stack_.empty()) {
    stack_.pop_back();
  }
  --depth_;
  if (had_content) {
    indent();
  }
  out_.push_back('}');
  need_comma_ = true;
}

void Writer::begin_array() {
  separate();
  out_.push_back('[');
  ++depth_;
  need_comma_ = false;
  stack_.push_back(false);
}

void Writer::end_array() {
  const bool had_content = !stack_.empty() && stack_.back();
  if (!stack_.empty()) {
    stack_.pop_back();
  }
  --depth_;
  if (had_content) {
    indent();
  }
  out_.push_back(']');
  need_comma_ = true;
}

void Writer::key(std::string_view name) {
  separate();
  out_.push_back('"');
  out_.append(escape(name));
  out_.append("\":");
  if (pretty_) {
    out_.push_back(' ');
  }
  need_comma_ = false;
  if (!stack_.empty()) {
    stack_.back() = true;
  }
}

void Writer::value_string(std::string_view value) {
  separate();
  out_.push_back('"');
  out_.append(escape(value));
  out_.push_back('"');
}

void Writer::value_uint(std::uint64_t value) {
  separate();
  out_.append(std::to_string(value));
}

void Writer::value_int(std::int64_t value) {
  separate();
  out_.append(std::to_string(value));
}

void Writer::value_bool(bool value) {
  separate();
  out_.append(value ? "true" : "false");
}

void Writer::value_null() {
  separate();
  out_.append("null");
}

void Writer::value_ratio(std::uint64_t numerator, std::uint64_t denominator, unsigned scale) {
  std::string text;
  if (denominator == 0) {
    value_null();
    return;
  }
  text.append(std::to_string(numerator / denominator));
  text.push_back('.');
  std::uint64_t remainder = numerator % denominator;
  for (unsigned i = 0; i < scale; ++i) {
    const Checked<std::uint64_t> scaled = checked_mul(remainder, std::uint64_t{10});
    if (scaled.overflow) {
      remainder = 0;
      text.push_back('0');
      continue;
    }
    text.push_back(static_cast<char>('0' + (scaled.value / denominator)));
    remainder = scaled.value % denominator;
  }
  value_string(text);
}

void Writer::field(std::string_view name, std::string_view value) {
  key(name);
  value_string(value);
}
void Writer::field_null(std::string_view name) {
  key(name);
  value_null();
}
void Writer::field_ratio(std::string_view name, std::uint64_t numerator,
                         std::uint64_t denominator, unsigned scale) {
  key(name);
  value_ratio(numerator, denominator, scale);
}

}  // namespace json

namespace detail {

Throughput compute_throughput(Timestamp first, Timestamp last,
                              const Consumption& consumption) {
  Throughput throughput;
  if (!first.known() || !last.known() || last <= first) {
    return throughput;
  }
  const Duration span = time_difference(last, first);
  if (span.nanos() <= 0) {
    return throughput;
  }
  throughput.duration_known = true;
  throughput.duration_nanos = static_cast<std::uint64_t>(span.nanos());
  const Checked<std::uint64_t> bytes =
      checked_mul_div(consumption.bytes, 1000000000000ull, throughput.duration_nanos);
  const Checked<std::uint64_t> packets =
      checked_mul_div(consumption.packets, 1000000000000ull, throughput.duration_nanos);
  throughput.bytes_per_second_milli = bytes.value;
  throughput.packets_per_second_milli = packets.value;
  throughput.saturated = bytes.overflow || packets.overflow;
  return throughput;
}

void write_consumption(json::Writer& writer, const Consumption& consumption,
                       std::string_view name) {
  writer.key(name);
  writer.begin_object();
  if (consumption.bytes_unsupported) {
    writer.field_null("bytes");
  } else {
    writer.field("bytes", consumption.bytes);
  }
  if (consumption.packets_unsupported) {
    writer.field_null("packets");
  } else {
    writer.field("packets", consumption.packets);
  }
  if (consumption.retransmissions_unsupported) {
    writer.field_null("retransmissions");
  } else {
    writer.field("retransmissions", consumption.retransmissions);
  }
  writer.field("unknown_intervals", consumption.unknown_intervals);
  writer.field("exact", consumption.is_exact());
  writer.field("saturated", consumption.saturated);
  writer.end_object();
}

namespace {

void write_coverage(json::Writer& writer, const Coverage& coverage) {
  writer.key("coverage");
  writer.begin_object();
  writer.field("forward", coverage.forward_complete
                              ? "complete"
                              : (coverage.forward_partial ? "partial" : "unobserved"));
  writer.field("reverse", coverage.reverse_complete
                              ? "complete"
                              : (coverage.reverse_partial ? "partial" : "unobserved"));
  writer.field("bidirectional_proven", coverage.bidirectional());
  writer.end_object();
}

const char* yes_no(bool value) { return value ? "yes" : "no"; }

}  // namespace

void write_anomaly_json(json::Writer& writer, const Anomaly& anomaly) {
  writer.begin_object();
  writer.field("kind", flowobs::to_string(anomaly.kind));
  writer.field("conservative_refusal", anomaly_is_conservative_refusal(anomaly.kind));
  writer.field("at", anomaly.at.nanos());
  writer.field("observed_at", anomaly.observed_at.nanos());
  if (anomaly.generation.valid()) {
    writer.field("flow", flowobs::to_string(anomaly.generation.flow()));
    writer.field("generation", anomaly.generation.generation().value());
  }
  if (anomaly.source.valid()) {
    writer.field("source", flowobs::to_string(anomaly.source));
  }
  writer.field("detail", anomaly.detail);
  writer.end_object();
}

void write_source_json(json::Writer& writer, const SourceView& source) {
  writer.begin_object();
  writer.field("source", flowobs::to_string(source.source));
  writer.field("key", source.canonical_key);
  writer.field("authority", source.authority);
  writer.field("advisory_only", source.advisory_only);
  writer.field("capabilities", source.capabilities.describe());
  writer.field("incarnation", source.incarnation.value());
  writer.field("epoch", source.epoch.value());
  writer.field("high_revision", source.high_revision.value());
  writer.field("high_sequence", source.high_sequence.value());
  writer.field("first_observed_at", source.first_observed_at.nanos());
  writer.field("last_observed_at", source.last_observed_at.nanos());
  writer.field("last_received_at", source.last_received_at.nanos());
  writer.field("last_progress_at", source.last_progress_at.nanos());
  writer.field("freshness", flowobs::to_string(source.freshness));
  writer.field("evidence", flowobs::to_string(source.last_evidence));
  writer.field("last_kind", flowobs::to_string(source.last_kind));
  writer.field("last_direction", flowobs::to_string(source.last_direction));
  writer.field("restored_from_journal", source.restored_from_journal);
  writer.field("reconfirmed", source.reconfirmed);
  writer.field("usable_for_liveness", source.usable_for_liveness());
  write_coverage(writer, source.coverage);
  write_consumption(writer, source.forward_totals, "forward");
  write_consumption(writer, source.reverse_totals, "reverse");
  write_consumption(writer, source.aggregate_totals, "aggregate");
  if (source.declared_terminal.has_value()) {
    writer.field("declared_terminal", flowobs::to_string(*source.declared_terminal));
  } else {
    writer.field_null("declared_terminal");
  }
  writer.field("accepted_observations", source.accepted_observations);
  writer.field("dropped_observations", source.dropped_observations);
  writer.field("duplicates_suppressed", source.duplicates_suppressed);
  writer.field("content_conflicts", source.content_conflicts);
  writer.field("distinct_incarnations", source.distinct_incarnations);
  writer.field("regressions", source.regressions);
  writer.field("restart_gaps", source.restart_gaps);
  writer.field("saturated", source.saturated);
  writer.key("anomalies");
  writer.begin_object();
  for (std::size_t i = 0; i < kAnomalyKindCount; ++i) {
    if (source.anomaly_counts[i] == 0) {
      continue;
    }
    writer.field(flowobs::to_string(static_cast<AnomalyKind>(i)),
                 static_cast<std::uint64_t>(source.anomaly_counts[i]));
  }
  writer.end_object();
  writer.end_object();
}

void write_generation_json(json::Writer& writer, const GenerationView& generation,
                           const ExportOptions& options) {
  writer.begin_object();
  writer.field("generation", generation.generation.generation().value());
  writer.field("flow", flowobs::to_string(generation.generation.flow()));
  writer.field("is_current", generation.is_current);
  writer.field("state", flowobs::to_string(generation.state));
  writer.field("cause", flowobs::to_string(generation.cause));
  writer.field("freshness", flowobs::to_string(generation.freshness));
  writer.field("consistency", flowobs::to_string(generation.consistency));
  writer.field("visibility", flowobs::to_string(generation.visibility));
  writer.field("unresolved_conflict", generation.unresolved_conflict);
  if (!generation.conflict_detail.empty()) {
    writer.field("conflict_detail", generation.conflict_detail);
  }
  writer.field("first_observed_at", generation.first_observed_at.nanos());
  writer.field("last_observed_at", generation.last_observed_at.nanos());
  writer.field("last_received_at", generation.last_received_at.nanos());
  writer.field("last_progress_at", generation.last_progress_at.nanos());
  writer.field("closed_at", generation.closed_at.nanos());
  const Throughput throughput =
      compute_throughput(generation.first_observed_at, generation.last_observed_at,
                         generation.total);
  writer.field("duration_ns", throughput.duration_nanos);
  writer.field("duration_known", throughput.duration_known);
  writer.field("throughput_bytes_per_second_milli", throughput.bytes_per_second_milli);
  writer.field("throughput_packets_per_second_milli", throughput.packets_per_second_milli);
  writer.field("throughput_saturated", throughput.saturated);
  writer.field("endpoint_a", flowobs::to_string(generation.endpoint_a));
  writer.field("endpoint_b", flowobs::to_string(generation.endpoint_b));
  if (generation.path.has_value()) {
    writer.field("path", flowobs::to_string(*generation.path));
  } else {
    writer.field_null("path");
  }
  writer.field("has_path_correlation", generation.has_path_correlation);
  writer.field("has_queue_correlation", generation.has_queue_correlation);
  writer.field("primary_source", flowobs::to_string(generation.primary_source));
  writer.field("excluded_stale_sources", generation.excluded_stale_sources);
  writer.field("excluded_advisory_sources", generation.excluded_advisory_sources);
  write_coverage(writer, generation.coverage);
  write_consumption(writer, generation.forward, "forward");
  write_consumption(writer, generation.reverse, "reverse");
  write_consumption(writer, generation.aggregate, "aggregate");
  write_consumption(writer, generation.total, "total");
  writer.key("links");
  writer.begin_array();
  for (const LinkId link : generation.links) {
    writer.value_string(flowobs::to_string(link));
  }
  writer.end_array();
  writer.key("queues");
  writer.begin_array();
  for (const QueueId queue : generation.queues) {
    writer.value_string(flowobs::to_string(queue));
  }
  writer.end_array();
  writer.key("hop_capacity_units");
  writer.begin_array();
  for (const std::uint64_t capacity : generation.hop_capacity_units) {
    writer.value_uint(capacity);
  }
  writer.end_array();
  if (options.include_sources) {
    writer.key("sources");
    writer.begin_array();
    for (const SourceView& source : generation.sources) {
      write_source_json(writer, source);
    }
    writer.end_array();
  }
  writer.end_object();
}

void write_flow_json(json::Writer& writer, const FlowSnapshot& snapshot,
                     const ExportOptions& options) {
  writer.begin_object();
  writer.field("flow", flowobs::to_string(snapshot.id));
  writer.field("key", snapshot.canonical_key);
  writer.field("state", flowobs::to_string(snapshot.state));
  writer.field("cause", flowobs::to_string(snapshot.cause));
  writer.field("freshness", flowobs::to_string(snapshot.freshness));
  writer.field("consistency", flowobs::to_string(snapshot.consistency));
  writer.field("visibility", flowobs::to_string(snapshot.visibility));
  writer.field("live", snapshot.is_live());
  writer.field("completion_proven", asserts_completion(snapshot.state));
  writer.field("current_generation", snapshot.current_generation.value());
  writer.field("revision", snapshot.revision.value());
  writer.field("created_at", snapshot.created_at.nanos());
  writer.field("state_changed_at", snapshot.state_changed_at.nanos());
  writer.field("first_observed_at", snapshot.first_observed_at.nanos());
  writer.field("last_observed_at", snapshot.last_observed_at.nanos());
  writer.field("last_received_at", snapshot.last_received_at.nanos());
  writer.field("last_progress_at", snapshot.last_progress_at.nanos());
  writer.field("endpoint_a", flowobs::to_string(snapshot.endpoint_a));
  writer.field("endpoint_b", flowobs::to_string(snapshot.endpoint_b));
  writer.field("restored_from_journal", snapshot.restored_from_journal);
  writer.field("restored_epoch", snapshot.restored_epoch.value());
  writer.field("current_epoch", snapshot.current_epoch.value());
  writer.field("key_collisions", snapshot.key_collisions);
  writer.field("anomalies_dropped", snapshot.anomalies_dropped);
  const Throughput throughput =
      compute_throughput(snapshot.first_observed_at, snapshot.last_observed_at, snapshot.total);
  writer.field("duration_ns", throughput.duration_nanos);
  writer.field("throughput_bytes_per_second_milli", throughput.bytes_per_second_milli);
  write_coverage(writer, snapshot.coverage);
  write_consumption(writer, snapshot.total, "total");
  if (options.include_generations) {
    writer.key("generations");
    writer.begin_array();
    for (const GenerationView& generation : snapshot.generations) {
      write_generation_json(writer, generation, options);
    }
    writer.end_array();
  }
  if (options.include_anomalies) {
    writer.key("anomalies");
    writer.begin_array();
    for (const Anomaly& anomaly : snapshot.anomalies) {
      write_anomaly_json(writer, anomaly);
    }
    writer.end_array();
  }
  writer.end_object();
}

void write_query_result_json(json::Writer& writer, const QueryResult& result,
                             const ExportOptions& options) {
  writer.begin_object();
  writer.field("schema", "flow-observatory/export/1");
  writer.field("engine_version", version_string());
  writer.field("api_version", static_cast<std::uint64_t>(kApiVersion));
  writer.field("identity_scheme", static_cast<std::uint64_t>(kIdentitySchemeVersion));
  writer.field("journal_format_version", static_cast<std::uint64_t>(kJournalFormatVersion));
  writer.field("evaluated_at", result.evaluated_at.nanos());
  writer.field("evaluated_at_utc", format_timestamp(result.evaluated_at));
  writer.key("result");
  writer.begin_object();
  writer.field("matched", static_cast<std::uint64_t>(result.matched));
  writer.field("returned", static_cast<std::uint64_t>(result.returned));
  writer.field("truncated", result.truncated);
  writer.end_object();
  writer.field("deterministic", options.deterministic);
  writer.key("flows");
  writer.begin_array();
  for (const FlowSnapshot& snapshot : result.flows) {
    write_flow_json(writer, snapshot, options);
  }
  writer.end_array();
  writer.end_object();
}

std::string render_csv(const QueryResult& result) {
  std::string out;
  out.reserve(256 + result.flows.size() * 192u);
  out.append(
      "flow,key,state,cause,freshness,consistency,visibility,current_generation,revision,"
      "first_observed_at,last_observed_at,duration_ns,bytes,packets,retransmissions,"
      "unknown_intervals,total_exact,throughput_bytes_per_second_milli,restored\n");
  for (const FlowSnapshot& snapshot : result.flows) {
    const Throughput throughput =
        compute_throughput(snapshot.first_observed_at, snapshot.last_observed_at, snapshot.total);
    const auto cell = [&out](const std::string& text) {
      out.push_back('"');
      for (const char c : text) {
        if (c == '"') {
          out.append("\"\"");
        } else {
          out.push_back(c);
        }
      }
      out.push_back('"');
    };
    cell(flowobs::to_string(snapshot.id));
    out.push_back(',');
    cell(snapshot.canonical_key);
    out.push_back(',');
    cell(std::string(flowobs::to_string(snapshot.state)));
    out.push_back(',');
    cell(std::string(flowobs::to_string(snapshot.cause)));
    out.push_back(',');
    cell(std::string(flowobs::to_string(snapshot.freshness)));
    out.push_back(',');
    cell(std::string(flowobs::to_string(snapshot.consistency)));
    out.push_back(',');
    cell(std::string(flowobs::to_string(snapshot.visibility)));
    out.push_back(',');
    out.append(std::to_string(snapshot.current_generation.value()));
    out.push_back(',');
    out.append(std::to_string(snapshot.revision.value()));
    out.push_back(',');
    out.append(std::to_string(snapshot.first_observed_at.nanos()));
    out.push_back(',');
    out.append(std::to_string(snapshot.last_observed_at.nanos()));
    out.push_back(',');
    out.append(std::to_string(throughput.duration_nanos));
    out.push_back(',');
    out.append(std::to_string(snapshot.total.bytes));
    out.push_back(',');
    out.append(std::to_string(snapshot.total.packets));
    out.push_back(',');
    out.append(std::to_string(snapshot.total.retransmissions));
    out.push_back(',');
    out.append(std::to_string(snapshot.total.unknown_intervals));
    out.push_back(',');
    out.append(yes_no(snapshot.total.is_exact()));
    out.push_back(',');
    out.append(std::to_string(throughput.bytes_per_second_milli));
    out.push_back(',');
    out.append(yes_no(snapshot.restored_from_journal));
    out.push_back('\n');
  }
  return out;
}

std::string render_text(const QueryResult& result) {
  std::string out;
  out.reserve(result.flows.size() * 160u);
  for (const FlowSnapshot& snapshot : result.flows) {
    out.append(snapshot.one_line());
    out.push_back('\n');
  }
  return out;
}

}  // namespace detail
}  // namespace flowobs
