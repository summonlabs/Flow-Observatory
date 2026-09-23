// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Runtime internals: configuration validation, the pure view builders, the
// ingest pipeline and journal recovery. The public entry points live in
// engine_api.cpp.

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "flowobs/checksum.hpp"
#include "flowobs/reconcile.hpp"
#include "engine_impl.hpp"
#include "lockorder.hpp"
#include "obs_codec.hpp"
#include "render.hpp"

namespace flowobs {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

Status EngineConfig::validate() const {
  Status s = limits.validate();
  if (s.failed()) return s;
  s = freshness.validate();
  if (s.failed()) return s;
  s = expiry.validate();
  if (s.failed()) return s;
  s = counters.validate();
  if (s.failed()) return s;
  s = reconciliation.validate();
  if (s.failed()) return s;
  s = attribution.validate();
  if (s.failed()) return s;
  s = recovery.validate();
  if (s.failed()) return s;
  s = query.validate();
  if (s.failed()) return s;
  if (query.max_rows > limits.max_result_set) {
    return out_of_range_error("query.max_rows must not exceed limits.max_result_set");
  }
  return make_ok();
}

std::uint64_t EngineConfig::policy_digest() const noexcept {
  // Only the counter policy participates in the durable fold; it is the policy
  // a snapshot must agree on, because a snapshot stores folded accounting.
  const std::string text = counters.describe();
  return fnv1a64_raw(text.data(), text.size());
}

std::string EngineConfig::describe() const {
  std::string out;
  out.reserve(1024);
  out.append("limits:\n");
  out.append(limits.describe());
  out.append("freshness: ");
  out.append(freshness.describe());
  out.push_back('\n');
  out.append("expiry: ");
  out.append(expiry.describe());
  out.push_back('\n');
  out.append("counters: ");
  out.append(counters.describe());
  out.push_back('\n');
  out.append("reconciliation: ");
  out.append(reconciliation.describe());
  out.push_back('\n');
  out.append("attribution: ");
  out.append(attribution.describe());
  out.push_back('\n');
  out.append("recovery: ");
  out.append(recovery.describe());
  out.push_back('\n');
  out.append("query: ");
  out.append(query.describe());
  out.push_back('\n');
  out.append("journal_path=");
  out.append(journal_path.generic_string());
  out.push_back('\n');
  out.append("restore_on_open=");
  out.append(restore_on_open ? "true" : "false");
  out.push_back('\n');
  out.append("policy_digest=");
  out.append(std::to_string(policy_digest()));
  out.push_back('\n');
  return out;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

FreshnessContext Engine::Impl::context() const {
  FreshnessContext ctx;
  ctx.now = (replaying && replay_now.known()) ? replay_now : now();
  ctx.policy = config.freshness;
  return ctx;
}

void Engine::Impl::note(FlowRuntime& flow, const Anomaly& anomaly, const Limits& limits) {
  if (limits.max_anomalies_per_flow == 0) {
    ++flow.anomalies_dropped;
    return;
  }
  if (flow.anomalies.size() >= limits.max_anomalies_per_flow) {
    flow.anomalies.erase(flow.anomalies.begin());
    ++flow.anomalies_dropped;
  }
  flow.anomalies.push_back(anomaly);
}

Anomaly Engine::Impl::make_anomaly(AnomalyKind kind, Timestamp at,
                                   const Observation& observation, std::string detail) {
  Anomaly anomaly;
  anomaly.kind = kind;
  anomaly.at = at;
  anomaly.observed_at = observation.observed_at;
  anomaly.generation =
      FlowGeneration(observation.subject.flow, observation.subject.generation);
  anomaly.source = observation.key.source;
  anomaly.detail = std::move(detail);
  return anomaly;
}

void Engine::Impl::transition(FlowRuntime& flow, GenerationRuntime& generation, FlowState next,
                              StateCause cause, Timestamp at, Timestamp observed_at,
                              SourceId by, std::string detail, const Limits& limits) {
  if (generation.state == next && generation.cause == cause) {
    return;
  }
  LifecycleEvent event;
  event.at = at;
  event.observed_at = observed_at;
  event.generation = FlowGeneration(flow.id, generation.id);
  event.from = generation.state;
  event.to = next;
  event.cause = cause;
  event.source = by;
  event.detail = detail.size() > limits.max_detail_bytes
                     ? detail.substr(0, limits.max_detail_bytes)
                     : detail;
  flow.history.push(event);
  generation.state = next;
  generation.cause = cause;
  if (is_terminal(next)) {
    generation.closed_at = at;
  }
}

// ---------------------------------------------------------------------------
// Pure view builders
// ---------------------------------------------------------------------------

SourceView Engine::Impl::source_view(const SourceBinding& binding,
                                     const SourceDescriptor& descriptor,
                                     const FreshnessContext& ctx) const {
  if (!binding.cache_valid) {
    FreshnessContext fold_ctx = ctx;
    fold_ctx.from_previous_epoch = !binding.reconfirmed;
    binding.cached = fold_source_view(binding.ledger, descriptor, fold_ctx, config.counters);
    binding.cache_valid = true;
  }
  SourceView view = binding.cached;
  view.freshness = classify_freshness(view.last_observed_at, view.last_received_at, ctx);
  view.refused_observations = binding.refused_observations;
  view.stale_epoch_events = binding.stale_epoch_events;
  view.stale_incarnation_events = binding.stale_incarnation_events;
  view.clock_skew_events = binding.clock_skew_events;
  view.unsupported_events = binding.unsupported_events;
  view.incomplete_events = binding.incomplete_events;
  view.restored_from_journal = binding.restored;
  view.reconfirmed = binding.reconfirmed;
  if (!binding.reconfirmed && config.freshness.require_reconfirmation_after_restart) {
    view.freshness = Freshness::Unknown;
  }
  return view;
}

GenerationView Engine::Impl::generation_view(const FlowRuntime& flow,
                                             const GenerationRuntime& generation,
                                             const FreshnessContext& ctx) const {
  std::vector<SourceView> views;
  views.reserve(generation.bindings.size());
  for (const SourceBinding& binding : generation.bindings) {
    const auto found = sources.find(binding.source);
    if (found == sources.end()) {
      continue;
    }
    views.push_back(source_view(binding, descriptor_for(found->second), ctx));
  }
  GenerationView view =
      reconcile_generation(flow.id, generation.id, std::move(views), config.reconciliation,
                           generation.id == flow.current_generation);
  view.state = generation.state;
  view.cause = generation.cause;
  // `closed_at` records when the runtime closed the generation, which is a
  // property of the transition and not of any single observation.
  view.closed_at = generation.closed_at;
  // Everything else in the view is a pure function of the folded evidence:
  // coverage, correlation, timings, accounting and consistency are all computed
  // above. Nothing is merged back in from the runtime record, because a merge
  // would make the view depend on the order in which evidence arrived.
  return view;
}

void Engine::Impl::recompute_flow(FlowRuntime& flow, Timestamp at, const Limits& limits,
                                  std::vector<LifecycleEvent>* new_events) {
  const FreshnessContext ctx = context();
  Coverage union_coverage;
  for (GenerationRuntime& generation : flow.generations) {
    const GenerationView view = generation_view(flow, generation, ctx);
    const StateDecision decision =
        decide_generation_state(view, generation.state, config.expiry, ctx);

    // Fold-level anomalies are promoted once per (source, kind) so that an
    // operator reading the flow sees "this source's counters regressed" without
    // having to inspect every generation.
    for (SourceBinding& binding : generation.bindings) {
      const auto found = sources.find(binding.source);
      if (found == sources.end()) {
        continue;
      }
      const SourceDescriptor& descriptor = descriptor_for(found->second);
      const SourceView source = source_view(binding, descriptor, ctx);
      for (std::size_t index = 0; index < kAnomalyKindCount; ++index) {
        if (source.anomaly_counts[index] == 0) {
          continue;
        }
        if (index >= 32 || (binding.promoted_anomaly_kinds & (1u << index)) != 0u) {
          continue;
        }
        binding.promoted_anomaly_kinds |= (1u << index);
        Anomaly anomaly;
        anomaly.kind = static_cast<AnomalyKind>(index);
        anomaly.at = at;
        anomaly.observed_at = source.last_observed_at;
        anomaly.generation = FlowGeneration(flow.id, generation.id);
        anomaly.source = binding.source;
        anomaly.detail = std::to_string(source.anomaly_counts[index]) +
                         " occurrence(s) recorded while folding this source's evidence";
        note(flow, anomaly, limits);
        counters.anomalies_recorded.fetch_add(1, std::memory_order_relaxed);
      }
    }

    // Capability refusals are surfaced even when they do not change state: a
    // declaration the source is not entitled to make must never be silent.
    for (const SourceView& source : view.sources) {
      if (!source.declared_terminal.has_value() || !source.usable_for_liveness()) {
        continue;
      }
      const FlowState declared = *source.declared_terminal;
      const bool capable =
          declared == FlowState::Completed
              ? source.capabilities.asserts_completion
              : (declared == FlowState::Reset ? source.capabilities.asserts_reset : true);
      if (!capable) {
        note(flow,
             make_anomaly(AnomalyKind::CapabilityUnsupported, at, Observation{},
                          std::string("source ") + flowobs::to_string(source.source) +
                              " declared " + std::string(flowobs::to_string(declared)) +
                              " without holding the capability"),
             limits);
      }
    }

    generation.coverage = view.coverage;
    generation.endpoint_a = view.endpoint_a;
    generation.endpoint_b = view.endpoint_b;
    // Assign, never accumulate: these are projections of the folded evidence,
    // so an intermediate value seen while evidence was still arriving must not
    // be able to outlive the final one.
    generation.first_observed_at = view.first_observed_at;
    generation.last_observed_at = view.last_observed_at;
    generation.last_received_at = view.last_received_at;
    generation.last_progress_at = view.last_progress_at;

    // StateCause::None means "the previous cause still stands", so a decision
    // that changes neither state nor cause is a no-op even when it was reached
    // through a different branch.
    const StateCause next_cause =
        decision.cause == StateCause::None ? generation.cause : decision.cause;
    const bool changed =
        decision.state != generation.state || next_cause != generation.cause;
    if (changed) {
      const FlowState previous_state = generation.state;
      SourceId by{};
      if (!generation.bindings.empty()) {
        by = generation.bindings.front().source;
      }
      transition(flow, generation, decision.state, next_cause, at,
                 generation.last_observed_at, by, decision.detail, limits);
      counters.transitions.fetch_add(1, std::memory_order_relaxed);
      if (new_events != nullptr) {
        LifecycleEvent event;
        event.at = at;
        event.observed_at = generation.last_observed_at;
        event.generation = FlowGeneration(flow.id, generation.id);
        event.from = previous_state;
        event.to = decision.state;
        event.cause = next_cause;
        event.source = by;
        event.detail = decision.detail;
        new_events->push_back(std::move(event));
      }
      flow.state_changed_at = at;
      if (decision.state == FlowState::Expired && previous_state != FlowState::Expired) {
        counters.flows_expired.fetch_add(1, std::memory_order_relaxed);
      }
    }

    union_coverage.forward_complete =
        union_coverage.forward_complete || generation.coverage.forward_complete;
    union_coverage.forward_partial =
        union_coverage.forward_partial || generation.coverage.forward_partial;
    union_coverage.reverse_complete =
        union_coverage.reverse_complete || generation.coverage.reverse_complete;
    union_coverage.reverse_partial =
        union_coverage.reverse_partial || generation.coverage.reverse_partial;
  }
  flow.coverage = union_coverage;

  Timestamp first_observed{};
  Timestamp last_observed{};
  Timestamp last_received{};
  Timestamp last_progress{};
  for (const GenerationRuntime& generation : flow.generations) {
    if (generation.first_observed_at.known() &&
        (!first_observed.known() || generation.first_observed_at < first_observed)) {
      first_observed = generation.first_observed_at;
    }
    if (generation.last_observed_at > last_observed) {
      last_observed = generation.last_observed_at;
    }
    if (generation.last_received_at > last_received) {
      last_received = generation.last_received_at;
    }
    if (generation.last_progress_at > last_progress) {
      last_progress = generation.last_progress_at;
    }
  }
  flow.first_observed_at = first_observed;
  flow.last_observed_at = last_observed;
  flow.last_received_at = last_received;
  flow.last_progress_at = last_progress;

  const GenerationRuntime* current = nullptr;
  for (const GenerationRuntime& generation : flow.generations) {
    if (generation.id == flow.current_generation) {
      current = &generation;
      break;
    }
  }
  if (current == nullptr && !flow.generations.empty()) {
    current = &flow.generations.back();
    flow.current_generation = current->id;
  }
  if (current != nullptr) {
    flow.state = current->state;
    flow.cause = current->cause;
  }

  // A flow counts as restored while no source of its current generation has
  // been reconfirmed in this runtime epoch. That is exactly the condition under
  // which the runtime refuses to call it live.
  bool reconfirmed = false;
  for (const GenerationRuntime& generation : flow.generations) {
    if (generation.id != flow.current_generation) {
      continue;
    }
    for (const SourceBinding& binding : generation.bindings) {
      if (binding.reconfirmed) {
        reconfirmed = true;
        break;
      }
    }
  }
  flow.restored = !reconfirmed && !flow.generations.empty();
}

FlowSnapshot Engine::Impl::snapshot_of(const FlowRuntime& flow, const FreshnessContext& ctx,
                                       const EngineConfig& cfg) const {
  (void)cfg;
  FlowSnapshot snapshot;
  snapshot.id = flow.id;
  snapshot.canonical_key = flow.canonical_key;
  snapshot.key_collisions = flow.key_collisions;
  snapshot.current_generation = flow.current_generation;
  snapshot.revision = flow.revision;
  snapshot.state = flow.state;
  snapshot.cause = flow.cause;
  snapshot.created_at = flow.created_at;
  snapshot.state_changed_at = flow.state_changed_at;
  snapshot.first_observed_at = flow.first_observed_at;
  snapshot.last_observed_at = flow.last_observed_at;
  snapshot.last_received_at = flow.last_received_at;
  snapshot.last_progress_at = flow.last_progress_at;
  snapshot.endpoint_a = flow.endpoint_a;
  snapshot.endpoint_b = flow.endpoint_b;
  snapshot.coverage = flow.coverage;
  snapshot.visibility = flow.coverage.classify();
  snapshot.anomalies = flow.anomalies;
  snapshot.anomalies_dropped = flow.anomalies_dropped;
  snapshot.restored_from_journal = flow.restored;
  snapshot.restored_epoch = flow.restored_epoch;
  snapshot.current_epoch = epoch;

  snapshot.generations.reserve(flow.generations.size());
  for (const GenerationRuntime& generation : flow.generations) {
    snapshot.generations.push_back(generation_view(flow, generation, ctx));
  }
  const GenerationView* current = nullptr;
  for (const GenerationView& view : snapshot.generations) {
    if (view.is_current) {
      current = &view;
      break;
    }
  }
  if (current == nullptr && !snapshot.generations.empty()) {
    current = &snapshot.generations.back();
  }
  if (current != nullptr) {
    snapshot.freshness = current->freshness;
    snapshot.consistency = current->consistency;
    snapshot.visibility = current->visibility;
    snapshot.total = current->total;
    snapshot.state = current->state;
    snapshot.cause = current->cause;
  }
  return snapshot;
}

// ---------------------------------------------------------------------------
// Durable projection
// ---------------------------------------------------------------------------

PersistedSource Engine::Impl::persist_source(const SourceBinding& binding) const {
  // The durable record is the *whole* fold, not just the part that has already
  // been compacted out of the retained window.
  const SourceLedger::Prefix prefix = binding.ledger.folded_state();
  PersistedSource out;
  out.source = binding.source;
  out.incarnation = prefix.current_incarnation;
  out.epoch = prefix.current_epoch;
  out.high_revision = prefix.high_revision;
  out.high_sequence = prefix.high_sequence;
  out.first_observed_at = prefix.first_observed_at;
  out.last_observed_at = prefix.last_observed_at;
  out.last_received_at = prefix.last_received_at;
  out.last_progress_at = prefix.last_progress_at;
  out.coverage = prefix.coverage;
  out.hop_capacity_units = prefix.hop_capacity_units;
  out.forward = prefix.forward;
  out.reverse = prefix.reverse;
  out.aggregate = prefix.aggregate;
  out.last_kind = prefix.last_kind;
  out.last_direction = prefix.last_direction;
  out.last_evidence = prefix.last_evidence;
  out.declared_terminal = prefix.declared_terminal;
  out.declared_cause = prefix.declared_cause;
  out.observed_progress = prefix.observed_progress;
  out.accepted_observations = prefix.folded_observations;
  out.refused_observations = binding.refused_observations;
  out.dropped_observations = static_cast<std::uint32_t>(
      binding.ledger.prefix().folded_observations > 0xFFFFFFFFull
          ? 0xFFFFFFFFull
          : binding.ledger.prefix().folded_observations);
  out.duplicates_suppressed =
      static_cast<std::uint32_t>(binding.ledger.duplicates_suppressed());
  out.content_conflicts = static_cast<std::uint32_t>(binding.ledger.content_conflicts());
  out.historical_incarnations = prefix.historical_incarnations;
  out.after_terminal_observations = prefix.after_terminal_observations;

  const auto found = sources.find(binding.source);
  if (found != sources.end()) {
    out.canonical_key = found->second.descriptor.canonical_key;
    out.authority = found->second.descriptor.authority;
    out.capabilities = found->second.descriptor.capabilities;
    out.advisory_only = found->second.descriptor.advisory_only;
  }
  const auto to_consumption = [](const DirectionalAccounting& accounting) {
    Consumption consumption;
    consumption.bytes = accounting.bytes.accumulated();
    consumption.packets = accounting.packets.accumulated();
    consumption.retransmissions = accounting.retransmissions.accumulated();
    consumption.unknown_intervals = accounting.unknown_intervals();
    consumption.bytes_unsupported = accounting.bytes_unsupported;
    consumption.packets_unsupported = accounting.packets_unsupported;
    consumption.retransmissions_unsupported = accounting.retransmissions_unsupported;
    consumption.saturated = accounting.bytes.saturated() || accounting.packets.saturated() ||
                            accounting.retransmissions.saturated();
    return consumption;
  };
  out.forward_totals = to_consumption(out.forward);
  out.reverse_totals = to_consumption(out.reverse);
  out.aggregate_totals = to_consumption(out.aggregate);
  return out;
}

PersistedFlow Engine::Impl::persist_flow(const FlowRuntime& flow,
                                         const FreshnessContext& ctx) const {
  PersistedFlow out;
  out.id = flow.id;
  out.canonical_key = flow.canonical_key;
  out.key_collisions = flow.key_collisions;
  out.current_generation = flow.current_generation;
  out.revision = flow.revision;
  out.state = flow.state;
  out.cause = flow.cause;
  out.coverage = flow.coverage;
  out.created_at = flow.created_at;
  out.state_changed_at = flow.state_changed_at;
  out.first_observed_at = flow.first_observed_at;
  out.last_observed_at = flow.last_observed_at;
  out.last_received_at = flow.last_received_at;
  out.last_progress_at = flow.last_progress_at;
  out.endpoint_a = flow.endpoint_a;
  out.endpoint_b = flow.endpoint_b;
  out.history = flow.history.to_vector();
  out.history_dropped = flow.history.dropped();
  out.anomalies = flow.anomalies;
  out.anomalies_dropped = flow.anomalies_dropped;

  out.generations.reserve(flow.generations.size());
  for (const GenerationRuntime& generation : flow.generations) {
    PersistedGeneration item;
    item.generation = generation.id;
    item.is_current = generation.id == flow.current_generation;
    item.first_observed_at = generation.first_observed_at;
    item.last_observed_at = generation.last_observed_at;
    item.last_received_at = generation.last_received_at;
    item.last_progress_at = generation.last_progress_at;
    item.closed_at = generation.closed_at;
    item.state = generation.state;
    item.cause = generation.cause;
    item.coverage = generation.coverage;
    item.endpoint_a = generation.endpoint_a;
    item.endpoint_b = generation.endpoint_b;
    for (const SourceBinding& binding : generation.bindings) {
      item.sources.push_back(persist_source(binding));
    }
    const GenerationView view = generation_view(flow, generation, ctx);
    item.path = view.path;
    item.links = view.links;
    item.queues = view.queues;
    item.forward = view.forward;
    item.reverse = view.reverse;
    item.aggregate = view.aggregate;
    item.total = view.total;
    item.unresolved_conflict = view.unresolved_conflict;
    item.conflict_detail = view.conflict_detail;
    out.generations.push_back(std::move(item));
  }
  return out;
}

PersistedState Engine::Impl::persist_view_state() const {
  PersistedState state = persist_state();
  // Ingest bookkeeping describes what the runtime saw, not what it concluded.
  // The determinism contract is about the conclusion, so the digest is taken
  // over the evidence-derived view with the diagnostics cleared.
  for (PersistedFlow& flow : state.flows) {
    flow.anomalies.clear();
    flow.anomalies_dropped = 0;
    // The lifecycle log records the path the runtime took to reach the current
    // view, which legitimately depends on the order evidence arrived in. The
    // current view does not.
    flow.history.clear();
    flow.history_dropped = 0;
    for (PersistedGeneration& generation : flow.generations) {
      for (PersistedSource& source : generation.sources) {
        source.duplicates_suppressed = 0;
        source.refused_observations = 0;
        source.dropped_observations = 0;
        source.after_terminal_observations = 0;
      }
    }
  }
  return state;
}

PersistedState Engine::Impl::persist_state() const {
  PersistedState state;
  state.format_version = kJournalFormatVersion;
  state.identity_scheme = kIdentitySchemeVersion;
  state.schema_version = kObservationSchemaVersion;
  state.policy_digest = config.policy_digest();
  state.runtime_epoch = epoch;
  state.created_at = opened_at;
  state.snapshot_at = now();
  state.snapshot_sequence = snapshot_sequence + 1;
  for (const auto& [id, source] : sources) {
    (void)id;
    if (!source.retired) {
      state.sources.push_back(source.descriptor);
    }
  }
  const FreshnessContext ctx = context();
  state.flows.reserve(flows.size());
  for (const auto& [id, flow] : flows) {
    (void)id;
    state.flows.push_back(persist_flow(flow, ctx));
  }
  return state;
}

// ---------------------------------------------------------------------------
// Journal append helpers
// ---------------------------------------------------------------------------

Status Engine::Impl::compact_locked() {
  if (journal == nullptr) {
    return make_ok();
  }
  const PersistedState state = persist_state();
  std::vector<journal::Record> records;
  Status s = encode_state(state, config.limits, records);
  if (s.failed()) {
    counters.journal_failures.fetch_add(1, std::memory_order_relaxed);
    return s;
  }
  s = journal->rewrite(records, epoch, opened_at);
  if (s.failed()) {
    counters.journal_failures.fetch_add(1, std::memory_order_relaxed);
    return s;
  }
  counters.journal_compactions.fetch_add(1, std::memory_order_relaxed);
  snapshot_sequence = state.snapshot_sequence;
  return make_ok();
}

Status Engine::Impl::append_observation_locked(const Observation& observation) {
  if (journal == nullptr) {
    return make_ok();
  }
  detail::Writer writer;
  detail::encode_observation(writer, observation);
  journal::Record record;
  record.kind = journal::RecordKind::ObservationRecord;
  record.payload = writer.take();
  Status s = journal->append(record);
  if (s.failed() && s.code() == ErrorCode::LimitExceeded) {
    // The log reached its growth bound: compact once, then retry.
    const Status compacted = compact_locked();
    if (compacted.failed()) {
      return compacted;
    }
    s = journal->append(record);
  }
  if (s.failed()) {
    counters.journal_failures.fetch_add(1, std::memory_order_relaxed);
    return s;
  }
  counters.journal_records_appended.fetch_add(1, std::memory_order_relaxed);
  counters.journal_bytes_appended.fetch_add(record.payload.size(), std::memory_order_relaxed);
  return make_ok();
}

Status Engine::Impl::append_source_locked(const SourceDescriptor& descriptor) {
  if (journal == nullptr) {
    return make_ok();
  }
  detail::Writer writer;
  writer.u64(descriptor.id.value());
  writer.str(descriptor.canonical_key);
  writer.u32(descriptor.authority);
  writer.boolean(descriptor.capabilities.asserts_completion);
  writer.boolean(descriptor.capabilities.asserts_reset);
  writer.boolean(descriptor.capabilities.asserts_path);
  writer.boolean(descriptor.capabilities.asserts_queue);
  writer.boolean(descriptor.capabilities.asserts_counters);
  writer.boolean(descriptor.capabilities.authoritative_identity);
  writer.boolean(descriptor.advisory_only);
  journal::Record record;
  record.kind = journal::RecordKind::SourceDescriptor;
  record.payload = writer.take();
  const Status s = journal->append(record);
  if (s.failed()) {
    counters.journal_failures.fetch_add(1, std::memory_order_relaxed);
    return s;
  }
  return make_ok();
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

Status Engine::Impl::validate_observation(const Observation& observation) const {
  const Limits& limits = config.limits;
  if (observation.schema_version != kObservationSchemaVersion) {
    return Status(ErrorCode::VersionMismatch,
                  "observation schema version " +
                      std::to_string(observation.schema_version) +
                      " is not supported by this build");
  }
  if (!observation.key.source.valid()) {
    return invalid_argument("observation source identity is invalid");
  }
  if (!observation.key.incarnation.valid()) {
    return invalid_argument(
        "observation incarnation is invalid: an incarnation fence is required");
  }
  if (!observation.subject.generation.valid()) {
    return invalid_argument("observation generation is invalid");
  }
  if (observation.subject.flow_key.empty()) {
    return invalid_argument("observation flow_key is required");
  }
  if (observation.subject.flow_key.size() > limits.max_key_bytes) {
    return limit_exceeded("observation flow_key exceeds limits.max_key_bytes");
  }
  if (observation.subject.flow.valid() &&
      !key_matches(observation.subject.flow, observation.subject.flow_key)) {
    return invalid_argument("observation flow identity does not match its canonical key");
  }
  if (observation.subject.endpoint.valid() && !observation.subject.endpoint_key.empty() &&
      !key_matches(observation.subject.endpoint, observation.subject.endpoint_key)) {
    return invalid_argument("observation endpoint identity does not match its canonical key");
  }
  if (observation.subject.path.has_value() && !observation.subject.path_key.empty() &&
      !key_matches(*observation.subject.path, observation.subject.path_key)) {
    return invalid_argument("observation path identity does not match its canonical key");
  }
  if (observation.subject.queue.has_value() && !observation.subject.queue_key.empty() &&
      !key_matches(*observation.subject.queue, observation.subject.queue_key)) {
    return invalid_argument("observation queue identity does not match its canonical key");
  }
  if (observation.subject.link.has_value() && !observation.subject.link_key.empty() &&
      !key_matches(*observation.subject.link, observation.subject.link_key)) {
    return invalid_argument("observation link identity does not match its canonical key");
  }
  if (!observation.observed_at.known()) {
    return invalid_argument(
        "observation observed_at is required: absence of a time is not evidence");
  }
  if (observation.evidence == Evidence::Unsupported && observation.counters.any()) {
    return invalid_argument(
        "evidence marked unsupported must not carry counter values; a value that does not "
        "exist is not zero");
  }
  if (observation.subject.hop_capacity_units.size() > limits.max_path_hops) {
    return limit_exceeded("observation declares more hops than limits.max_path_hops");
  }
  if (observation.subject.path_generation.has_value() &&
      *observation.subject.path_generation != observation.subject.generation) {
    return Status(ErrorCode::StaleGeneration,
                  "observation declares path generation " +
                      std::to_string(observation.subject.path_generation->value()) +
                      " for flow generation " +
                      std::to_string(observation.subject.generation.value()));
  }
  return make_ok();
}

Status Engine::Impl::register_identity(FlowRuntime& flow, const Observation& observation,
                                       AnomalyKind& anomaly_kind, std::string& detail) {
  const auto handle = [&](auto& registry, auto id, const std::string& key) -> Status {
    if (!id.valid() || key.empty()) {
      return make_ok();
    }
    std::string existing;
    const auto outcome = registry.observe(id, key, config.limits.max_flows, existing);
    using Outcome = std::decay_t<decltype(outcome)>;
    switch (outcome) {
      case Outcome::Registered:
      case Outcome::Known:
        return make_ok();
      case Outcome::Collision:
        anomaly_kind = AnomalyKind::IdentityCollision;
        detail = "identity collision: \"" + existing + "\" and \"" + key +
                 "\" map to the same identity";
        flow.key_collisions += 1;
        counters.identity_collisions.fetch_add(1, std::memory_order_relaxed);
        return Status(ErrorCode::EvidenceConflict, detail);
      case Outcome::Full:
        anomaly_kind = AnomalyKind::LimitEviction;
        detail = "identity registry is full";
        return limit_exceeded("identity registry is full");
    }
    return internal_error("unreachable identity registry outcome");
  };

  Status status = handle(endpoints, observation.subject.endpoint,
                         observation.subject.endpoint_key);
  if (status.failed()) return status;
  status = handle(endpoints, observation.subject.peer_endpoint,
                  observation.subject.peer_endpoint_key);
  if (status.failed()) return status;
  status = handle(paths, observation.subject.path.value_or(PathId{}),
                  observation.subject.path_key);
  if (status.failed()) return status;
  status = handle(queues, observation.subject.queue.value_or(QueueId{}),
                  observation.subject.queue_key);
  if (status.failed()) return status;
  return handle(links, observation.subject.link.value_or(LinkId{}),
                observation.subject.link_key);
}

GenerationRuntime& Engine::Impl::obtain_generation(FlowRuntime& flow, GenerationId id,
                                                  Timestamp at) {
  const auto position = std::lower_bound(
      flow.generations.begin(), flow.generations.end(), id,
      [](const GenerationRuntime& generation, GenerationId wanted) {
        return generation.id < wanted;
      });
  if (position != flow.generations.end() && position->id == id) {
    return *position;
  }
  GenerationRuntime created;
  created.id = id;
  created.last_touched = at;
  const auto inserted = flow.generations.insert(position, std::move(created));
  counters.generations_created.fetch_add(1, std::memory_order_relaxed);
  return *inserted;
}

SourceBinding& Engine::Impl::obtain_binding(GenerationRuntime& generation, SourceId source,
                                            FlowGeneration flow_generation) {
  const auto position = std::lower_bound(
      generation.bindings.begin(), generation.bindings.end(), source,
      [](const SourceBinding& binding, SourceId wanted) { return binding.source < wanted; });
  if (position != generation.bindings.end() && position->source == source) {
    return *position;
  }
  SourceBinding created;
  created.source = source;
  created.ledger.configure(source, flow_generation,
                           config.limits.max_observations_per_source_flow);
  created.restored = replaying;
  created.reconfirmed = !replaying;
  const auto inserted = generation.bindings.insert(position, std::move(created));
  return *inserted;
}

// ---------------------------------------------------------------------------
// Ingest
// ---------------------------------------------------------------------------

Result<IngestOutcome> Engine::Impl::apply_locked(const Observation& incoming) {
  const Timestamp at = (replaying && replay_now.known()) ? replay_now : now();
  IngestOutcome outcome;
  counters.observations_received.fetch_add(1, std::memory_order_relaxed);

  const Status validation = validate_observation(incoming);
  if (validation.failed()) {
    outcome.disposition = IngestDisposition::Refused;
    outcome.code = validation.code();
    outcome.detail = validation.message();
    counters.observations_refused.fetch_add(1, std::memory_order_relaxed);
    return outcome;
  }

  Observation observation = incoming;
  if (!observation.received_at.known()) {
    observation.received_at = at;
  }
  if (!observation.subject.flow.valid()) {
    observation.subject.flow = flow_id_from_key(observation.subject.flow_key);
  }

  // The observation is journaled before it is applied so that a log failure is
  // a clean refusal: nothing is applied that the durable log did not record.
  if (!replaying) {
    const Status journaled = append_observation_locked(observation);
    if (journaled.failed()) {
      outcome.disposition = IngestDisposition::Refused;
      outcome.code = journaled.code();
      outcome.detail = "durable log refused the observation: " + journaled.message();
      counters.observations_refused.fetch_add(1, std::memory_order_relaxed);
      return outcome;
    }
  }

  // --- source -----------------------------------------------------------
  auto source_it = sources.find(observation.key.source);
  if (source_it == sources.end()) {
    if (sources.size() >= config.limits.max_sources) {
      outcome.disposition = IngestDisposition::Refused;
      outcome.code = ErrorCode::LimitExceeded;
      outcome.detail = "source registry is full";
      counters.observations_refused.fetch_add(1, std::memory_order_relaxed);
      return outcome;
    }
    SourceState state;
    state.descriptor.id = observation.key.source;
    state.descriptor.canonical_key = "unregistered:" + flowobs::to_string(observation.key.source);
    state.descriptor.authority = 0;
    // An unregistered source gets no capabilities. A source cannot grant
    // itself the authority to declare completion or reset by asserting it in
    // a payload; only register_source does that.
    state.descriptor.capabilities = SourceCapabilities{};
    state.registered_at = at;
    source_it = sources.emplace(observation.key.source, std::move(state)).first;
  }
  if (source_it->second.retired) {
    outcome.disposition = IngestDisposition::Refused;
    outcome.code = ErrorCode::NotFound;
    outcome.detail = "source has been retired";
    counters.observations_refused.fetch_add(1, std::memory_order_relaxed);
    return outcome;
  }

  // --- flow -------------------------------------------------------------
  auto flow_it = flows.find(observation.subject.flow);
  if (flow_it == flows.end()) {
    if (flows.size() >= config.limits.max_flows) {
      const Status evicted = evict_one_flow_locked();
      if (evicted.failed() || flows.size() >= config.limits.max_flows) {
        outcome.disposition = IngestDisposition::Refused;
        outcome.code = ErrorCode::LimitExceeded;
        outcome.detail = "flow table is full and no terminal flow can be evicted";
        counters.observations_refused.fetch_add(1, std::memory_order_relaxed);
        return outcome;
      }
    }
    FlowRuntime created;
    created.id = observation.subject.flow;
    created.canonical_key = observation.subject.flow_key;
    created.created_at = at;
    created.state_changed_at = at;
    created.restored = replaying;
    created.restored_epoch = replaying ? replay_epoch : epoch;
    created.last_touched = at;
    created.history.set_capacity(config.limits.max_history_per_flow);
    flow_it = flows.emplace(observation.subject.flow, std::move(created)).first;
    counters.flows_created.fetch_add(1, std::memory_order_relaxed);
  }
  FlowRuntime& flow = flow_it->second;
  if (flow.canonical_key != observation.subject.flow_key) {
    const std::string detail = "identity collision: \"" + flow.canonical_key + "\" and \"" +
                               observation.subject.flow_key +
                               "\" map to the same flow identity";
    flow.key_collisions += 1;
    counters.identity_collisions.fetch_add(1, std::memory_order_relaxed);
    note(flow, make_anomaly(AnomalyKind::IdentityCollision, at, observation, detail),
         config.limits);
    counters.anomalies_recorded.fetch_add(1, std::memory_order_relaxed);
    flow.state = FlowState::Conflicting;
    flow.cause = StateCause::AuthorityConflict;
    outcome.disposition = IngestDisposition::Refused;
    outcome.code = ErrorCode::EvidenceConflict;
    outcome.detail = detail;
    counters.observations_conflicting.fetch_add(1, std::memory_order_relaxed);
    return outcome;
  }

  AnomalyKind anomaly_kind = AnomalyKind::None;
  std::string anomaly_detail;
  const Status identity_status =
      register_identity(flow, observation, anomaly_kind, anomaly_detail);
  if (identity_status.failed()) {
    if (anomaly_kind != AnomalyKind::None) {
      note(flow, make_anomaly(anomaly_kind, at, observation, anomaly_detail), config.limits);
      counters.anomalies_recorded.fetch_add(1, std::memory_order_relaxed);
    }
    outcome.disposition = IngestDisposition::Refused;
    outcome.code = identity_status.code();
    outcome.detail = identity_status.message();
    counters.observations_refused.fetch_add(1, std::memory_order_relaxed);
    return outcome;
  }

  // --- clock skew -------------------------------------------------------
  if (exceeds_clock_skew(observation.observed_at, observation.received_at,
                         config.limits.max_clock_skew)) {
    if (!config.freshness.clamp_future_timestamps) {
      note(flow,
           make_anomaly(AnomalyKind::ClockSkew, at, observation,
                        "observed_at is further ahead of received_at than "
                        "limits.max_clock_skew and clamping is disabled"),
           config.limits);
      counters.anomalies_recorded.fetch_add(1, std::memory_order_relaxed);
      outcome.disposition = IngestDisposition::Refused;
      outcome.code = ErrorCode::OutOfRange;
      outcome.detail = "observed_at exceeds the tolerated clock skew";
      counters.observations_refused.fetch_add(1, std::memory_order_relaxed);
      return outcome;
    }
    note(flow,
         make_anomaly(AnomalyKind::ClockSkew, at, observation,
                      "observed_at was ahead of received_at and was clamped"),
         config.limits);
    counters.anomalies_recorded.fetch_add(1, std::memory_order_relaxed);
    observation.observed_at = observation.received_at;
  }

  // --- generation -------------------------------------------------------
  const bool generation_known =
      std::any_of(flow.generations.begin(), flow.generations.end(),
                  [&observation](const GenerationRuntime& generation) {
                    return generation.id == observation.subject.generation;
                  });
  if (!generation_known &&
      flow.generations.size() >= config.limits.max_generations_per_flow) {
    const Status evicted = evict_one_generation_locked(flow);
    if (evicted.failed()) {
      note(flow,
           make_anomaly(AnomalyKind::LimitEviction, at, observation,
                        "generation table is full and no terminal generation can be evicted"),
           config.limits);
      counters.anomalies_recorded.fetch_add(1, std::memory_order_relaxed);
      outcome.disposition = IngestDisposition::Refused;
      outcome.code = ErrorCode::LimitExceeded;
      outcome.detail = "generation table is full";
      counters.observations_refused.fetch_add(1, std::memory_order_relaxed);
      return outcome;
    }
  }

  const bool first_generation = !flow.current_generation.valid();
  const bool newer_generation = observation.subject.generation > flow.current_generation;
  GenerationRuntime& generation = obtain_generation(flow, observation.subject.generation, at);
  if (first_generation || newer_generation) {
    flow.current_generation = observation.subject.generation;
  } else if (observation.subject.generation < flow.current_generation) {
    note(flow,
         make_anomaly(AnomalyKind::StaleGenerationEvidence, at, observation,
                      "evidence names a superseded generation; it is retained as history and "
                      "cannot support current attribution"),
         config.limits);
    counters.anomalies_recorded.fetch_add(1, std::memory_order_relaxed);
  }

  const bool binding_known =
      std::any_of(generation.bindings.begin(), generation.bindings.end(),
                  [&observation](const SourceBinding& binding) {
                    return binding.source == observation.key.source;
                  });
  if (!binding_known && generation.bindings.size() >= config.limits.max_sources_per_flow) {
    note(flow,
         make_anomaly(AnomalyKind::LimitEviction, at, observation,
                      "per-generation source table is full"),
         config.limits);
    counters.anomalies_recorded.fetch_add(1, std::memory_order_relaxed);
    outcome.disposition = IngestDisposition::Refused;
    outcome.code = ErrorCode::LimitExceeded;
    outcome.detail = "per-generation source table is full";
    counters.observations_refused.fetch_add(1, std::memory_order_relaxed);
    return outcome;
  }

  SourceBinding& binding =
      obtain_binding(generation, observation.key.source, FlowGeneration(flow.id, generation.id));

  // --- apply ------------------------------------------------------------
  const SourceLedger::InsertResult inserted = binding.ledger.insert(observation);
  outcome.generation = FlowGeneration(flow.id, generation.id);
  outcome.state_before = generation.state;

  switch (inserted.kind) {
    case SourceLedger::InsertResult::Kind::Accepted: {
      binding.cache_valid = false;
      if (!replaying) {
        binding.reconfirmed = true;
      }
      counters.observations_applied.fetch_add(1, std::memory_order_relaxed);
      outcome.disposition = IngestDisposition::Applied;
      outcome.code = ErrorCode::Ok;
      flow.revision = flow.revision.next();
      break;
    }
    case SourceLedger::InsertResult::Kind::Duplicate: {
      // An exact redelivery is a no-op in every observable respect: it is
      // counted in the ingest statistics and nowhere else, so the reconciled
      // view - and therefore the durable projection - is bit identical.
      counters.observations_duplicate.fetch_add(1, std::memory_order_relaxed);
      outcome.disposition = IngestDisposition::Duplicate;
      outcome.code = ErrorCode::DuplicateEvidence;
      outcome.detail = inserted.detail;
      break;
    }
    case SourceLedger::InsertResult::Kind::ContentConflict: {
      counters.observations_conflicting.fetch_add(1, std::memory_order_relaxed);
      note(flow, make_anomaly(AnomalyKind::SequenceConflict, at, observation, inserted.detail),
           config.limits);
      counters.anomalies_recorded.fetch_add(1, std::memory_order_relaxed);
      outcome.disposition = IngestDisposition::Refused;
      outcome.code = ErrorCode::EvidenceConflict;
      outcome.detail = inserted.detail;
      break;
    }
    case SourceLedger::InsertResult::Kind::Refused: {
      binding.refused_observations += 1;
      counters.observations_refused.fetch_add(1, std::memory_order_relaxed);
      const AnomalyKind kind = inserted.code == ErrorCode::StaleRevision
                                   ? AnomalyKind::LateEvidenceRejected
                                   : AnomalyKind::MalformedEvidence;
      note(flow, make_anomaly(kind, at, observation, inserted.detail), config.limits);
      counters.anomalies_recorded.fetch_add(1, std::memory_order_relaxed);
      outcome.disposition = IngestDisposition::Refused;
      outcome.code = inserted.code;
      outcome.detail = inserted.detail;
      break;
    }
  }

  if (inserted.kind == SourceLedger::InsertResult::Kind::Refused) {
    return outcome;
  }

  // --- roll-ups and state ----------------------------------------------
  flow.last_touched = at;
  generation.last_touched = at;

  std::vector<LifecycleEvent> events;
  const std::uint64_t recorded_before =
      counters.anomalies_recorded.load(std::memory_order_relaxed);
  recompute_flow(flow, at, config.limits, &events);
  const std::uint64_t anomaly_delta =
      counters.anomalies_recorded.load(std::memory_order_relaxed) - recorded_before;

  outcome.state_after = generation.state;
  outcome.cause = generation.cause;
  outcome.transitions = static_cast<std::uint32_t>(events.size());
  outcome.anomalies = static_cast<std::uint32_t>(
      anomaly_delta > 0xFFFFFFFFull ? 0xFFFFFFFFull : anomaly_delta);
  return outcome;
}

Status Engine::Impl::evict_one_flow_locked() {
  FlowId victim{};
  Timestamp best{};
  bool found = false;
  for (const auto& [id, flow] : flows) {
    if (!is_terminal(flow.state)) {
      continue;
    }
    if (!found || flow.last_touched < best || (flow.last_touched == best && id < victim)) {
      victim = id;
      best = flow.last_touched;
      found = true;
    }
  }
  if (!found) {
    return limit_exceeded("no terminal flow is available for eviction");
  }
  flows.erase(victim);
  counters.flows_evicted.fetch_add(1, std::memory_order_relaxed);
  return make_ok();
}

Status Engine::Impl::evict_one_generation_locked(FlowRuntime& flow) {
  std::size_t victim = flow.generations.size();
  for (std::size_t i = 0; i < flow.generations.size(); ++i) {
    if (flow.generations[i].id == flow.current_generation) {
      continue;
    }
    if (!is_terminal(flow.generations[i].state)) {
      continue;
    }
    if (victim == flow.generations.size() || flow.generations[i].id < flow.generations[victim].id) {
      victim = i;
    }
  }
  if (victim == flow.generations.size()) {
    return limit_exceeded("no terminal generation is available for eviction");
  }
  flow.generations.erase(flow.generations.begin() + static_cast<std::ptrdiff_t>(victim));
  counters.generations_evicted.fetch_add(1, std::memory_order_relaxed);
  return make_ok();
}

// ---------------------------------------------------------------------------
// Journal, workers and tick
// ---------------------------------------------------------------------------

Status Engine::Impl::open_journal_locked() {
  opened_at = now();
  if (config.journal_path.empty()) {
    epoch = RuntimeEpoch::from_value(1);
    return make_ok();
  }
  JournalOpenOptions options;
  options.limits = config.limits;
  options.recovery = config.recovery;
  options.read_only = false;
  options.create_if_missing = true;
  options.truncate_existing = false;
  Result<std::unique_ptr<Journal>> opened = Journal::open(config.journal_path, options);
  if (!opened.ok()) {
    return opened.status();
  }
  journal = std::move(opened.value());
  const RuntimeEpoch previous = journal->header().runtime_epoch;
  epoch = RuntimeEpoch::from_value(previous.value() + 1);
  journal->set_runtime_epoch(epoch);
  journal->set_created_at(opened_at);

  Result<std::vector<journal::Record>> records = journal->read_all();
  if (!records.ok()) {
    return records.status();
  }
  if (config.restore_on_open && !records.value().empty()) {
    const Status restored = restore_locked(records.value(), previous);
    if (restored.failed()) {
      return restored;
    }
  }
  return journal->flush();
}

Status Engine::Impl::restore_locked(const std::vector<journal::Record>& records,
                                    RuntimeEpoch previous_epoch) {
  Result<ReplayResult> replayed = replay_records(records, config.limits, config.recovery);
  if (!replayed.ok()) {
    return replayed.status();
  }
  ReplayResult& result = replayed.value();
  if (result.saw_snapshot && result.state.policy_digest != config.policy_digest()) {
    return Status(ErrorCode::VersionMismatch,
                  "the journal was folded under a different counter policy; refusing to "
                  "reinterpret its accounting");
  }
  snapshot_sequence = result.state.snapshot_sequence;

  for (const SourceDescriptor& descriptor : result.state.sources) {
    SourceState state;
    state.descriptor = descriptor;
    state.registered_at = opened_at;
    sources[descriptor.id] = std::move(state);
  }

  for (const PersistedFlow& persisted : result.state.flows) {
    FlowRuntime flow;
    flow.id = persisted.id;
    flow.canonical_key = persisted.canonical_key;
    flow.key_collisions = persisted.key_collisions;
    flow.current_generation = persisted.current_generation;
    flow.revision = persisted.revision;
    flow.state = persisted.state;
    flow.cause = persisted.cause;
    flow.coverage = persisted.coverage;
    flow.created_at = persisted.created_at;
    flow.state_changed_at = persisted.state_changed_at;
    flow.first_observed_at = persisted.first_observed_at;
    flow.last_observed_at = persisted.last_observed_at;
    flow.last_received_at = persisted.last_received_at;
    flow.last_progress_at = persisted.last_progress_at;
    flow.endpoint_a = persisted.endpoint_a;
    flow.endpoint_b = persisted.endpoint_b;
    flow.anomalies = persisted.anomalies;
    flow.anomalies_dropped = persisted.anomalies_dropped;
    flow.restored = true;
    flow.restored_epoch = previous_epoch;
    flow.last_touched = persisted.last_received_at;
    flow.history.set_capacity(config.limits.max_history_per_flow);
    for (const LifecycleEvent& event : persisted.history) {
      flow.history.push(event);
    }

    for (const PersistedGeneration& item : persisted.generations) {
      if (flow.generations.size() >= config.limits.max_generations_per_flow) {
        break;
      }
      GenerationRuntime generation;
      generation.id = item.generation;
      generation.state = item.state;
      generation.cause = item.cause;
      generation.first_observed_at = item.first_observed_at;
      generation.last_observed_at = item.last_observed_at;
      generation.last_received_at = item.last_received_at;
      generation.last_progress_at = item.last_progress_at;
      generation.closed_at = item.closed_at;
      generation.coverage = item.coverage;
      generation.endpoint_a = item.endpoint_a;
      generation.endpoint_b = item.endpoint_b;
      generation.last_touched = item.last_received_at;
      for (const PersistedSource& persisted_source : item.sources) {
        if (generation.bindings.size() >= config.limits.max_sources_per_flow) {
          break;
        }
        SourceBinding binding;
        binding.source = persisted_source.source;
        binding.ledger.configure(persisted_source.source,
                                 FlowGeneration(flow.id, generation.id),
                                 config.limits.max_observations_per_source_flow);
        SourceLedger::Prefix prefix;
        prefix.current_incarnation = persisted_source.incarnation;
        prefix.current_epoch = persisted_source.epoch;
        prefix.high_revision = persisted_source.high_revision;
        prefix.high_sequence = persisted_source.high_sequence;
        // The folded high-water mark is the persisted high-water mark: anything
        // at or below it was already folded into the accounting that is being
        // restored, and replaying it would double count.
        prefix.folded_incarnation = persisted_source.incarnation;
        prefix.folded_epoch = persisted_source.epoch;
        prefix.folded_revision = persisted_source.high_revision;
        prefix.folded_sequence = persisted_source.high_sequence;
        prefix.first_observed_at = persisted_source.first_observed_at;
        prefix.last_observed_at = persisted_source.last_observed_at;
        prefix.last_received_at = persisted_source.last_received_at;
        prefix.last_progress_at = persisted_source.last_progress_at;
        prefix.coverage = persisted_source.coverage;
        prefix.hop_capacity_units = persisted_source.hop_capacity_units;
        prefix.forward = persisted_source.forward;
        prefix.reverse = persisted_source.reverse;
        prefix.aggregate = persisted_source.aggregate;
        prefix.last_kind = persisted_source.last_kind;
        prefix.last_direction = persisted_source.last_direction;
        prefix.last_evidence = persisted_source.last_evidence;
        prefix.declared_terminal = persisted_source.declared_terminal;
        prefix.declared_cause = persisted_source.declared_cause;
        prefix.observed_progress = persisted_source.observed_progress;
        prefix.historical_incarnations = persisted_source.historical_incarnations;
        prefix.distinct_incarnations =
            persisted_source.historical_incarnations == 0
                ? 0u
                : persisted_source.historical_incarnations + 1u;
        prefix.after_terminal_observations = persisted_source.after_terminal_observations;
        prefix.folded_observations = persisted_source.accepted_observations;
        binding.ledger.seed(prefix);
        binding.restored = true;
        binding.reconfirmed = false;
        binding.refused_observations = persisted_source.refused_observations;
        generation.bindings.push_back(std::move(binding));

        if (sources.find(persisted_source.source) == sources.end()) {
          SourceState state;
          state.descriptor.id = persisted_source.source;
          state.descriptor.canonical_key = persisted_source.canonical_key;
          state.descriptor.authority = persisted_source.authority;
          state.descriptor.capabilities = persisted_source.capabilities;
          state.descriptor.advisory_only = persisted_source.advisory_only;
          state.registered_at = opened_at;
          sources.emplace(persisted_source.source, std::move(state));
        }
      }
      flow.generations.push_back(std::move(generation));
    }
    if (flow.generations.empty() && persisted.current_generation.valid()) {
      return Status(ErrorCode::IntegrityFailure,
                    "persisted flow declares a current generation but records none");
    }
    flows.emplace(flow.id, std::move(flow));
  }

  // Replay accepted evidence in log order. The evaluation instant for each step
  // is the instant the evidence was originally received, so the replayed
  // history is the history the previous run computed.
  replaying = true;
  replay_epoch = previous_epoch;
  for (const Observation& observation : result.observations) {
    replay_now = observation.received_at;
    const Result<IngestOutcome> applied = apply_locked(observation);
    (void)applied;
  }
  replaying = false;
  replay_now = Timestamp{};
  replay_epoch = RuntimeEpoch{};

  // Settle every flow at the current instant. Restored generations have no
  // reconfirmed source, so they can only be Observed, Idle or Expired - never
  // Active. This is the mechanism that stops a restart from resurrecting
  // liveness.
  const Timestamp at = now();
  for (auto& [id, flow] : flows) {
    (void)id;
    recompute_flow(flow, at, config.limits);
  }
  return make_ok();
}

Status Engine::Impl::start_workers_locked() {
  if (workers_running || config.limits.worker_threads == 0) {
    return make_ok();
  }
  stop.store(false, std::memory_order_relaxed);
  workers.reserve(config.limits.worker_threads);
  for (std::uint32_t i = 0; i < config.limits.worker_threads; ++i) {
    workers.emplace_back([this]() {
      for (;;) {
        std::vector<Observation> batch;
        {
          detail::QueueGuard queue_lock(queue_mutex);
          queue_cv.wait(queue_lock.raw(), [this]() {
            return stop.load(std::memory_order_relaxed) || !queue.empty();
          });
          if (queue.empty()) {
            if (stop.load(std::memory_order_relaxed)) {
              return;
            }
            continue;
          }
          std::size_t take = config.limits.max_ingest_batch;
          if (take > queue.size()) {
            take = queue.size();
          }
          batch.reserve(take);
          for (std::size_t index = 0; index < take; ++index) {
            batch.push_back(std::move(queue.front()));
            queue.pop_front();
          }
        }
        queue_space_cv.notify_all();
        if (batch.empty()) {
          continue;
        }
        detail::EngineMutexGuard engine_lock(mutex);
        for (const Observation& observation : batch) {
          const Result<IngestOutcome> applied = apply_locked(observation);
          (void)applied;
        }
        counters.worker_batches.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  workers_running = true;
  return make_ok();
}

Status Engine::Impl::stop_workers_locked() {
  // The caller must NOT hold the engine mutex: a worker may be waiting for it.
  stop.store(true, std::memory_order_relaxed);
  queue_cv.notify_all();
  queue_space_cv.notify_all();
  for (std::thread& worker : workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers.clear();
  workers_running = false;
  return make_ok();
}

TickReport Engine::Impl::tick_locked(Timestamp evaluated_at) {
  TickReport report;
  report.evaluated_at = evaluated_at;
  evaluation_override = evaluated_at;
  const std::uint64_t expired_before = counters.flows_expired.load(std::memory_order_relaxed);
  std::vector<LifecycleEvent> events;
  for (auto& [id, flow] : flows) {
    (void)id;
    ++report.flows_examined;
    if (!flow.generations.empty()) {
      bool all_terminal = true;
      for (const GenerationRuntime& generation : flow.generations) {
        if (!is_terminal(generation.state)) {
          all_terminal = false;
          break;
        }
      }
      if (all_terminal) {
        continue;
      }
    }
    recompute_flow(flow, evaluated_at, config.limits, &events);
  }
  evaluation_override = Timestamp{};
  counters.ticks.fetch_add(1, std::memory_order_relaxed);
  report.expired = static_cast<std::uint32_t>(
      counters.flows_expired.load(std::memory_order_relaxed) - expired_before);
  report.transitions = static_cast<std::uint32_t>(
      events.size() > 0xFFFFFFFFull ? 0xFFFFFFFFull : events.size());
  for (const LifecycleEvent& event : events) {
    if (event.to == FlowState::Idle) {
      ++report.became_idle;
    }
  }
  if (events.size() > config.limits.max_result_set) {
    events.resize(config.limits.max_result_set);
  }
  report.events = std::move(events);
  return report;
}

}  // namespace flowobs
