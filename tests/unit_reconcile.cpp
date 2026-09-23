// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <vector>

#include "flowobs/flowobs.hpp"
#include "fotest.hpp"
#include "support.hpp"

using namespace flowobs;
using fotest::ObservationSpec;
using fotest::Rng;
using fotest::base_time;
using fotest::make_observation;
using fotest::make_source;

namespace {

Observation make(std::uint64_t revision, std::optional<std::uint64_t> bytes,
                 Direction direction = Direction::Forward,
                 Evidence evidence = Evidence::Complete,
                 ObservationKind kind = ObservationKind::Sample,
                 std::uint64_t incarnation = 1, std::uint64_t epoch = 1,
                 const std::string& flow = "flow-1", std::uint64_t generation = 1) {
  ObservationSpec spec;
  spec.flow = flow;
  spec.generation = generation;
  spec.revision = revision;
  spec.sequence = revision;
  spec.incarnation = incarnation;
  spec.epoch = epoch;
  spec.bytes = bytes;
  spec.direction = direction;
  spec.evidence = evidence;
  spec.kind = kind;
  return make_observation(spec);
}

FreshnessContext recent_context() {
  FreshnessContext fctx;
  fctx.now = base_time() + Duration::from_seconds(1);
  fctx.policy.fresh_window = Duration::from_seconds(30);
  fctx.policy.expire_window = Duration::from_seconds(120);
  return fctx;
}

}  // namespace

FOTEST("ledger", "duplicate_is_idempotent") {
  const FlowGeneration generation(flow_id_from_key("flow-1"), GenerationId::from_value(1));
  SourceLedger ledger(source_id_from_key("src-a"), generation, 16);
  const Observation observation = make(1, 100);
  FO_CHECK_EQ(static_cast<int>(ledger.insert(observation).kind),
              static_cast<int>(SourceLedger::InsertResult::Kind::Accepted));
  FO_CHECK_EQ(static_cast<int>(ledger.insert(observation).kind),
              static_cast<int>(SourceLedger::InsertResult::Kind::Duplicate));
  FO_CHECK_EQ(ledger.size(), 1u);
  FO_CHECK_EQ(ledger.duplicates_suppressed(), 1ull);
  FO_CHECK_EQ(ledger.total_accepted(), 1ull);
}

FOTEST("ledger", "same_key_different_content_is_a_conflict") {
  const FlowGeneration generation(flow_id_from_key("flow-1"), GenerationId::from_value(1));
  SourceLedger ledger(source_id_from_key("src-a"), generation, 16);
  Observation first = make(1, 100);
  Observation second = make(1, 200);  // identical key, different content
  FO_CHECK_EQ(static_cast<int>(ledger.insert(first).kind),
              static_cast<int>(SourceLedger::InsertResult::Kind::Accepted));
  FO_CHECK_EQ(static_cast<int>(ledger.insert(second).kind),
              static_cast<int>(SourceLedger::InsertResult::Kind::ContentConflict));
  FO_CHECK_EQ(ledger.content_conflicts(), 1ull);
  FO_CHECK_EQ(ledger.size(), 1u);
  // The retained copy is the one with the smaller canonical digest, whichever
  // order the two arrived in.
  const std::uint64_t retained = content_digest(ledger.entries().front());
  FO_CHECK_EQ(retained, std::min(content_digest(first), content_digest(second)));

  SourceLedger reversed(source_id_from_key("src-a"), generation, 16);
  FO_CHECK_EQ(static_cast<int>(reversed.insert(second).kind),
              static_cast<int>(SourceLedger::InsertResult::Kind::Accepted));
  FO_CHECK_EQ(static_cast<int>(reversed.insert(first).kind),
              static_cast<int>(SourceLedger::InsertResult::Kind::ContentConflict));
  FO_CHECK_EQ(content_digest(reversed.entries().front()), retained);
}

FOTEST("ledger", "rejects_evidence_for_another_subject") {
  const FlowGeneration generation(flow_id_from_key("flow-1"), GenerationId::from_value(1));
  SourceLedger ledger(source_id_from_key("src-a"), generation, 16);
  const Observation other_flow = make(1, 10, Direction::Forward, Evidence::Complete,
                                      ObservationKind::Sample, 1, 1, "flow-2");
  FO_CHECK_EQ(static_cast<int>(ledger.insert(other_flow).kind),
              static_cast<int>(SourceLedger::InsertResult::Kind::Refused));
  const Observation other_source = make(1, 10, Direction::Forward, Evidence::Complete,
                                        ObservationKind::Sample, 1, 1, "flow-1", 1);
  Observation wrong_source = other_source;
  wrong_source.key.source = source_id_from_key("src-b");
  FO_CHECK_EQ(static_cast<int>(ledger.insert(wrong_source).kind),
              static_cast<int>(SourceLedger::InsertResult::Kind::Refused));
}

FOTEST("ledger", "folding_preserves_accumulated_accounting") {
  const FlowGeneration generation(flow_id_from_key("flow-1"), GenerationId::from_value(1));
  SourceLedger small(source_id_from_key("src-a"), generation, 2);
  SourceLedger large(source_id_from_key("src-a"), generation, 4096);
  for (std::uint64_t revision = 1; revision <= 64; ++revision) {
    const Observation observation = make(revision, revision * 100);
    small.insert(observation);
    large.insert(observation);
  }
  FO_CHECK(small.prefix().folded_observations > 0);
  SourceDescriptor descriptor = make_source("src-a", 1);
  const SourceView small_view =
      fold_source_view(small, descriptor, recent_context(), CounterPolicy{});
  const SourceView large_view =
      fold_source_view(large, descriptor, recent_context(), CounterPolicy{});
  // The whole point of folding rather than dropping: a bounded ledger yields the
  // same accounting as an unbounded one.
  FO_CHECK_EQ(small_view.forward_totals.bytes, large_view.forward_totals.bytes);
  FO_CHECK_EQ(small_view.forward_totals.bytes, 6300ull);
  FO_CHECK_EQ(small_view.forward_totals.unknown_intervals, 0ull);
}

FOTEST("ledger", "evidence_inside_the_folded_window_is_refused") {
  const FlowGeneration generation(flow_id_from_key("flow-1"), GenerationId::from_value(1));
  SourceLedger ledger(source_id_from_key("src-a"), generation, 2);
  for (std::uint64_t revision = 1; revision <= 100; ++revision) {
    ledger.insert(make(revision, revision * 100));
  }
  FO_REQUIRE(ledger.prefix().folded_observations > 0);
  const std::uint64_t before = ledger.prefix().folded_observations;
  // A redelivery of evidence that was already folded is still recognised
  // exactly, so a retransmitted record costs nothing.
  const SourceLedger::InsertResult late = ledger.insert(make(80, 8000));
  FO_CHECK_EQ(static_cast<int>(late.kind),
              static_cast<int>(SourceLedger::InsertResult::Kind::Duplicate));
  FO_CHECK_EQ(ledger.late_rejected(), 0ull);
  // Something that was never seen and cannot be ordered is refused rather than
  // applied out of order, and the refusal is counted.
  Observation late_unseen = make(10, 555);
  late_unseen.observed_at = Timestamp::from_nanos(base_time().nanos() + 2);
  const SourceLedger::InsertResult refused = ledger.insert(late_unseen);
  FO_CHECK_EQ(static_cast<int>(refused.kind),
              static_cast<int>(SourceLedger::InsertResult::Kind::Refused));
  FO_CHECK_EQ(static_cast<int>(refused.code), static_cast<int>(ErrorCode::StaleRevision));
  FO_CHECK_EQ(ledger.late_rejected(), 1ull);
  FO_CHECK(ledger.prefix().folded_observations >= before);
}

FOTEST("fold", "direction_and_coverage") {
  const FlowGeneration generation(flow_id_from_key("flow-1"), GenerationId::from_value(1));
  SourceDescriptor descriptor = make_source("src-a", 1);

  // Forward only: one-sided, and one-sided is not bidirectional truth.
  SourceLedger forward_only(source_id_from_key("src-a"), generation, 32);
  forward_only.insert(make(1, 100, Direction::Forward));
  forward_only.insert(make(2, 200, Direction::Forward));
  const SourceView one_sided =
      fold_source_view(forward_only, descriptor, recent_context(), CounterPolicy{});
  FO_CHECK(one_sided.coverage.forward_complete);
  FO_CHECK(!one_sided.coverage.reverse_complete);
  FO_CHECK(!one_sided.coverage.bidirectional());
  FO_CHECK_EQ(static_cast<int>(one_sided.coverage.classify()),
              static_cast<int>(Visibility::OneSided));
  FO_CHECK_EQ(one_sided.forward_totals.bytes, 100ull);
  FO_CHECK_EQ(one_sided.reverse_totals.bytes, 0ull);

  // Both directions observed: now the runtime may call it two sided.
  SourceLedger both(source_id_from_key("src-a"), generation, 32);
  both.insert(make(1, 100, Direction::Forward));
  both.insert(make(2, 200, Direction::Forward));
  both.insert(make(3, 50, Direction::Reverse));
  const SourceView two_sided =
      fold_source_view(both, descriptor, recent_context(), CounterPolicy{});
  FO_CHECK(two_sided.coverage.forward_complete);
  FO_CHECK(two_sided.coverage.reverse_complete);
  FO_CHECK(two_sided.coverage.bidirectional());
  FO_CHECK_EQ(static_cast<int>(two_sided.coverage.classify()),
              static_cast<int>(Visibility::TwoSided));
}

FOTEST("fold", "aggregate_reading_never_proves_bidirectional") {
  const FlowGeneration generation(flow_id_from_key("flow-1"), GenerationId::from_value(1));
  SourceLedger ledger(source_id_from_key("src-a"), generation, 32);
  ledger.insert(make(1, 100, Direction::Aggregate));
  ledger.insert(make(2, 250, Direction::Aggregate));
  SourceDescriptor descriptor = make_source("src-a", 1);
  const SourceView view = fold_source_view(ledger, descriptor, recent_context(), CounterPolicy{});
  FO_CHECK(!view.coverage.forward_complete);
  FO_CHECK(!view.coverage.reverse_complete);
  FO_CHECK(!view.coverage.bidirectional());
  FO_CHECK_EQ(static_cast<int>(view.coverage.classify()), static_cast<int>(Visibility::Unknown));
  FO_CHECK_EQ(view.aggregate_totals.bytes, 150ull);
  FO_CHECK(view.incomplete_events > 0);
}

FOTEST("fold", "incarnation_restart_breaks_the_interval") {
  const FlowGeneration generation(flow_id_from_key("flow-1"), GenerationId::from_value(1));
  SourceLedger ledger(source_id_from_key("src-a"), generation, 32);
  ledger.insert(make(1, 1000, Direction::Forward, Evidence::Complete,
                     ObservationKind::Sample, 1));
  ledger.insert(make(2, 1400, Direction::Forward, Evidence::Complete,
                     ObservationKind::Sample, 1));
  // The source restarted: its counters may have continued or restarted, and the
  // runtime refuses to guess.
  ledger.insert(make(1, 10, Direction::Forward, Evidence::Complete,
                     ObservationKind::Sample, 2));
  ledger.insert(make(2, 40, Direction::Forward, Evidence::Complete,
                     ObservationKind::Sample, 2));
  SourceDescriptor descriptor = make_source("src-a", 1);
  const SourceView view = fold_source_view(ledger, descriptor, recent_context(), CounterPolicy{});
  FO_CHECK_EQ(view.forward_totals.bytes, 430ull);
  FO_CHECK_EQ(view.forward_totals.unknown_intervals, 1ull);
  FO_CHECK_EQ(view.distinct_incarnations, 2u);
  FO_CHECK(view.restart_gaps > 0);
}

FOTEST("fold", "declared_terminal_stops_the_fold") {
  const FlowGeneration generation(flow_id_from_key("flow-1"), GenerationId::from_value(1));
  SourceLedger ledger(source_id_from_key("src-a"), generation, 32);
  ledger.insert(make(1, 100));
  ledger.insert(make(2, 200, Direction::Forward, Evidence::Complete, ObservationKind::Close));
  ledger.insert(make(3, 900));
  SourceDescriptor descriptor = make_source("src-a", 1);
  const SourceView view = fold_source_view(ledger, descriptor, recent_context(), CounterPolicy{});
  FO_CHECK_EQ(view.forward_totals.bytes, 100ull);
  FO_REQUIRE(view.declared_terminal.has_value());
  FO_CHECK_EQ(static_cast<int>(*view.declared_terminal), static_cast<int>(FlowState::Completed));
  FO_CHECK(view.after_terminal_observations >= 1u);
}

FOTEST("reconcile", "authority_order_is_total_and_stable") {
  std::vector<SourceView> views(3);
  views[0].source = source_id_from_key("src-c");
  views[0].authority = 5;
  views[1].source = source_id_from_key("src-a");
  views[1].authority = 5;
  views[2].source = source_id_from_key("src-b");
  views[2].authority = 9;
  sort_source_views(views);
  FO_CHECK_EQ(views[0].source, source_id_from_key("src-b"));
  FO_CHECK_EQ(views[1].source, source_id_from_key("src-a"));
  FO_CHECK_EQ(views[2].source, source_id_from_key("src-c"));
}

FOTEST("reconcile", "equal_authority_disagreement_is_conflicting") {
  const FlowId flow = flow_id_from_key("flow-1");
  std::vector<SourceView> views(2);
  views[0].source = source_id_from_key("src-a");
  views[0].authority = 5;
  views[0].freshness = Freshness::Fresh;
  views[0].reconfirmed = true;
  views[0].forward.bytes.observe(0, true);
  views[0].forward.bytes.observe(1000, true);
  views[1].source = source_id_from_key("src-b");
  views[1].authority = 5;
  views[1].freshness = Freshness::Fresh;
  views[1].reconfirmed = true;
  views[1].forward.bytes.observe(0, true);
  views[1].forward.bytes.observe(5000, true);
  const GenerationView generation = reconcile_generation(
      flow, GenerationId::from_value(1), views, ReconciliationPolicy{}, true);
  FO_CHECK_EQ(static_cast<int>(generation.consistency), static_cast<int>(Consistency::Conflicting));
  FO_CHECK(generation.unresolved_conflict);
  FO_CHECK_EQ(generation.primary_source, source_id_from_key("src-a"));
}

FOTEST("reconcile", "higher_authority_resolves") {
  const FlowId flow = flow_id_from_key("flow-1");
  std::vector<SourceView> views(3);
  views[0].source = source_id_from_key("src-a");
  views[0].authority = 5;
  views[0].freshness = Freshness::Fresh;
  views[0].reconfirmed = true;
  views[0].forward.bytes.observe(0, true);
  views[0].forward.bytes.observe(1000, true);
  views[1].source = source_id_from_key("src-b");
  views[1].authority = 5;
  views[1].freshness = Freshness::Fresh;
  views[1].reconfirmed = true;
  views[1].forward.bytes.observe(0, true);
  views[1].forward.bytes.observe(5000, true);
  views[2].source = source_id_from_key("src-c");
  views[2].authority = 9;
  views[2].freshness = Freshness::Fresh;
  views[2].reconfirmed = true;
  views[2].forward.bytes.observe(0, true);
  views[2].forward.bytes.observe(4000, true);
  ReconciliationPolicy policy;
  policy.higher_authority_resolves = true;
  const GenerationView generation =
      reconcile_generation(flow, GenerationId::from_value(1), views, policy, true);
  FO_CHECK_EQ(static_cast<int>(generation.consistency), static_cast<int>(Consistency::Resolved));
  FO_CHECK(!generation.unresolved_conflict);
  FO_CHECK_EQ(generation.primary_source, source_id_from_key("src-c"));
  FO_CHECK_EQ(generation.total.bytes, 4000ull);
}

FOTEST("reconcile", "corroboration_within_tolerance") {
  const FlowId flow = flow_id_from_key("flow-1");
  std::vector<SourceView> views(2);
  views[0].source = source_id_from_key("src-a");
  views[0].authority = 5;
  views[0].freshness = Freshness::Fresh;
  views[0].reconfirmed = true;
  views[0].forward.bytes.observe(0, true);
  views[0].forward.bytes.observe(10000, true);
  views[1].source = source_id_from_key("src-b");
  views[1].authority = 5;
  views[1].freshness = Freshness::Fresh;
  views[1].reconfirmed = true;
  views[1].forward.bytes.observe(0, true);
  views[1].forward.bytes.observe(10040, true);
  ReconciliationPolicy policy;
  policy.absolute_tolerance_bytes = 100;
  const GenerationView generation =
      reconcile_generation(flow, GenerationId::from_value(1), views, policy, true);
  FO_CHECK_EQ(static_cast<int>(generation.consistency),
              static_cast<int>(Consistency::Corroborated));
}

FOTEST("reconcile", "stale_sources_are_excluded_from_the_current_view") {
  const FlowId flow = flow_id_from_key("flow-1");
  std::vector<SourceView> views(1);
  views[0].source = source_id_from_key("src-a");
  views[0].authority = 5;
  views[0].freshness = Freshness::Stale;
  views[0].reconfirmed = true;
  views[0].forward.bytes.observe(0, true);
  views[0].forward.bytes.observe(1000, true);
  const GenerationView generation = reconcile_generation(
      flow, GenerationId::from_value(1), views, ReconciliationPolicy{}, true);
  FO_CHECK_EQ(generation.excluded_stale_sources, 1u);
  FO_CHECK_EQ(static_cast<int>(generation.freshness), static_cast<int>(Freshness::Stale));
  // The durable accounting is still reported; only its currency is denied.
  FO_CHECK_EQ(generation.total.bytes, 1000ull);
}

FOTEST("state", "idle_is_not_completion_and_completion_needs_capability") {
  const FlowId flow = flow_id_from_key("flow-1");
  FreshnessContext fctx = recent_context();
  ExpiryPolicy expiry;
  expiry.idle_after = Duration::from_seconds(10);
  expiry.expire_after = Duration::from_seconds(60);

  GenerationView view;
  view.generation = FlowGeneration(flow, GenerationId::from_value(1));
  view.is_current = true;
  view.freshness = Freshness::Fresh;
  SourceView source;
  source.source = source_id_from_key("src-a");
  source.authority = 1;
  source.freshness = Freshness::Fresh;
  source.reconfirmed = true;
  source.observed_progress = true;
  source.last_progress_at = fctx.now;
  source.declared_terminal = FlowState::Completed;
  source.capabilities.asserts_completion = false;
  view.sources.push_back(source);
  StateDecision decision = decide_generation_state(view, FlowState::Observed, expiry, fctx);
  FO_CHECK_EQ(static_cast<int>(decision.state), static_cast<int>(FlowState::Active));

  view.sources[0].capabilities.asserts_completion = true;
  decision = decide_generation_state(view, FlowState::Observed, expiry, fctx);
  FO_CHECK_EQ(static_cast<int>(decision.state), static_cast<int>(FlowState::Completed));
  FO_CHECK_EQ(static_cast<int>(decision.cause),
              static_cast<int>(StateCause::CompletionObserved));
}

FOTEST("state", "idle_then_expired_follows_the_policy") {
  const FlowId flow = flow_id_from_key("flow-1");
  ExpiryPolicy expiry;
  expiry.idle_after = Duration::from_seconds(10);
  expiry.expire_after = Duration::from_seconds(60);
  GenerationView view;
  view.generation = FlowGeneration(flow, GenerationId::from_value(1));
  view.is_current = true;
  SourceView source;
  source.source = source_id_from_key("src-a");
  source.freshness = Freshness::Fresh;
  source.reconfirmed = true;
  source.observed_progress = true;
  source.last_progress_at = base_time();
  source.last_observed_at = base_time();
  view.sources.push_back(source);
  view.last_progress_at = base_time();
  view.last_observed_at = base_time();

  FreshnessContext fctx = recent_context();
  fctx.now = base_time() + Duration::from_seconds(11);
  StateDecision decision = decide_generation_state(view, FlowState::Active, expiry, fctx);
  FO_CHECK_EQ(static_cast<int>(decision.state), static_cast<int>(FlowState::Idle));

  fctx.now = base_time() + Duration::from_seconds(61);
  view.sources[0].freshness = Freshness::Expired;
  view.freshness = Freshness::Expired;
  decision = decide_generation_state(view, FlowState::Idle, expiry, fctx);
  FO_CHECK_EQ(static_cast<int>(decision.state), static_cast<int>(FlowState::Expired));
  FO_CHECK_EQ(static_cast<int>(decision.cause), static_cast<int>(StateCause::ExpiryPolicy));
  FO_CHECK(!asserts_completion(decision.state));
}

FOTEST("state", "terminal_states_are_never_reopened") {
  const FlowId flow = flow_id_from_key("flow-1");
  FreshnessContext fctx = recent_context();
  ExpiryPolicy expiry;
  GenerationView view;
  view.generation = FlowGeneration(flow, GenerationId::from_value(1));
  SourceView source;
  source.source = source_id_from_key("src-a");
  source.freshness = Freshness::Fresh;
  source.reconfirmed = true;
  source.observed_progress = true;
  source.last_progress_at = fctx.now;
  view.sources.push_back(source);
  for (const FlowState terminal :
       {FlowState::Completed, FlowState::Reset, FlowState::Expired}) {
    const StateDecision decision = decide_generation_state(view, terminal, expiry, fctx);
    FO_CHECK_EQ(static_cast<int>(decision.state), static_cast<int>(terminal));
    FO_CHECK(!decision.changed);
  }
}

FOTEST("state", "restored_evidence_is_never_active") {
  const FlowId flow = flow_id_from_key("flow-1");
  FreshnessContext fctx = recent_context();
  ExpiryPolicy expiry;
  GenerationView view;
  view.generation = FlowGeneration(flow, GenerationId::from_value(1));
  SourceView source;
  source.source = source_id_from_key("src-a");
  source.freshness = Freshness::Unknown;
  source.reconfirmed = false;
  source.restored_from_journal = true;
  source.observed_progress = true;
  source.last_progress_at = fctx.now;
  source.last_observed_at = fctx.now;
  view.sources.push_back(source);
  view.last_observed_at = fctx.now;
  view.last_progress_at = fctx.now;
  const StateDecision decision = decide_generation_state(view, FlowState::Active, expiry, fctx);
  FO_CHECK_EQ(static_cast<int>(decision.state), static_cast<int>(FlowState::Observed));
  FO_CHECK_EQ(static_cast<int>(decision.cause),
              static_cast<int>(StateCause::RecoveredFromJournal));
}

FOTEST("attribution", "endpoint_only_never_invents_hops") {
  FlowSnapshot flow;
  flow.id = flow_id_from_key("flow-1");
  flow.state = FlowState::Active;
  GenerationView generation;
  generation.generation = FlowGeneration(flow.id, GenerationId::from_value(1));
  generation.is_current = true;
  generation.freshness = Freshness::Fresh;
  generation.endpoint_a = endpoint_id_from_key("ep-a");
  generation.endpoint_b = endpoint_id_from_key("ep-b");
  generation.total.bytes = 1000;
  generation.forward.bytes = 1000;
  generation.links = {link_id_from_key("l0"), link_id_from_key("l1")};
  AttributionPolicy policy;
  policy.method = AttributionPolicy::Method::EndpointOnly;
  policy.require_fresh_generation = true;
  const AttributionReport report = attribute_generation(flow, generation, policy, 64);
  FO_CHECK(report.ok());
  FO_CHECK_EQ(report.targets.size(), 3u);
  FO_CHECK_EQ(static_cast<int>(report.targets[0].kind),
              static_cast<int>(AttributedTarget::Kind::Flow));
  FO_CHECK_EQ(report.targets[1].consumption.bytes, 1000ull);
}

FOTEST("attribution", "uniform_split_sums_exactly") {
  FlowSnapshot flow;
  flow.id = flow_id_from_key("flow-1");
  GenerationView generation;
  generation.generation = FlowGeneration(flow.id, GenerationId::from_value(1));
  generation.is_current = true;
  generation.freshness = Freshness::Fresh;
  generation.total.bytes = 1000;
  generation.total.packets = 7;
  generation.path = path_id_from_key("p0");
  for (int i = 0; i < 3; ++i) {
    generation.links.push_back(link_id_from_key("l" + std::to_string(i)));
  }
  AttributionPolicy policy;
  policy.method = AttributionPolicy::Method::UniformAcrossHops;
  const AttributionReport report = attribute_generation(flow, generation, policy, 64);
  FO_REQUIRE(report.ok());
  std::uint64_t byte_sum = 0;
  std::uint64_t packet_sum = 0;
  for (const AttributedTarget& target : report.targets) {
    if (target.kind != AttributedTarget::Kind::Link) {
      continue;
    }
    byte_sum += target.consumption.bytes;
    packet_sum += target.consumption.packets;
  }
  FO_CHECK_EQ(byte_sum, 1000ull);
  FO_CHECK_EQ(packet_sum, 7ull);
}

FOTEST("attribution", "proportional_split_uses_supplied_capacity") {
  FlowSnapshot flow;
  flow.id = flow_id_from_key("flow-1");
  GenerationView generation;
  generation.generation = FlowGeneration(flow.id, GenerationId::from_value(1));
  generation.is_current = true;
  generation.freshness = Freshness::Fresh;
  generation.total.bytes = 1000;
  generation.path = path_id_from_key("p0");
  generation.links = {link_id_from_key("l0"), link_id_from_key("l1")};
  generation.hop_capacity_units = {3, 1};
  AttributionPolicy policy;
  policy.method = AttributionPolicy::Method::ProportionalToCapacity;
  const AttributionReport report = attribute_generation(flow, generation, policy, 64);
  FO_REQUIRE(report.ok());
  std::uint64_t sum = 0;
  std::uint32_t links = 0;
  for (const AttributedTarget& target : report.targets) {
    if (target.kind == AttributedTarget::Kind::Link) {
      sum += target.consumption.bytes;
      ++links;
    }
  }
  FO_CHECK_EQ(links, 2u);
  FO_CHECK_EQ(sum, 1000ull);
  FO_CHECK_EQ(report.targets[1].capacity_units.value_or(0), 3ull);
}

FOTEST("attribution", "missing_capacity_is_unsupported_not_guessed") {
  FlowSnapshot flow;
  flow.id = flow_id_from_key("flow-1");
  GenerationView generation;
  generation.generation = FlowGeneration(flow.id, GenerationId::from_value(1));
  generation.is_current = true;
  generation.freshness = Freshness::Fresh;
  generation.total.bytes = 1000;
  generation.path = path_id_from_key("p0");
  generation.links = {link_id_from_key("l0"), link_id_from_key("l1")};
  AttributionPolicy policy;
  policy.method = AttributionPolicy::Method::ProportionalToCapacity;
  const AttributionReport report = attribute_generation(flow, generation, policy, 64);
  FO_CHECK_EQ(static_cast<int>(report.status),
              static_cast<int>(AttributionStatus::UnsupportedByMetadata));
}

FOTEST("attribution", "stale_generation_cannot_support_current_attribution") {
  FlowSnapshot flow;
  flow.id = flow_id_from_key("flow-1");
  GenerationView generation;
  generation.generation = FlowGeneration(flow.id, GenerationId::from_value(1));
  generation.is_current = true;
  generation.freshness = Freshness::Stale;
  generation.total.bytes = 1000;
  AttributionPolicy policy;
  const AttributionReport report = attribute_generation(flow, generation, policy, 64);
  FO_CHECK_EQ(static_cast<int>(report.status),
              static_cast<int>(AttributionStatus::StaleGeneration));
  FO_CHECK(report.targets.empty());
}

FOTEST("attribution", "superseded_generation_cannot_support_current_attribution") {
  FlowSnapshot flow;
  flow.id = flow_id_from_key("flow-1");
  GenerationView generation;
  generation.generation = FlowGeneration(flow.id, GenerationId::from_value(1));
  generation.is_current = false;
  generation.freshness = Freshness::Fresh;
  generation.total.bytes = 1000;
  AttributionPolicy policy;
  const AttributionReport report = attribute_generation(flow, generation, policy, 64);
  FO_CHECK_EQ(static_cast<int>(report.status),
              static_cast<int>(AttributionStatus::NotCurrentGeneration));
}
