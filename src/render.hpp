// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Internal rendering surface shared by the engine, the CLI and the transport.

#ifndef FLOWOBS_SRC_RENDER_HPP
#define FLOWOBS_SRC_RENDER_HPP

#include <string>
#include <vector>

#include "flowobs/engine.hpp"
#include "flowobs/export_json.hpp"
#include "flowobs/flow.hpp"
#include "flowobs/limits.hpp"

namespace flowobs {
namespace detail {

// Exact integer throughput in milli-bytes per second. Never floating point, so
// the value is reproducible on every platform.
struct Throughput final {
  std::uint64_t duration_nanos = 0;
  std::uint64_t bytes_per_second_milli = 0;
  std::uint64_t packets_per_second_milli = 0;
  bool duration_known = false;
  bool saturated = false;
};

Throughput compute_throughput(Timestamp first, Timestamp last, const Consumption& consumption);

void write_consumption(json::Writer& writer, const Consumption& consumption,
                       std::string_view name);
void write_flow_json(json::Writer& writer, const FlowSnapshot& snapshot,
                     const ExportOptions& options);
void write_generation_json(json::Writer& writer, const GenerationView& generation,
                           const ExportOptions& options);
void write_source_json(json::Writer& writer, const SourceView& source);
void write_anomaly_json(json::Writer& writer, const Anomaly& anomaly);
void write_query_result_json(json::Writer& writer, const QueryResult& result,
                             const ExportOptions& options);

// Builds the deterministic explanation of one flow.
Explanation build_explanation(const FlowSnapshot& snapshot, Timestamp evaluated_at,
                              bool journal_open, RuntimeEpoch current_epoch);

// Renders a query result as CSV with a fixed column order.
std::string render_csv(const QueryResult& result);
// Renders a query result as one canonical line per flow.
std::string render_text(const QueryResult& result);

}  // namespace detail
}  // namespace flowobs

#endif  // FLOWOBS_SRC_RENDER_HPP
