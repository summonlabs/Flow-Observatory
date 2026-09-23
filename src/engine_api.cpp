// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Public entry points of the runtime. Every mutating call takes the engine lock
// exclusively; every read takes it shared. The lock-order audit in
// docs/concurrency.md covers this file.

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "engine_impl.hpp"
#include "lockorder.hpp"
#include "flowobs/export_json.hpp"
#include "flowobs/textproto.hpp"
#include "render.hpp"

namespace flowobs {

namespace {

bool flow_matches(const FlowSnapshot& snapshot, const FlowQuery& query) {
  if (query.state.has_value() && snapshot.state != *query.state) {
    return false;
  }
  if (query.generation.has_value()) {
    bool found = false;
    for (const GenerationView& generation : snapshot.generations) {
      if (generation.generation.generation() == *query.generation) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  if (query.source.has_value()) {
    bool found = false;
    for (const GenerationView& generation : snapshot.generations) {
      for (const SourceView& source : generation.sources) {
        if (source.source == *query.source) {
          found = true;
          break;
        }
      }
      if (found) break;
    }
    if (!found) return false;
  }
  if (query.endpoint.has_value() && snapshot.endpoint_a != *query.endpoint &&
      snapshot.endpoint_b != *query.endpoint) {
    return false;
  }
  if (query.path.has_value()) {
    bool found = false;
    for (const GenerationView& generation : snapshot.generations) {
      if (generation.path.has_value() && *generation.path == *query.path) {
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  if (query.queue.has_value()) {
    bool found = false;
    for (const GenerationView& generation : snapshot.generations) {
      for (const QueueId queue : generation.queues) {
        if (queue == *query.queue) {
          found = true;
          break;
        }
      }
      if (found) break;
    }
    if (!found) return false;
  }
  if (!query.include_terminal && is_terminal(snapshot.state)) {
    return false;
  }
  if (query.only_live && !snapshot.is_live()) {
    return false;
  }
  if (query.only_restored && !snapshot.restored_from_journal) {
    return false;
  }
  if (query.since.has_value() && snapshot.last_observed_at.known() &&
      snapshot.last_observed_at < *query.since) {
    return false;
  }
  return true;
}

}  // namespace

Engine::Engine() : impl_(std::make_unique<Impl>()) {}

Engine::Engine(EngineConfig config) : impl_(std::make_unique<Impl>()) {
  impl_->config = std::move(config);
}

Engine::~Engine() {
  if (impl_ != nullptr) {
    const Status closed = close();
    (void)closed;
  }
}

const EngineConfig& Engine::config() const noexcept { return impl_->config; }
const Limits& Engine::limits() const noexcept { return impl_->config.limits; }

Timestamp Engine::now() const {
  detail::EngineSharedGuard lock(impl_->mutex);
  return impl_->now();
}

RuntimeEpoch Engine::runtime_epoch() const {
  detail::EngineSharedGuard lock(impl_->mutex);
  return impl_->epoch;
}

bool Engine::is_open() const noexcept {
  detail::EngineSharedGuard lock(impl_->mutex);
  return impl_->open;
}

bool Engine::frozen() const noexcept { return impl_->frozen.load(std::memory_order_relaxed); }

void Engine::freeze() noexcept { impl_->frozen.store(true, std::memory_order_relaxed); }

Status Engine::open() {
  const Status valid = impl_->config.validate();
  if (valid.failed()) {
    return valid;
  }
  detail::EngineMutexGuard lock(impl_->mutex);
  if (impl_->open) {
    return Status(ErrorCode::AlreadyOpen, "engine is already open");
  }
  impl_->clock = impl_->config.clock != nullptr ? impl_->config.clock : &impl_->default_clock;
  const Status journaled = impl_->open_journal_locked();
  if (journaled.failed()) {
    return journaled;
  }
  const Status workers = impl_->start_workers_locked();
  if (workers.failed()) {
    return workers;
  }
  impl_->frozen.store(false, std::memory_order_relaxed);
  impl_->open = true;
  // Settle restored state at the current instant before serving anything.
  const TickReport settled = impl_->tick_locked(impl_->now());
  (void)settled;
  return make_ok();
}

Status Engine::close() {
  if (impl_ == nullptr) {
    return make_ok();
  }
  {
    detail::EngineMutexGuard lock(impl_->mutex);
    if (!impl_->open && impl_->journal == nullptr) {
      return make_ok();
    }
    impl_->open = false;
  }
  // Joining workers must not happen under the engine mutex: a worker may be
  // blocked waiting to acquire it. This is the one place where the runtime
  // deliberately drops the lock, and it is recorded in the lock-order audit.
  const Status stopped = impl_->stop_workers_locked();
  (void)stopped;
  detail::EngineMutexGuard lock(impl_->mutex);
  Status result = make_ok();
  if (impl_->journal != nullptr) {
    const Status flushed = impl_->journal->flush();
    const Status closed = impl_->journal->close();
    impl_->journal.reset();
    if (closed.failed()) {
      result = closed;
    } else if (flushed.failed()) {
      result = flushed;
    }
  }
  impl_->counters.shutdowns.fetch_add(1, std::memory_order_relaxed);
  impl_->flows.clear();
  impl_->sources.clear();
  impl_->endpoints.entries.clear();
  impl_->paths.entries.clear();
  impl_->links.entries.clear();
  impl_->queues.entries.clear();
  {
    detail::QueueGuard queue_lock(impl_->queue_mutex);
    impl_->queue.clear();
  }
  return result;
}

Status Engine::register_source(const SourceDescriptor& descriptor) {
  if (!descriptor.valid()) {
    return invalid_argument("source descriptor has an invalid identity");
  }
  if (descriptor.canonical_key.empty()) {
    return invalid_argument("source descriptor requires a canonical key");
  }
  if (descriptor.canonical_key.size() > impl_->config.limits.max_key_bytes) {
    return limit_exceeded("source key exceeds limits.max_key_bytes");
  }
  if (descriptor.id != source_id_from_key(descriptor.canonical_key)) {
    return invalid_argument("source identity does not match its canonical key");
  }
  detail::EngineMutexGuard lock(impl_->mutex);
  if (impl_->frozen.load(std::memory_order_relaxed)) {
    return shutting_down("engine is frozen");
  }
  const auto found = impl_->sources.find(descriptor.id);
  if (found != impl_->sources.end()) {
    if (found->second.descriptor.canonical_key != descriptor.canonical_key) {
      return Status(ErrorCode::EvidenceConflict,
                    "source identity collision: two keys map to the same source identity");
    }
    found->second.descriptor = descriptor;
    found->second.retired = false;
    return make_ok();
  }
  if (impl_->sources.size() >= impl_->config.limits.max_sources) {
    return limit_exceeded("source registry is full");
  }
  const Status journaled = impl_->append_source_locked(descriptor);
  if (journaled.failed()) {
    return journaled;
  }
  SourceState state;
  state.descriptor = descriptor;
  state.registered_at = impl_->now();
  impl_->sources.emplace(descriptor.id, std::move(state));
  return make_ok();
}

Result<SourceDescriptor> Engine::describe_source(SourceId id) const {
  detail::EngineSharedGuard lock(impl_->mutex);
  const auto found = impl_->sources.find(id);
  if (found == impl_->sources.end()) {
    return not_found("source is not registered");
  }
  return found->second.descriptor;
}

Status Engine::list_sources(std::vector<SourceDescriptor>& out) const {
  detail::EngineSharedGuard lock(impl_->mutex);
  out.clear();
  out.reserve(impl_->sources.size());
  for (const auto& [id, state] : impl_->sources) {
    (void)id;
    out.push_back(state.descriptor);
  }
  return make_ok();
}

Status Engine::retire_source(SourceId id) {
  detail::EngineMutexGuard lock(impl_->mutex);
  const auto found = impl_->sources.find(id);
  if (found == impl_->sources.end()) {
    return not_found("source is not registered");
  }
  found->second.retired = true;
  // Retiring a source removes the evidence it contributed: the runtime never
  // keeps accounting it can no longer attribute.
  for (auto& [flow_id, flow] : impl_->flows) {
    (void)flow_id;
    for (GenerationRuntime& generation : flow.generations) {
      generation.bindings.erase(
          std::remove_if(generation.bindings.begin(), generation.bindings.end(),
                         [id](const SourceBinding& binding) { return binding.source == id; }),
          generation.bindings.end());
    }
  }
  for (auto& [flow_id, flow] : impl_->flows) {
    (void)flow_id;
    impl_->recompute_flow(flow, impl_->now(), impl_->config.limits);
  }
  return make_ok();
}

Result<IngestOutcome> Engine::ingest(const Observation& observation) {
  detail::EngineMutexGuard lock(impl_->mutex);
  if (!impl_->open) {
    return not_open("engine is not open");
  }
  if (impl_->frozen.load(std::memory_order_relaxed)) {
    return shutting_down("engine is frozen");
  }
  return impl_->apply_locked(observation);
}

Result<IngestOutcome> Engine::submit(const Observation& observation) {
  Observation prepared = observation;
  {
    detail::EngineSharedGuard lock(impl_->mutex);
    if (impl_->frozen.load(std::memory_order_relaxed)) {
      return shutting_down("engine is frozen");
    }
    if (!prepared.received_at.known()) {
      prepared.received_at = impl_->now();
    }
    if (!prepared.subject.flow.valid() && !prepared.subject.flow_key.empty()) {
      prepared.subject.flow = flow_id_from_key(prepared.subject.flow_key);
    }
    const auto found = impl_->sources.find(prepared.key.source);
    if (found != impl_->sources.end() && prepared.capabilities.empty()) {
      prepared.capabilities = found->second.descriptor.capabilities;
    }
  }
  return ingest(prepared);
}

Result<IngestOutcome> Engine::enqueue(const Observation& observation) {
  IngestOutcome outcome;
  Observation prepared = observation;
  {
    detail::EngineSharedGuard lock(impl_->mutex);
    if (!impl_->open) {
      outcome.disposition = IngestDisposition::Refused;
      outcome.code = ErrorCode::NotOpen;
      outcome.detail = "engine is not open";
      return outcome;
    }
    if (impl_->frozen.load(std::memory_order_relaxed)) {
      outcome.disposition = IngestDisposition::Refused;
      outcome.code = ErrorCode::ShuttingDown;
      outcome.detail = "engine is frozen";
      return outcome;
    }
    if (!prepared.received_at.known()) {
      prepared.received_at = impl_->now();
    }
    if (!prepared.subject.flow.valid() && !prepared.subject.flow_key.empty()) {
      prepared.subject.flow = flow_id_from_key(prepared.subject.flow_key);
    }
  }
  {
    detail::QueueGuard queue_lock(impl_->queue_mutex);
    if (impl_->queue.size() >= impl_->config.limits.max_pending_observations) {
      impl_->counters.observations_dropped_queue_full.fetch_add(1, std::memory_order_relaxed);
      outcome.disposition = IngestDisposition::DroppedQueueFull;
      outcome.code = ErrorCode::LimitExceeded;
      outcome.detail = "ingest queue is full; the observation was refused, not discarded";
      return outcome;
    }
    impl_->queue.push_back(std::move(prepared));
  }
  impl_->queue_cv.notify_one();
  outcome.disposition = IngestDisposition::Queued;
  return outcome;
}

Status Engine::start_workers() {
  detail::EngineMutexGuard lock(impl_->mutex);
  if (!impl_->open) {
    return not_open("engine is not open");
  }
  return impl_->start_workers_locked();
}

Status Engine::stop_workers() {
  {
    detail::EngineMutexGuard lock(impl_->mutex);
    if (!impl_->workers_running) {
      return make_ok();
    }
  }
  return impl_->stop_workers_locked();
}

std::size_t Engine::queue_depth() const {
  detail::QueueGuard queue_lock(impl_->queue_mutex);
  return impl_->queue.size();
}

Status Engine::drain(std::size_t max_observations) {
  std::vector<Observation> batch;
  {
    detail::QueueGuard queue_lock(impl_->queue_mutex);
    std::size_t take = max_observations;
    if (take > impl_->queue.size()) {
      take = impl_->queue.size();
    }
    batch.reserve(take);
    for (std::size_t index = 0; index < take; ++index) {
      batch.push_back(std::move(impl_->queue.front()));
      impl_->queue.pop_front();
    }
  }
  impl_->queue_space_cv.notify_all();
  if (batch.empty()) {
    return make_ok();
  }
  detail::EngineMutexGuard lock(impl_->mutex);
  if (!impl_->open) {
    return not_open("engine is not open");
  }
  for (const Observation& observation : batch) {
    const Result<IngestOutcome> applied = impl_->apply_locked(observation);
    (void)applied;
  }
  impl_->counters.worker_batches.fetch_add(1, std::memory_order_relaxed);
  return make_ok();
}

Result<QueryResult> Engine::query(const FlowQuery& query_spec) const {
  const Status valid = query_spec.validate(impl_->config.query);
  if (valid.failed()) {
    return valid;
  }
  detail::EngineSharedGuard lock(impl_->mutex);
  const FreshnessContext ctx = impl_->context();
  QueryResult result;
  result.evaluated_at = ctx.now;
  impl_->counters.queries_served.fetch_add(1, std::memory_order_relaxed);

  if (query_spec.flow.has_value()) {
    const auto found = impl_->flows.find(*query_spec.flow);
    if (found == impl_->flows.end()) {
      return result;
    }
    FlowSnapshot snapshot = impl_->snapshot_of(found->second, ctx, impl_->config);
    if (flow_matches(snapshot, query_spec)) {
      result.matched = 1;
      result.returned = 1;
      result.flows.push_back(std::move(snapshot));
    }
    return result;
  }

  for (const auto& [id, flow] : impl_->flows) {
    (void)id;
    FlowSnapshot snapshot = impl_->snapshot_of(flow, ctx, impl_->config);
    if (!flow_matches(snapshot, query_spec)) {
      continue;
    }
    ++result.matched;
    if (result.matched <= query_spec.offset) {
      continue;
    }
    if (result.flows.size() >= query_spec.limit) {
      result.truncated = true;
      continue;
    }
    result.flows.push_back(std::move(snapshot));
  }
  result.returned = result.flows.size();
  return result;
}

Result<FlowSnapshot> Engine::snapshot(FlowId flow) const {
  detail::EngineSharedGuard lock(impl_->mutex);
  const auto found = impl_->flows.find(flow);
  if (found == impl_->flows.end()) {
    return not_found("flow is not known to this runtime");
  }
  return impl_->snapshot_of(found->second, impl_->context(), impl_->config);
}

Result<HistoryReport> Engine::history(FlowId flow, std::size_t limit) const {
  detail::EngineSharedGuard lock(impl_->mutex);
  const auto found = impl_->flows.find(flow);
  if (found == impl_->flows.end()) {
    return not_found("flow is not known to this runtime");
  }
  HistoryReport report;
  report.flow = flow;
  report.events_dropped = found->second.history.dropped();
  const std::size_t max_rows = impl_->config.limits.max_result_set;
  const std::size_t bounded = limit == 0 ? max_rows : (limit > max_rows ? max_rows : limit);
  report.events = found->second.history.tail(bounded);
  report.returned = report.events.size();
  report.truncated = found->second.history.size() > report.returned;
  return report;
}

Result<Explanation> Engine::explain(FlowId flow) const {
  detail::EngineSharedGuard lock(impl_->mutex);
  const auto found = impl_->flows.find(flow);
  if (found == impl_->flows.end()) {
    return not_found("flow is not known to this runtime");
  }
  const FreshnessContext ctx = impl_->context();
  const FlowSnapshot snapshot = impl_->snapshot_of(found->second, ctx, impl_->config);
  impl_->counters.explanations_served.fetch_add(1, std::memory_order_relaxed);
  return detail::build_explanation(snapshot, ctx.now, impl_->journal != nullptr, impl_->epoch);
}

Result<AttributionReport> Engine::attribute(FlowId flow, GenerationId generation,
                                            const AttributionPolicy& policy) const {
  const Status valid = policy.validate();
  if (valid.failed()) {
    return valid;
  }
  detail::EngineSharedGuard lock(impl_->mutex);
  const auto found = impl_->flows.find(flow);
  if (found == impl_->flows.end()) {
    return not_found("flow is not known to this runtime");
  }
  impl_->counters.attributions_served.fetch_add(1, std::memory_order_relaxed);
  const FreshnessContext ctx = impl_->context();
  const FlowSnapshot snapshot = impl_->snapshot_of(found->second, ctx, impl_->config);
  const GenerationView* view = nullptr;
  for (const GenerationView& candidate : snapshot.generations) {
    if (candidate.generation.generation() == generation) {
      view = &candidate;
      break;
    }
  }
  if (view == nullptr) {
    AttributionReport report;
    report.flow = flow;
    report.generation = generation;
    report.status = AttributionStatus::GenerationNotFound;
    report.status_detail = "the flow has no record of that generation";
    return report;
  }
  return attribute_generation(snapshot, *view, policy, impl_->config.limits.max_result_set);
}

Result<AttributionReport> Engine::attribute_current(FlowId flow) const {
  GenerationId generation{};
  {
    detail::EngineSharedGuard lock(impl_->mutex);
    const auto found = impl_->flows.find(flow);
    if (found == impl_->flows.end()) {
      return not_found("flow is not known to this runtime");
    }
    generation = found->second.current_generation;
  }
  return attribute(flow, generation, impl_->config.attribution);
}

Result<ExportResult> Engine::export_data(const ExportOptions& options) const {
  const Status valid = options.validate(impl_->config.limits, impl_->config.query);
  if (valid.failed()) {
    return valid;
  }
  ExportResult out;
  out.format = options.format;
  detail::EngineSharedGuard lock(impl_->mutex);
  const FreshnessContext ctx = impl_->context();
  QueryResult result;
  result.evaluated_at = options.evaluated_at.has_value() ? *options.evaluated_at : ctx.now;
  for (const auto& [id, flow] : impl_->flows) {
    (void)id;
    FlowSnapshot snapshot = impl_->snapshot_of(flow, ctx, impl_->config);
    if (!flow_matches(snapshot, options.query)) {
      continue;
    }
    ++result.matched;
    if (result.matched <= options.query.offset) {
      continue;
    }
    if (result.flows.size() >= options.query.limit) {
      result.truncated = true;
      continue;
    }
    result.flows.push_back(std::move(snapshot));
  }
  result.returned = result.flows.size();
  switch (options.format) {
    case ExportOptions::Format::Json: {
      json::Writer writer(options.pretty);
      detail::write_query_result_json(writer, result, options);
      out.text = writer.take();
      out.text.push_back('\n');
      break;
    }
    case ExportOptions::Format::Csv:
      out.text = detail::render_csv(result);
      break;
    case ExportOptions::Format::Text:
      out.text = detail::render_text(result);
      break;
  }
  out.rows = result.returned;
  out.truncated = result.truncated;
  impl_->counters.exports_served.fetch_add(1, std::memory_order_relaxed);
  return out;
}

Result<TickReport> Engine::tick(Timestamp evaluated_at) {
  if (!evaluated_at.known()) {
    return invalid_argument("tick requires a known evaluation instant");
  }
  detail::EngineMutexGuard lock(impl_->mutex);
  if (!impl_->open) {
    return not_open("engine is not open");
  }
  return impl_->tick_locked(evaluated_at);
}

Status Engine::flush() {
  detail::EngineMutexGuard lock(impl_->mutex);
  if (impl_->journal == nullptr) {
    return make_ok();
  }
  return impl_->journal->flush();
}

Status Engine::compact() {
  detail::EngineMutexGuard lock(impl_->mutex);
  if (impl_->journal == nullptr) {
    return make_ok();
  }
  return impl_->compact_locked();
}

Status Engine::verify_journal() const {
  detail::EngineSharedGuard lock(impl_->mutex);
  if (impl_->journal == nullptr) {
    return not_found("no journal is configured for this engine");
  }
  return impl_->journal->verify();
}

Status Engine::save_snapshot(const std::filesystem::path& path) const {
  detail::EngineSharedGuard lock(impl_->mutex);
  const PersistedState state = impl_->persist_state();
  std::vector<journal::Record> records;
  Status status = encode_state(state, impl_->config.limits, records);
  if (status.failed()) {
    return status;
  }
  JournalOpenOptions options;
  options.limits = impl_->config.limits;
  options.recovery = impl_->config.recovery;
  options.create_if_missing = true;
  options.truncate_existing = true;
  Result<std::unique_ptr<Journal>> opened = Journal::open(path, options);
  if (!opened.ok()) {
    return opened.status();
  }
  std::unique_ptr<Journal> file = std::move(opened.value());
  file->set_runtime_epoch(state.runtime_epoch);
  file->set_created_at(state.created_at);
  status = file->append(records);
  if (status.failed()) {
    return status;
  }
  status = file->flush();
  if (status.failed()) {
    return status;
  }
  return file->close();
}

Status Engine::load_snapshot(const std::filesystem::path& path) {
  JournalOpenOptions options;
  options.limits = impl_->config.limits;
  options.recovery = impl_->config.recovery;
  options.read_only = true;
  options.create_if_missing = false;
  Result<std::unique_ptr<Journal>> opened = Journal::open(path, options);
  if (!opened.ok()) {
    return opened.status();
  }
  std::unique_ptr<Journal> file = std::move(opened.value());
  Result<std::vector<journal::Record>> records = file->read_all();
  if (!records.ok()) {
    return records.status();
  }
  const Status closed = file->close();
  if (closed.failed()) {
    return closed;
  }
  detail::EngineMutexGuard lock(impl_->mutex);
  impl_->flows.clear();
  impl_->endpoints.entries.clear();
  impl_->paths.entries.clear();
  impl_->links.entries.clear();
  impl_->queues.entries.clear();
  return impl_->restore_locked(records.value(), impl_->epoch);
}

EngineStats Engine::stats() const {
  detail::EngineSharedGuard lock(impl_->mutex);
  EngineStats stats = EngineStats::capture(impl_->counters);
  stats.total_flows = impl_->flows.size();
  stats.runtime_epoch = impl_->epoch;
  for (const auto& [id, flow] : impl_->flows) {
    (void)id;
    if (flow.state == FlowState::Active) {
      ++stats.live_flows;
    }
  }
  stats.queue_capacity = impl_->config.limits.max_pending_observations;
  stats.journal_open = impl_->journal != nullptr;
  if (impl_->journal != nullptr) {
    stats.journal_file_bytes = impl_->journal->stats().file_size;
  }
  {
    detail::QueueGuard queue_lock(impl_->queue_mutex);
    stats.queue_depth = impl_->queue.size();
  }
  return stats;
}

std::uint64_t Engine::state_digest() const {
  detail::EngineSharedGuard lock(impl_->mutex);
  return digest_state(impl_->persist_view_state());
}

}  // namespace flowobs
