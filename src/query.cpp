// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "flowobs/engine.hpp"
#include "flowobs/version.hpp"

namespace flowobs {

std::string_view to_string(IngestDisposition disposition) noexcept {
  switch (disposition) {
    case IngestDisposition::Applied: return "applied";
    case IngestDisposition::Duplicate: return "duplicate";
    case IngestDisposition::Refused: return "refused";
    case IngestDisposition::Queued: return "queued";
    case IngestDisposition::DroppedQueueFull: return "dropped-queue-full";
  }
  return "invalid";
}

std::string IngestOutcome::describe() const {
  std::string out;
  out.reserve(160);
  out.append(to_string(disposition));
  if (code != ErrorCode::Ok) {
    out.append(" (");
    out.append(flowobs::to_string(code));
    if (!detail.empty()) {
      out.append(": ");
      out.append(detail);
    }
    out.push_back(')');
  } else if (!detail.empty()) {
    out.append(" (");
    out.append(detail);
    out.push_back(')');
  }
  if (generation.valid()) {
    out.append(" ");
    out.append(generation.to_string());
  }
  if (state_before != state_after) {
    out.append(" ");
    out.append(flowobs::to_string(state_before));
    out.append("->");
    out.append(flowobs::to_string(state_after));
  }
  return out;
}

Status FlowQuery::validate(const QueryPolicy& policy) const {
  if (limit == 0) {
    return invalid_argument("query.limit must be greater than zero");
  }
  if (limit > policy.max_rows) {
    return limit_exceeded("query.limit exceeds query.max_rows");
  }
  if (offset > 100000000ull) {
    return out_of_range_error("query.offset exceeds the supported maximum");
  }
  if (flow.has_value() && !flow->valid()) {
    return invalid_argument("query.flow is the invalid identity");
  }
  if (source.has_value() && !source->valid()) {
    return invalid_argument("query.source is the invalid identity");
  }
  if (generation.has_value() && !generation->valid()) {
    return invalid_argument("query.generation is the invalid identity");
  }
  if (state.has_value() && static_cast<std::uint8_t>(*state) >= kFlowStateCount) {
    return invalid_argument("query.state is not a known lifecycle state");
  }
  if (since.has_value() && !since->known()) {
    return invalid_argument("query.since must be a known instant");
  }
  return make_ok();
}

std::string QueryResult::describe() const {
  std::string out;
  out.reserve(128);
  out.append("matched=");
  out.append(std::to_string(matched));
  out.append(" returned=");
  out.append(std::to_string(returned));
  out.append(truncated ? " truncated=true" : " truncated=false");
  out.append(" evaluated_at=");
  out.append(std::to_string(evaluated_at.nanos()));
  return out;
}

std::string HistoryReport::describe() const {
  std::string out;
  out.reserve(128);
  out.append("history ");
  out.append(flowobs::to_string(flow));
  out.append(" returned=");
  out.append(std::to_string(returned));
  out.append(" dropped=");
  out.append(std::to_string(events_dropped));
  out.append(truncated ? " truncated=true" : " truncated=false");
  return out;
}

std::string ExportResult::describe() const {
  std::string out;
  out.reserve(96);
  out.append("export rows=");
  out.append(std::to_string(rows));
  out.append(" bytes=");
  out.append(std::to_string(text.size()));
  out.append(truncated ? " truncated=true" : " truncated=false");
  return out;
}

Status ExportOptions::validate(const Limits& limits, const QueryPolicy& policy) const {
  Status s = query.validate(policy);
  if (s.failed()) {
    return s;
  }
  if (query.limit > limits.max_result_set) {
    return limit_exceeded("export row limit exceeds limits.max_result_set");
  }
  switch (format) {
    case Format::Json:
    case Format::Csv:
    case Format::Text:
      break;
    default:
      return invalid_argument("export format is not a known format");
  }
  if (evaluated_at.has_value() && !evaluated_at->known()) {
    return invalid_argument("export.evaluated_at must be a known instant");
  }
  return make_ok();
}

}  // namespace flowobs
