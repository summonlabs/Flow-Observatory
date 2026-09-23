// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowobs/limits.hpp"

#include <string>

namespace flowobs {

namespace {
Status check(bool condition, const char* message) {
  if (condition) {
    return make_ok();
  }
  return Status(ErrorCode::OutOfRange, message);
}
}  // namespace

Status Limits::validate() const {
  Status s = check(max_flows > 0, "limits.max_flows must be greater than zero");
  if (s.failed()) return s;
  s = check(max_flows <= 100000000u, "limits.max_flows exceeds the supported maximum");
  if (s.failed()) return s;
  s = check(max_sources > 0, "limits.max_sources must be greater than zero");
  if (s.failed()) return s;
  s = check(max_sources <= 1000000u, "limits.max_sources exceeds the supported maximum");
  if (s.failed()) return s;
  s = check(max_generations_per_flow > 0,
            "limits.max_generations_per_flow must be greater than zero");
  if (s.failed()) return s;
  s = check(max_generations_per_flow <= 4096u,
            "limits.max_generations_per_flow exceeds the supported maximum");
  if (s.failed()) return s;
  s = check(max_paths_per_flow > 0, "limits.max_paths_per_flow must be greater than zero");
  if (s.failed()) return s;
  s = check(max_paths_per_flow <= 4096u, "limits.max_paths_per_flow exceeds the supported maximum");
  if (s.failed()) return s;
  s = check(max_queues_per_flow > 0, "limits.max_queues_per_flow must be greater than zero");
  if (s.failed()) return s;
  s = check(max_queues_per_flow <= 65536u, "limits.max_queues_per_flow exceeds the supported maximum");
  if (s.failed()) return s;
  s = check(max_path_hops > 0, "limits.max_path_hops must be greater than zero");
  if (s.failed()) return s;
  s = check(max_path_hops <= 4096u, "limits.max_path_hops exceeds the supported maximum");
  if (s.failed()) return s;
  s = check(max_observations_per_source_flow > 0,
            "limits.max_observations_per_source_flow must be greater than zero");
  if (s.failed()) return s;
  s = check(max_observations_per_source_flow <= 65536u,
            "limits.max_observations_per_source_flow exceeds the supported maximum");
  if (s.failed()) return s;
  s = check(max_history_per_flow > 0, "limits.max_history_per_flow must be greater than zero");
  if (s.failed()) return s;
  s = check(max_history_per_flow <= 65536u,
            "limits.max_history_per_flow exceeds the supported maximum");
  if (s.failed()) return s;
  s = check(max_history_flows > 0, "limits.max_history_flows must be greater than zero");
  if (s.failed()) return s;
  s = check(max_history_flows <= 100000000u,
            "limits.max_history_flows exceeds the supported maximum");
  if (s.failed()) return s;
  s = check(max_anomalies_per_flow > 0, "limits.max_anomalies_per_flow must be greater than zero");
  if (s.failed()) return s;
  s = check(max_anomalies_per_flow <= 65536u,
            "limits.max_anomalies_per_flow exceeds the supported maximum");
  if (s.failed()) return s;
  s = check(max_sources_per_flow > 0, "limits.max_sources_per_flow must be greater than zero");
  if (s.failed()) return s;
  s = check(max_sources_per_flow <= 65536u,
            "limits.max_sources_per_flow exceeds the supported maximum");
  if (s.failed()) return s;
  s = check(max_pending_observations > 0,
            "limits.max_pending_observations must be greater than zero");
  if (s.failed()) return s;
  s = check(max_pending_observations <= (1ull << 32),
            "limits.max_pending_observations exceeds the supported maximum");
  if (s.failed()) return s;
  s = check(max_ingest_batch > 0, "limits.max_ingest_batch must be greater than zero");
  if (s.failed()) return s;
  s = check(max_ingest_batch <= max_pending_observations,
            "limits.max_ingest_batch must not exceed limits.max_pending_observations");
  if (s.failed()) return s;
  s = check(worker_threads <= 1024u, "limits.worker_threads exceeds the supported maximum");
  if (s.failed()) return s;
  s = check(max_result_set > 0, "limits.max_result_set must be greater than zero");
  if (s.failed()) return s;
  s = check(max_result_set <= 100000000ull, "limits.max_result_set exceeds the supported maximum");
  if (s.failed()) return s;
  s = check(max_journal_bytes > 4096ull, "limits.max_journal_bytes must exceed 4096");
  if (s.failed()) return s;
  s = check(max_key_bytes > 0, "limits.max_key_bytes must be greater than zero");
  if (s.failed()) return s;
  s = check(max_key_bytes <= 65536u, "limits.max_key_bytes exceeds the supported maximum");
  if (s.failed()) return s;
  s = check(max_detail_bytes > 0, "limits.max_detail_bytes must be greater than zero");
  if (s.failed()) return s;
  s = check(max_detail_bytes <= 65536u, "limits.max_detail_bytes exceeds the supported maximum");
  if (s.failed()) return s;
  s = check(max_frame_bytes >= 64u, "limits.max_frame_bytes must be at least 64");
  if (s.failed()) return s;
  s = check(max_frame_bytes <= (64ull << 20), "limits.max_frame_bytes exceeds the supported maximum");
  if (s.failed()) return s;
  s = check(max_connections > 0, "limits.max_connections must be greater than zero");
  if (s.failed()) return s;
  s = check(max_connections <= 4096u, "limits.max_connections exceeds the supported maximum");
  if (s.failed()) return s;
  s = check(max_aggregation_window.nanos() > 0,
            "limits.max_aggregation_window must be positive");
  if (s.failed()) return s;
  s = check(max_clock_skew.nanos() >= 0, "limits.max_clock_skew must not be negative");
  if (s.failed()) return s;
  s = check(max_ingest_records > 0, "limits.max_ingest_records must be greater than zero");
  if (s.failed()) return s;
  return make_ok();
}

std::string Limits::describe() const {
  std::string out;
  out.reserve(512);
  const auto line = [&out](const char* key, std::uint64_t value) {
    out.append(key);
    out.push_back('=');
    out.append(std::to_string(value));
    out.push_back('\n');
  };
  line("max_flows", max_flows);
  line("max_sources", max_sources);
  line("max_generations_per_flow", max_generations_per_flow);
  line("max_paths_per_flow", max_paths_per_flow);
  line("max_queues_per_flow", max_queues_per_flow);
  line("max_path_hops", max_path_hops);
  line("max_observations_per_source_flow", max_observations_per_source_flow);
  line("max_history_per_flow", max_history_per_flow);
  line("max_history_flows", max_history_flows);
  line("max_anomalies_per_flow", max_anomalies_per_flow);
  line("max_sources_per_flow", max_sources_per_flow);
  line("max_pending_observations", max_pending_observations);
  line("max_ingest_batch", max_ingest_batch);
  line("worker_threads", worker_threads);
  line("max_result_set", max_result_set);
  line("max_journal_bytes", max_journal_bytes);
  line("max_key_bytes", max_key_bytes);
  line("max_detail_bytes", max_detail_bytes);
  line("max_frame_bytes", max_frame_bytes);
  line("max_connections", max_connections);
  line("max_aggregation_window_ns", static_cast<std::uint64_t>(max_aggregation_window.nanos()));
  line("max_clock_skew_ns", static_cast<std::uint64_t>(max_clock_skew.nanos()));
  line("max_ingest_records", max_ingest_records);
  return out;
}

}  // namespace flowobs
