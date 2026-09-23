// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowobs/stats.hpp"

#include <string>

namespace flowobs {

namespace {

void absorb(std::atomic<std::uint64_t>& target, const std::atomic<std::uint64_t>& source) {
  target.fetch_add(source.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

std::uint64_t load(const std::atomic<std::uint64_t>& value) {
  return value.load(std::memory_order_relaxed);
}

}  // namespace

void EngineCounters::add(EngineCounters& other) noexcept {
  absorb(observations_received, other.observations_received);
  absorb(observations_applied, other.observations_applied);
  absorb(observations_refused, other.observations_refused);
  absorb(observations_duplicate, other.observations_duplicate);
  absorb(observations_conflicting, other.observations_conflicting);
  absorb(observations_dropped_queue_full, other.observations_dropped_queue_full);
  absorb(observations_requeued, other.observations_requeued);
  absorb(flows_created, other.flows_created);
  absorb(flows_evicted, other.flows_evicted);
  absorb(flows_expired, other.flows_expired);
  absorb(generations_created, other.generations_created);
  absorb(generations_evicted, other.generations_evicted);
  absorb(transitions, other.transitions);
  absorb(anomalies_recorded, other.anomalies_recorded);
  absorb(anomalies_dropped, other.anomalies_dropped);
  absorb(identity_collisions, other.identity_collisions);
  absorb(queries_served, other.queries_served);
  absorb(exports_served, other.exports_served);
  absorb(explanations_served, other.explanations_served);
  absorb(attributions_served, other.attributions_served);
  absorb(ticks, other.ticks);
  absorb(journal_records_appended, other.journal_records_appended);
  absorb(journal_bytes_appended, other.journal_bytes_appended);
  absorb(journal_compactions, other.journal_compactions);
  absorb(journal_failures, other.journal_failures);
  absorb(transport_frames_in, other.transport_frames_in);
  absorb(transport_frames_out, other.transport_frames_out);
  absorb(transport_frames_rejected, other.transport_frames_rejected);
  absorb(transport_connections, other.transport_connections);
  absorb(worker_batches, other.worker_batches);
  absorb(shutdowns, other.shutdowns);
}

void EngineCounters::reset() noexcept {
  observations_received.store(0, std::memory_order_relaxed);
  observations_applied.store(0, std::memory_order_relaxed);
  observations_refused.store(0, std::memory_order_relaxed);
  observations_duplicate.store(0, std::memory_order_relaxed);
  observations_conflicting.store(0, std::memory_order_relaxed);
  observations_dropped_queue_full.store(0, std::memory_order_relaxed);
  observations_requeued.store(0, std::memory_order_relaxed);
  flows_created.store(0, std::memory_order_relaxed);
  flows_evicted.store(0, std::memory_order_relaxed);
  flows_expired.store(0, std::memory_order_relaxed);
  generations_created.store(0, std::memory_order_relaxed);
  generations_evicted.store(0, std::memory_order_relaxed);
  transitions.store(0, std::memory_order_relaxed);
  anomalies_recorded.store(0, std::memory_order_relaxed);
  anomalies_dropped.store(0, std::memory_order_relaxed);
  identity_collisions.store(0, std::memory_order_relaxed);
  queries_served.store(0, std::memory_order_relaxed);
  exports_served.store(0, std::memory_order_relaxed);
  explanations_served.store(0, std::memory_order_relaxed);
  attributions_served.store(0, std::memory_order_relaxed);
  ticks.store(0, std::memory_order_relaxed);
  journal_records_appended.store(0, std::memory_order_relaxed);
  journal_bytes_appended.store(0, std::memory_order_relaxed);
  journal_compactions.store(0, std::memory_order_relaxed);
  journal_failures.store(0, std::memory_order_relaxed);
  transport_frames_in.store(0, std::memory_order_relaxed);
  transport_frames_out.store(0, std::memory_order_relaxed);
  transport_frames_rejected.store(0, std::memory_order_relaxed);
  transport_connections.store(0, std::memory_order_relaxed);
  worker_batches.store(0, std::memory_order_relaxed);
  shutdowns.store(0, std::memory_order_relaxed);
}

EngineStats EngineStats::capture(const EngineCounters& counters) {
  EngineStats stats;
  stats.observations_received = load(counters.observations_received);
  stats.observations_applied = load(counters.observations_applied);
  stats.observations_refused = load(counters.observations_refused);
  stats.observations_duplicate = load(counters.observations_duplicate);
  stats.observations_conflicting = load(counters.observations_conflicting);
  stats.observations_dropped_queue_full = load(counters.observations_dropped_queue_full);
  stats.observations_requeued = load(counters.observations_requeued);
  stats.flows_created = load(counters.flows_created);
  stats.flows_evicted = load(counters.flows_evicted);
  stats.flows_expired = load(counters.flows_expired);
  stats.generations_created = load(counters.generations_created);
  stats.generations_evicted = load(counters.generations_evicted);
  stats.transitions = load(counters.transitions);
  stats.anomalies_recorded = load(counters.anomalies_recorded);
  stats.anomalies_dropped = load(counters.anomalies_dropped);
  stats.identity_collisions = load(counters.identity_collisions);
  stats.queries_served = load(counters.queries_served);
  stats.exports_served = load(counters.exports_served);
  stats.explanations_served = load(counters.explanations_served);
  stats.attributions_served = load(counters.attributions_served);
  stats.ticks = load(counters.ticks);
  stats.journal_records_appended = load(counters.journal_records_appended);
  stats.journal_bytes_appended = load(counters.journal_bytes_appended);
  stats.journal_compactions = load(counters.journal_compactions);
  stats.journal_failures = load(counters.journal_failures);
  stats.transport_frames_in = load(counters.transport_frames_in);
  stats.transport_frames_out = load(counters.transport_frames_out);
  stats.transport_frames_rejected = load(counters.transport_frames_rejected);
  stats.transport_connections = load(counters.transport_connections);
  stats.worker_batches = load(counters.worker_batches);
  stats.shutdowns = load(counters.shutdowns);
  return stats;
}

std::string EngineStats::describe() const {
  std::string out;
  out.reserve(768);
  const auto line = [&out](const char* key, std::uint64_t value) {
    out.append(key);
    out.push_back('=');
    out.append(std::to_string(value));
    out.push_back('\n');
  };
  line("observations_received", observations_received);
  line("observations_applied", observations_applied);
  line("observations_refused", observations_refused);
  line("observations_duplicate", observations_duplicate);
  line("observations_conflicting", observations_conflicting);
  line("observations_dropped_queue_full", observations_dropped_queue_full);
  line("observations_requeued", observations_requeued);
  line("flows_created", flows_created);
  line("flows_evicted", flows_evicted);
  line("flows_expired", flows_expired);
  line("generations_created", generations_created);
  line("generations_evicted", generations_evicted);
  line("transitions", transitions);
  line("anomalies_recorded", anomalies_recorded);
  line("anomalies_dropped", anomalies_dropped);
  line("identity_collisions", identity_collisions);
  line("queries_served", queries_served);
  line("exports_served", exports_served);
  line("explanations_served", explanations_served);
  line("attributions_served", attributions_served);
  line("ticks", ticks);
  line("journal_records_appended", journal_records_appended);
  line("journal_bytes_appended", journal_bytes_appended);
  line("journal_compactions", journal_compactions);
  line("journal_failures", journal_failures);
  line("transport_frames_in", transport_frames_in);
  line("transport_frames_out", transport_frames_out);
  line("transport_frames_rejected", transport_frames_rejected);
  line("transport_connections", transport_connections);
  line("worker_batches", worker_batches);
  line("shutdowns", shutdowns);
  line("live_flows", live_flows);
  line("total_flows", total_flows);
  line("queue_depth", queue_depth);
  line("queue_capacity", queue_capacity);
  line("journal_file_bytes", journal_file_bytes);
  out.append("journal_open=");
  out.append(journal_open ? "true" : "false");
  out.push_back('\n');
  line("runtime_epoch", runtime_epoch.value());
  return out;
}

}  // namespace flowobs
