// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Engine behaviour on a single thread with a manual clock: every state, every
// refusal and every bound is exercised deterministically.

#include <filesystem>
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

struct EngineFixture final {
  ManualClock clock;
  EngineConfig config;
  std::unique_ptr<Engine> engine;

  EngineFixture() {
    config.clock = &clock;
    config.limits.worker_threads = 0;
    config.expiry.idle_after = Duration::from_seconds(10);
    config.expiry.expire_after = Duration::from_seconds(60);
    config.freshness.fresh_window = Duration::from_seconds(10);
    config.freshness.expire_window = Duration::from_seconds(60);
    config.limits.max_clock_skew = Duration::from_seconds(5);
    clock.set(fotest::recent_now());
    engine = std::make_unique<Engine>(config);
  }

  // Recreates the runtime from the current configuration. Limits are read when
  // the engine is constructed, so a test that tightens a bound must rebuild.
  void rebuild() { engine = std::make_unique<Engine>(config); }

  void open(fotest::Context& ctx) { FO_REQUIRE(engine->open().ok()); }

  void register_source(fotest::Context& ctx, const std::string& key, std::uint32_t authority,
                       bool completion = true, bool reset = true) {
    const SourceDescriptor descriptor = make_source(key, authority, completion, reset);
    FO_REQUIRE(engine->register_source(descriptor).ok());
  }

  Result<IngestOutcome> feed(const ObservationSpec& spec) {
    return engine->submit(make_observation(spec));
  }

  Result<FlowSnapshot> snapshot_of(const std::string& flow_key) const {
    return engine->snapshot(flow_id_from_key(flow_key));
  }
};

}  // namespace

FOTEST("engine", "requires_open_and_registers_sources") {
  EngineFixture fixture;
  ObservationSpec spec;
  spec.bytes = 100;
  FO_CHECK_EQ(static_cast<int>(fixture.feed(spec).code()),
              static_cast<int>(ErrorCode::NotOpen));
  fixture.open(ctx);
  FO_REQUIRE(fixture.engine->register_source(make_source("src-a", 10)).ok());
  FO_REQUIRE(fixture.engine->register_source(make_source("src-a", 10)).ok());
  std::vector<SourceDescriptor> sources;
  FO_REQUIRE(fixture.engine->list_sources(sources).ok());
  FO_CHECK_EQ(sources.size(), 1u);
  FO_CHECK_EQ(sources[0].authority, 10u);
  // A descriptor whose key does not hash to its identity is refused.
  SourceDescriptor bad = make_source("src-a", 1);
  bad.id = source_id_from_key("src-b");
  FO_CHECK(!fixture.engine->register_source(bad).ok());
  FO_CHECK(!fixture.engine->describe_source(source_id_from_key("nope")).ok());
}

FOTEST("engine", "first_observation_is_observed_not_active") {
  EngineFixture fixture;
  fixture.open(ctx);
  fixture.register_source(ctx, "src-a", 10);
  ObservationSpec spec;
  spec.bytes = 500;
  spec.kind = ObservationKind::Heartbeat;
  spec.set_counters = false;
  const Result<IngestOutcome> outcome = fixture.feed(spec);
  FO_REQUIRE(outcome.ok());
  FO_CHECK(outcome.value().applied());
  FO_CHECK_EQ(static_cast<int>(outcome.value().state_after),
              static_cast<int>(FlowState::Observed));
  const Result<FlowSnapshot> snapshot = fixture.snapshot_of("flow-1");
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(static_cast<int>(snapshot.value().state), static_cast<int>(FlowState::Observed));
  FO_CHECK(!snapshot.value().is_live());
}

FOTEST("engine", "progress_makes_a_flow_active_and_idle_after_the_window") {
  EngineFixture fixture;
  fixture.open(ctx);
  fixture.register_source(ctx, "src-a", 10);
  ObservationSpec spec;
  spec.bytes = 500;
  FO_REQUIRE(fixture.feed(spec).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 900;
  const Result<IngestOutcome> second = fixture.feed(spec);
  FO_REQUIRE(second.ok());
  FO_CHECK_EQ(static_cast<int>(second.value().state_after), static_cast<int>(FlowState::Active));
  Result<FlowSnapshot> snapshot = fixture.snapshot_of("flow-1");
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(snapshot.value().total.bytes, 400ull);
  FO_CHECK(snapshot.value().is_live());

  fixture.clock.advance(Duration::from_seconds(11));
  const Result<TickReport> ticked = fixture.engine->tick(fixture.clock.now());
  FO_REQUIRE(ticked.ok());
  snapshot = fixture.snapshot_of("flow-1");
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(static_cast<int>(snapshot.value().state), static_cast<int>(FlowState::Idle));
  FO_CHECK(!asserts_completion(snapshot.value().state));

  fixture.clock.advance(Duration::from_seconds(60));
  FO_REQUIRE(fixture.engine->tick(fixture.clock.now()).ok());
  snapshot = fixture.snapshot_of("flow-1");
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(static_cast<int>(snapshot.value().state), static_cast<int>(FlowState::Expired));
  FO_CHECK(!asserts_completion(snapshot.value().state));
}

FOTEST("engine", "completion_requires_evidence_and_capability") {
  EngineFixture fixture;
  fixture.open(ctx);
  fixture.register_source(ctx, "src-a", 10, /*completion=*/false);
  ObservationSpec spec;
  spec.bytes = 100;
  FO_REQUIRE(fixture.feed(spec).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 200;
  spec.kind = ObservationKind::Close;
  const Result<IngestOutcome> refused = fixture.feed(spec);
  FO_REQUIRE(refused.ok());
  Result<FlowSnapshot> snapshot = fixture.snapshot_of("flow-1");
  FO_REQUIRE(snapshot.ok());
  FO_CHECK(!asserts_completion(snapshot.value().state));

  fixture.register_source(ctx, "src-a", 10, /*completion=*/true);
  spec.revision = 3;
  spec.sequence = 3;
  spec.bytes = 300;
  FO_REQUIRE(fixture.feed(spec).ok());
  snapshot = fixture.snapshot_of("flow-1");
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(static_cast<int>(snapshot.value().state), static_cast<int>(FlowState::Completed));
}

FOTEST("engine", "unregistered_source_cannot_declare_completion") {
  EngineFixture fixture;
  fixture.open(ctx);
  ObservationSpec spec;
  spec.bytes = 100;
  FO_REQUIRE(fixture.feed(spec).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 200;
  spec.kind = ObservationKind::Close;
  FO_REQUIRE(fixture.feed(spec).ok());
  const Result<FlowSnapshot> snapshot = fixture.snapshot_of("flow-1");
  FO_REQUIRE(snapshot.ok());
  FO_CHECK(!asserts_completion(snapshot.value().state));
  bool saw_refusal = false;
  for (const Anomaly& anomaly : snapshot.value().anomalies) {
    if (anomaly.kind == AnomalyKind::CapabilityUnsupported) {
      saw_refusal = true;
    }
  }
  FO_CHECK(saw_refusal);
}

FOTEST("engine", "reset_is_terminal_and_leaves_the_gap_unknown") {
  EngineFixture fixture;
  fixture.open(ctx);
  fixture.register_source(ctx, "src-a", 10);
  ObservationSpec spec;
  spec.bytes = 1000;
  FO_REQUIRE(fixture.feed(spec).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 1400;
  FO_REQUIRE(fixture.feed(spec).ok());
  spec.revision = 3;
  spec.sequence = 3;
  spec.bytes = 0;
  spec.kind = ObservationKind::Reset;
  spec.epoch = 2;
  const Result<IngestOutcome> reset = fixture.feed(spec);
  FO_REQUIRE(reset.ok());
  const Result<FlowSnapshot> snapshot = fixture.snapshot_of("flow-1");
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(static_cast<int>(snapshot.value().state), static_cast<int>(FlowState::Reset));
  FO_CHECK_EQ(snapshot.value().total.bytes, 400ull);
  FO_CHECK_EQ(snapshot.value().total.unknown_intervals, 1ull);
  FO_CHECK(!snapshot.value().total.is_exact());
}

FOTEST("engine", "one_sided_visibility_is_never_bidirectional_truth") {
  EngineFixture fixture;
  fixture.open(ctx);
  fixture.register_source(ctx, "src-a", 10);
  ObservationSpec spec;
  spec.bytes = 100;
  spec.direction = Direction::Forward;
  FO_REQUIRE(fixture.feed(spec).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 400;
  FO_REQUIRE(fixture.feed(spec).ok());
  Result<FlowSnapshot> snapshot = fixture.snapshot_of("flow-1");
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(static_cast<int>(snapshot.value().visibility),
              static_cast<int>(Visibility::OneSided));
  FO_CHECK(!snapshot.value().coverage.bidirectional());

  spec.revision = 3;
  spec.sequence = 3;
  spec.bytes = 50;
  spec.direction = Direction::Reverse;
  spec.endpoint = "ep-b";
  spec.peer_endpoint = "ep-a";
  FO_REQUIRE(fixture.feed(spec).ok());
  snapshot = fixture.snapshot_of("flow-1");
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(static_cast<int>(snapshot.value().visibility),
              static_cast<int>(Visibility::TwoSided));
  FO_CHECK(snapshot.value().coverage.bidirectional());
}

FOTEST("engine", "duplicate_delivery_is_idempotent") {
  EngineFixture fixture;
  fixture.open(ctx);
  fixture.register_source(ctx, "src-a", 10);
  ObservationSpec spec;
  spec.bytes = 100;
  FO_REQUIRE(fixture.feed(spec).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 300;
  FO_REQUIRE(fixture.feed(spec).ok());
  const Result<FlowSnapshot> before = fixture.snapshot_of("flow-1");
  FO_REQUIRE(before.ok());
  const std::uint64_t digest_before = fixture.engine->state_digest();

  for (int i = 0; i < 5; ++i) {
    const Result<IngestOutcome> again = fixture.feed(spec);
    FO_REQUIRE(again.ok());
    FO_CHECK(again.value().idempotent());
  }
  const Result<FlowSnapshot> after = fixture.snapshot_of("flow-1");
  FO_REQUIRE(after.ok());
  FO_CHECK_EQ(after.value().total.bytes, before.value().total.bytes);
  FO_CHECK_EQ(fixture.engine->state_digest(), digest_before);
  FO_CHECK_EQ(fixture.engine->stats().observations_duplicate, 5ull);
}

FOTEST("engine", "identity_collision_is_detected_and_reported") {
  EngineFixture fixture;
  fixture.open(ctx);
  ObservationSpec spec;
  spec.bytes = 100;
  FO_REQUIRE(fixture.feed(spec).ok());
  // The same identity presented under a different canonical key: the runtime
  // proves the key hashes to the identity, so a substituted key never reaches
  // the flow table and no second flow is invented.
  Observation impostor = make_observation(spec);
  impostor.subject.flow_key = "flow-1-alias";
  const Result<IngestOutcome> refused = fixture.engine->submit(impostor);
  FO_REQUIRE(refused.ok());
  FO_CHECK_EQ(static_cast<int>(refused.value().disposition),
              static_cast<int>(IngestDisposition::Refused));
  FO_CHECK_EQ(static_cast<int>(refused.value().code),
              static_cast<int>(ErrorCode::InvalidArgument));
  FO_CHECK_EQ(fixture.engine->stats().flows_created, 1ull);
  const Result<FlowSnapshot> snapshot = fixture.snapshot_of("flow-1");
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(static_cast<int>(snapshot.value().state), static_cast<int>(FlowState::Observed));
  FO_CHECK_EQ(snapshot.value().key_collisions, 0ull);
}

FOTEST("engine", "generation_advance_does_not_fabricate_consumption") {
  EngineFixture fixture;
  fixture.open(ctx);
  fixture.register_source(ctx, "src-a", 10);
  ObservationSpec spec;
  spec.bytes = 1000;
  FO_REQUIRE(fixture.feed(spec).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 5000;
  FO_REQUIRE(fixture.feed(spec).ok());
  Result<FlowSnapshot> snapshot = fixture.snapshot_of("flow-1");
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(snapshot.value().total.bytes, 4000ull);

  // A new path generation starts its own accounting.
  spec.generation = 2;
  spec.revision = 1;
  spec.sequence = 1;
  spec.bytes = 10;
  spec.kind = ObservationKind::PathChange;
  FO_REQUIRE(fixture.feed(spec).ok());
  snapshot = fixture.snapshot_of("flow-1");
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(snapshot.value().current_generation.value(), 2ull);
  FO_CHECK_EQ(snapshot.value().total.bytes, 0ull);
  FO_REQUIRE(snapshot.value().generations.size() == 2u);
}

FOTEST("engine", "generation_regression_is_history_not_current_truth") {
  EngineFixture fixture;
  fixture.open(ctx);
  fixture.register_source(ctx, "src-a", 10);
  ObservationSpec spec;
  spec.generation = 1;
  spec.bytes = 100;
  FO_REQUIRE(fixture.feed(spec).ok());
  spec.generation = 2;
  spec.revision = 1;
  spec.sequence = 1;
  spec.bytes = 20;
  FO_REQUIRE(fixture.feed(spec).ok());
  Result<FlowSnapshot> snapshot = fixture.snapshot_of("flow-1");
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(snapshot.value().current_generation.value(), 2ull);

  // Evidence for the superseded generation is retained but cannot drive the
  // current view.
  spec.generation = 1;
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 900;
  const Result<IngestOutcome> stale = fixture.feed(spec);
  FO_REQUIRE(stale.ok());
  snapshot = fixture.snapshot_of("flow-1");
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(snapshot.value().current_generation.value(), 2ull);
  FO_CHECK_EQ(snapshot.value().total.bytes, 0ull);
  bool saw_stale_anomaly = false;
  for (const Anomaly& anomaly : snapshot.value().anomalies) {
    if (anomaly.kind == AnomalyKind::StaleGenerationEvidence) {
      saw_stale_anomaly = true;
    }
  }
  FO_CHECK(saw_stale_anomaly);

  const Result<AttributionReport> attribution = fixture.engine->attribute_current(
      flow_id_from_key("flow-1"));
  FO_REQUIRE(attribution.ok());
  FO_CHECK(attribution.value().ok());
  FO_CHECK_EQ(attribution.value().generation.value(), 2ull);
}

FOTEST("engine", "path_generation_mismatch_is_fenced") {
  EngineFixture fixture;
  fixture.open(ctx);
  ObservationSpec spec;
  spec.bytes = 10;
  spec.generation = 2;
  spec.path_generation = 1;
  const Result<IngestOutcome> refused = fixture.feed(spec);
  FO_REQUIRE(refused.ok());
  FO_CHECK_EQ(static_cast<int>(refused.value().code),
              static_cast<int>(ErrorCode::StaleGeneration));
}

FOTEST("engine", "unsupported_evidence_must_not_carry_values") {
  EngineFixture fixture;
  fixture.open(ctx);
  ObservationSpec spec;
  spec.bytes = 10;
  spec.evidence = Evidence::Unsupported;
  const Result<IngestOutcome> refused = fixture.feed(spec);
  FO_REQUIRE(refused.ok());
  FO_CHECK_EQ(static_cast<int>(refused.value().code),
              static_cast<int>(ErrorCode::InvalidArgument));

  spec.set_counters = false;
  spec.bytes.reset();
  FO_REQUIRE(fixture.feed(spec).ok());
  const Result<FlowSnapshot> snapshot = fixture.snapshot_of("flow-1");
  FO_REQUIRE(snapshot.ok());
  FO_REQUIRE(!snapshot.value().generations.empty());
  // The direction that was never measured says so; the total it contributes to
  // is the sum of what was measured, never a zero standing in for a value.
  FO_CHECK(snapshot.value().generations[0].forward.bytes_unsupported);
  FO_CHECK_EQ(snapshot.value().generations[0].forward.bytes, 0ull);
}

FOTEST("engine", "clock_skew_is_clamped_and_recorded") {
  EngineFixture fixture;
  fixture.open(ctx);
  fixture.register_source(ctx, "src-a", 10);
  ObservationSpec spec;
  spec.bytes = 100;
  spec.observed_at = static_cast<std::uint64_t>(base_time().nanos() + 30000000000LL);
  spec.received_at = static_cast<std::uint64_t>(base_time().nanos());
  FO_REQUIRE(fixture.feed(spec).ok());
  const Result<FlowSnapshot> snapshot = fixture.snapshot_of("flow-1");
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(snapshot.value().last_observed_at.nanos(), base_time().nanos());
  bool saw_skew = false;
  for (const Anomaly& anomaly : snapshot.value().anomalies) {
    if (anomaly.kind == AnomalyKind::ClockSkew) {
      saw_skew = true;
    }
  }
  FO_CHECK(saw_skew);
}

FOTEST("engine", "clock_skew_can_be_refused_outright") {
  EngineFixture fixture;
  fixture.config.freshness.clamp_future_timestamps = false;
  fixture.rebuild();
  fixture.open(ctx);
  ObservationSpec spec;
  spec.bytes = 100;
  spec.observed_at = static_cast<std::uint64_t>(base_time().nanos() + 30000000000LL);
  spec.received_at = static_cast<std::uint64_t>(base_time().nanos());
  const Result<IngestOutcome> refused = fixture.feed(spec);
  FO_REQUIRE(refused.ok());
  FO_CHECK_EQ(static_cast<int>(refused.value().disposition),
              static_cast<int>(IngestDisposition::Refused));
  FO_CHECK_EQ(static_cast<int>(refused.value().code), static_cast<int>(ErrorCode::OutOfRange));
}

FOTEST("engine", "bounds_are_enforced_with_explicit_refusals") {
  EngineFixture fixture;
  fixture.config.limits.max_sources_per_flow = 1;
  fixture.config.limits.max_generations_per_flow = 1;
  fixture.rebuild();
  fixture.open(ctx);
  ObservationSpec spec;
  spec.bytes = 10;
  FO_REQUIRE(fixture.feed(spec).ok());

  spec.source = "src-b";
  spec.revision = 1;
  spec.sequence = 1;
  const Result<IngestOutcome> too_many_sources = fixture.feed(spec);
  FO_REQUIRE(too_many_sources.ok());
  FO_CHECK_EQ(static_cast<int>(too_many_sources.value().code),
              static_cast<int>(ErrorCode::LimitExceeded));

  spec.source = "src-a";
  spec.generation = 2;
  spec.revision = 1;
  spec.sequence = 1;
  const Result<IngestOutcome> too_many_generations = fixture.feed(spec);
  FO_REQUIRE(too_many_generations.ok());
  FO_CHECK_EQ(static_cast<int>(too_many_generations.value().code),
              static_cast<int>(ErrorCode::LimitExceeded));

  Observation oversized = make_observation(spec);
  oversized.subject.hop_capacity_units.assign(4096, 1);
  const Result<IngestOutcome> refused = fixture.engine->submit(oversized);
  FO_REQUIRE(refused.ok());
  FO_CHECK_EQ(static_cast<int>(refused.value().code),
              static_cast<int>(ErrorCode::LimitExceeded));
}

FOTEST("engine", "flow_table_eviction_prefers_terminal_flows") {
  EngineFixture fixture;
  fixture.config.limits.max_flows = 2;
  fixture.rebuild();
  fixture.open(ctx);
  fixture.register_source(ctx, "src-a", 10);
  ObservationSpec spec;
  spec.bytes = 10;
  spec.flow = "flow-a";
  FO_REQUIRE(fixture.feed(spec).ok());
  spec.flow = "flow-b";
  FO_REQUIRE(fixture.feed(spec).ok());
  spec.flow = "flow-c";
  const Result<IngestOutcome> refused = fixture.feed(spec);
  FO_REQUIRE(refused.ok());
  FO_CHECK_EQ(static_cast<int>(refused.value().code),
              static_cast<int>(ErrorCode::LimitExceeded));

  // Completing one flow makes room without discarding live evidence.
  spec.flow = "flow-a";
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 20;
  spec.kind = ObservationKind::Close;
  FO_REQUIRE(fixture.feed(spec).ok());
  spec.flow = "flow-c";
  spec.revision = 1;
  spec.sequence = 1;
  const Result<IngestOutcome> admitted = fixture.feed(spec);
  FO_REQUIRE(admitted.ok());
  FO_CHECK_EQ(static_cast<int>(admitted.value().disposition),
              static_cast<int>(IngestDisposition::Applied));
  FO_CHECK_EQ(fixture.engine->stats().flows_evicted, 1ull);
}

FOTEST("engine", "freeze_refuses_writes_and_keeps_reads_working") {
  EngineFixture fixture;
  fixture.open(ctx);
  ObservationSpec spec;
  spec.bytes = 10;
  FO_REQUIRE(fixture.feed(spec).ok());
  fixture.engine->freeze();
  FO_CHECK(fixture.engine->frozen());
  const Result<IngestOutcome> refused = fixture.feed(spec);
  FO_REQUIRE(!refused.ok());
  FO_CHECK_EQ(static_cast<int>(refused.code()), static_cast<int>(ErrorCode::ShuttingDown));
  FO_CHECK(fixture.snapshot_of("flow-1").ok());
}

FOTEST("engine", "queue_is_bounded_and_reports_drops") {
  EngineFixture fixture;
  fixture.config.limits.max_pending_observations = 2;
  fixture.config.limits.max_ingest_batch = 1;
  fixture.rebuild();
  fixture.open(ctx);
  ObservationSpec spec;
  spec.bytes = 0;
  FO_REQUIRE(fixture.engine->enqueue(make_observation(spec)).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 10;
  FO_REQUIRE(fixture.engine->enqueue(make_observation(spec)).ok());
  spec.revision = 3;
  spec.sequence = 3;
  spec.bytes = 20;
  const Result<IngestOutcome> dropped = fixture.engine->enqueue(make_observation(spec));
  FO_REQUIRE(dropped.ok());
  FO_CHECK_EQ(static_cast<int>(dropped.value().disposition),
              static_cast<int>(IngestDisposition::DroppedQueueFull));
  FO_CHECK_EQ(fixture.engine->queue_depth(), 2u);
  FO_REQUIRE(fixture.engine->drain(8).ok());
  FO_CHECK_EQ(fixture.engine->queue_depth(), 0u);
  const Result<FlowSnapshot> snapshot = fixture.snapshot_of("flow-1");
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(snapshot.value().total.bytes, 10ull);
}

FOTEST("engine", "retiring_a_source_removes_its_accounting") {
  EngineFixture fixture;
  fixture.open(ctx);
  fixture.register_source(ctx, "src-a", 10);
  fixture.register_source(ctx, "src-b", 5);
  ObservationSpec spec;
  spec.source = "src-a";
  spec.bytes = 0;
  FO_REQUIRE(fixture.feed(spec).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 100;
  FO_REQUIRE(fixture.feed(spec).ok());
  spec.source = "src-b";
  spec.revision = 1;
  spec.sequence = 1;
  spec.bytes = 0;
  FO_REQUIRE(fixture.feed(spec).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 40;
  FO_REQUIRE(fixture.feed(spec).ok());
  Result<FlowSnapshot> snapshot = fixture.snapshot_of("flow-1");
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(snapshot.value().total.bytes, 100ull);

  // Retiring the authoritative source leaves only the lower-authority view.
  FO_REQUIRE(fixture.engine->retire_source(source_id_from_key("src-a")).ok());
  snapshot = fixture.snapshot_of("flow-1");
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(snapshot.value().total.bytes, 40ull);
  FO_CHECK_EQ(snapshot.value().generations.at(0).sources.size(), 1u);
  ObservationSpec after;
  after.source = "src-a";
  after.bytes = 10;
  const Result<IngestOutcome> refused = fixture.feed(after);
  FO_REQUIRE(refused.ok());
  FO_CHECK_EQ(static_cast<int>(refused.value().code), static_cast<int>(ErrorCode::NotFound));
}

FOTEST("engine", "query_filters_and_paging") {
  EngineFixture fixture;
  fixture.open(ctx);
  fixture.register_source(ctx, "src-a", 10);
  for (int i = 0; i < 5; ++i) {
    ObservationSpec spec;
    spec.flow = "flow-" + std::to_string(i);
    spec.bytes = static_cast<std::uint64_t>(100 * (i + 1));
    FO_REQUIRE(fixture.feed(spec).ok());
  }
  FlowQuery query;
  query.limit = 2;
  const Result<QueryResult> page = fixture.engine->query(query);
  FO_REQUIRE(page.ok());
  FO_CHECK_EQ(page.value().matched, 5u);
  FO_CHECK_EQ(page.value().returned, 2u);
  FO_CHECK(page.value().truncated);

  FlowQuery single;
  single.flow = flow_id_from_key("flow-3");
  const Result<QueryResult> one = fixture.engine->query(single);
  FO_REQUIRE(one.ok());
  FO_CHECK_EQ(one.value().returned, 1u);

  FlowQuery missing;
  missing.flow = flow_id_from_key("absent");
  const Result<QueryResult> none = fixture.engine->query(missing);
  FO_REQUIRE(none.ok());
  FO_CHECK_EQ(none.value().returned, 0u);

  FlowQuery invalid;
  invalid.limit = 0;
  FO_CHECK(!fixture.engine->query(invalid).ok());
}

FOTEST("engine", "explanation_is_deterministic_and_explicit") {
  EngineFixture fixture;
  fixture.open(ctx);
  fixture.register_source(ctx, "src-a", 10);
  ObservationSpec spec;
  spec.bytes = 100;
  spec.direction = Direction::Forward;
  FO_REQUIRE(fixture.feed(spec).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 300;
  FO_REQUIRE(fixture.feed(spec).ok());
  const Result<Explanation> first = fixture.engine->explain(flow_id_from_key("flow-1"));
  FO_REQUIRE(first.ok());
  const Result<Explanation> second = fixture.engine->explain(flow_id_from_key("flow-1"));
  FO_REQUIRE(second.ok());
  FO_CHECK_EQ(first.value().render(), second.value().render());
  const std::string text = first.value().render();
  FO_CHECK(text.find("one-sided visibility") != std::string::npos);
  FO_CHECK(text.find("freshness") != std::string::npos);
  FO_CHECK(!fixture.engine->explain(flow_id_from_key("absent")).ok());
}

FOTEST("engine", "export_formats_are_stable") {
  EngineFixture fixture;
  fixture.open(ctx);
  fixture.register_source(ctx, "src-a", 10);
  ObservationSpec spec;
  spec.bytes = 100;
  FO_REQUIRE(fixture.feed(spec).ok());
  ExportOptions options;
  options.format = ExportOptions::Format::Json;
  options.pretty = false;
  const Result<ExportResult> first = fixture.engine->export_data(options);
  FO_REQUIRE(first.ok());
  const Result<ExportResult> second = fixture.engine->export_data(options);
  FO_REQUIRE(second.ok());
  FO_CHECK_EQ(first.value().text, second.value().text);
  FO_CHECK(first.value().text.find("flow-observatory/export/1") != std::string::npos);
  FO_CHECK(first.value().text.find("\"state\":\"observed\"") != std::string::npos);

  options.format = ExportOptions::Format::Csv;
  const Result<ExportResult> csv = fixture.engine->export_data(options);
  FO_REQUIRE(csv.ok());
  FO_CHECK(csv.value().text.find("flow,key,state") != std::string::npos);

  options.format = ExportOptions::Format::Text;
  const Result<ExportResult> text = fixture.engine->export_data(options);
  FO_REQUIRE(text.ok());
  FO_CHECK(!text.value().text.empty());

  options.format = ExportOptions::Format::Json;
  options.query.limit = 0;
  FO_CHECK(!fixture.engine->export_data(options).ok());
}

FOTEST("engine", "save_and_load_snapshot_round_trip") {
  EngineFixture fixture;
  fixture.open(ctx);
  fixture.register_source(ctx, "src-a", 10);
  ObservationSpec spec;
  spec.bytes = 100;
  FO_REQUIRE(fixture.feed(spec).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 700;
  FO_REQUIRE(fixture.feed(spec).ok());

  std::filesystem::path path = std::filesystem::temp_directory_path() / "flowobs-tests";
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  path /= "snapshot.foj";
  std::filesystem::remove(path, ec);
  FO_REQUIRE(fixture.engine->save_snapshot(path).ok());

  EngineFixture other;
  other.open(ctx);
  FO_REQUIRE(other.engine->load_snapshot(path).ok());
  const Result<FlowSnapshot> snapshot = other.snapshot_of("flow-1");
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(snapshot.value().total.bytes, 600ull);
  // A loaded snapshot is restored evidence: it is never live.
  FO_CHECK(snapshot.value().restored_from_journal);
  FO_CHECK(!snapshot.value().is_live());
  FO_CHECK(!other.engine->load_snapshot(path.string() + ".missing").ok());
}
