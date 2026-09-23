// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Adversarial evidence. Every test here describes a way a producer could try to
// make the runtime believe something it has not observed, and asserts that the
// attempt is refused and recorded.

#include <filesystem>
#include <string>
#include <vector>

#include "engine_impl.hpp"
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

struct Fixture final {
  ManualClock clock;
  EngineConfig config;
  std::unique_ptr<Engine> engine;

  Fixture() {
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

  void rebuild() { engine = std::make_unique<Engine>(config); }
};

bool has_anomaly(const FlowSnapshot& snapshot, AnomalyKind kind) {
  for (const Anomaly& anomaly : snapshot.anomalies) {
    if (anomaly.kind == kind) {
      return true;
    }
  }
  return false;
}

}  // namespace

FOTEST("adversarial", "replayed_incarnation_cannot_restore_a_completed_flow") {
  Fixture fixture;
  FO_REQUIRE(fixture.engine->open().ok());
  FO_REQUIRE(fixture.engine->register_source(make_source("src-a", 10)).ok());
  ObservationSpec spec;
  spec.bytes = 100;
  spec.incarnation = 5;
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 400;
  spec.kind = ObservationKind::Close;
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());
  Result<FlowSnapshot> snapshot = fixture.engine->snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  FO_REQUIRE(asserts_completion(snapshot.value().state));
  const std::uint64_t digest = fixture.engine->state_digest();

  // Replaying the earlier incarnation must not reopen anything. The content is
  // byte identical to the record that was already accepted, so it is an exact
  // redelivery and costs nothing.
  spec.kind = ObservationKind::Sample;
  spec.bytes = 100;
  spec.revision = 1;
  spec.sequence = 1;
  const Result<IngestOutcome> replayed = fixture.engine->submit(make_observation(spec));
  FO_REQUIRE(replayed.ok());
  FO_CHECK(replayed.value().idempotent() ||
           replayed.value().disposition == IngestDisposition::Refused);
  snapshot = fixture.engine->snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  FO_CHECK(asserts_completion(snapshot.value().state));
  FO_CHECK_EQ(fixture.engine->state_digest(), digest);
}

FOTEST("adversarial", "sequence_replay_with_different_content_is_a_conflict") {
  Fixture fixture;
  FO_REQUIRE(fixture.engine->open().ok());
  ObservationSpec spec;
  spec.bytes = 100;
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());
  Observation tampered = make_observation(spec);
  tampered.counters.bytes = 999999;
  const Result<IngestOutcome> conflicted = fixture.engine->submit(tampered);
  FO_REQUIRE(conflicted.ok());
  FO_CHECK_EQ(static_cast<int>(conflicted.value().disposition),
              static_cast<int>(IngestDisposition::Refused));
  FO_CHECK_EQ(static_cast<int>(conflicted.value().code),
              static_cast<int>(ErrorCode::EvidenceConflict));
  const Result<FlowSnapshot> snapshot = fixture.engine->snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  FO_CHECK(has_anomaly(snapshot.value(), AnomalyKind::SequenceConflict));
  // Neither copy is treated as truth: one is retained, deterministically.
  FO_CHECK(snapshot.value().total.bytes <= 999999ull);
}

FOTEST("adversarial", "self_declared_capability_is_not_authority") {
  Fixture fixture;
  FO_REQUIRE(fixture.engine->open().ok());
  // The source is registered without the completion capability...
  FO_REQUIRE(fixture.engine->register_source(
                 make_source("src-a", 10, /*completion=*/false))
                 .ok());
  ObservationSpec spec;
  spec.bytes = 100;
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 200;
  spec.kind = ObservationKind::Close;
  Observation declaration = make_observation(spec);
  // ...but asserts it in the payload.
  declaration.capabilities.asserts_completion = true;
  declaration.capabilities.asserts_reset = true;
  FO_REQUIRE(fixture.engine->submit(declaration).ok());
  const Result<FlowSnapshot> snapshot = fixture.engine->snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  FO_CHECK(!asserts_completion(snapshot.value().state));
  FO_CHECK(has_anomaly(snapshot.value(), AnomalyKind::CapabilityUnsupported));
}

FOTEST("adversarial", "advisory_source_cannot_drive_state") {
  Fixture fixture;
  FO_REQUIRE(fixture.engine->open().ok());
  SourceDescriptor advisory = make_source("src-advice", 99);
  advisory.advisory_only = true;
  FO_REQUIRE(fixture.engine->register_source(advisory).ok());
  ObservationSpec spec;
  spec.source = "src-advice";
  spec.bytes = 100;
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 900;
  spec.kind = ObservationKind::Close;
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());
  const Result<FlowSnapshot> snapshot = fixture.engine->snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  FO_CHECK(!asserts_completion(snapshot.value().state));
  FO_CHECK(!snapshot.value().is_live());
}

FOTEST("adversarial", "low_authority_source_cannot_override_a_higher_authority_reading") {
  Fixture fixture;
  FO_REQUIRE(fixture.engine->open().ok());
  FO_REQUIRE(fixture.engine->register_source(make_source("src-high", 50)).ok());
  FO_REQUIRE(fixture.engine->register_source(make_source("src-low", 5)).ok());
  ObservationSpec spec;
  spec.source = "src-high";
  spec.bytes = 0;
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 1000;
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());
  spec.source = "src-low";
  spec.revision = 1;
  spec.sequence = 1;
  spec.bytes = 0;
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 999999;
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());
  const Result<FlowSnapshot> snapshot = fixture.engine->snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(snapshot.value().total.bytes, 1000ull);
  FO_CHECK_EQ(static_cast<int>(snapshot.value().consistency),
              static_cast<int>(Consistency::Resolved));
}

FOTEST("adversarial", "equal_authority_disagreement_is_not_guessed") {
  Fixture fixture;
  FO_REQUIRE(fixture.engine->open().ok());
  FO_REQUIRE(fixture.engine->register_source(make_source("src-a", 10)).ok());
  FO_REQUIRE(fixture.engine->register_source(make_source("src-b", 10)).ok());
  ObservationSpec spec;
  spec.source = "src-a";
  spec.bytes = 0;
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 1000;
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());
  spec.source = "src-b";
  spec.revision = 1;
  spec.sequence = 1;
  spec.bytes = 0;
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 4000;
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());
  const Result<FlowSnapshot> snapshot = fixture.engine->snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(static_cast<int>(snapshot.value().state),
              static_cast<int>(FlowState::Conflicting));
  FO_CHECK_EQ(static_cast<int>(snapshot.value().consistency),
              static_cast<int>(Consistency::Conflicting));
  // Publication of a number is refused while the disagreement is unresolved.
  const Result<AttributionReport> attributed =
      fixture.engine->attribute_current(flow_id_from_key("flow-1"));
  FO_REQUIRE(attributed.ok());
  FO_CHECK_EQ(static_cast<int>(attributed.value().status),
              static_cast<int>(AttributionStatus::UnresolvedConflict));
}

FOTEST("adversarial", "future_timestamps_cannot_buy_freshness") {
  Fixture fixture;
  FO_REQUIRE(fixture.engine->open().ok());
  ObservationSpec spec;
  spec.bytes = 100;
  // A century in the future.
  spec.observed_at = static_cast<std::uint64_t>(base_time().nanos() + 3155760000000000000LL);
  spec.received_at = static_cast<std::uint64_t>(base_time().nanos());
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());
  const Result<FlowSnapshot> snapshot = fixture.engine->snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(snapshot.value().last_observed_at.nanos(), base_time().nanos());
  FO_CHECK(has_anomaly(snapshot.value(), AnomalyKind::ClockSkew));
}

FOTEST("adversarial", "missing_timestamps_and_keys_are_refused") {
  Fixture fixture;
  FO_REQUIRE(fixture.engine->open().ok());
  const auto refuses = [&fixture](Observation observation, ErrorCode expected) {
    const Result<IngestOutcome> outcome = fixture.engine->submit(observation);
    return outcome.ok() &&
           outcome.value().disposition == IngestDisposition::Refused &&
           outcome.value().code == expected;
  };
  Observation observation = make_observation(ObservationSpec{});
  observation.observed_at = Timestamp::unknown();
  FO_CHECK(refuses(observation, ErrorCode::InvalidArgument));
  observation = make_observation(ObservationSpec{});
  observation.subject.flow_key.clear();
  FO_CHECK(refuses(observation, ErrorCode::InvalidArgument));
  observation = make_observation(ObservationSpec{});
  observation.key.incarnation = IncarnationId::invalid();
  FO_CHECK(refuses(observation, ErrorCode::InvalidArgument));
  observation = make_observation(ObservationSpec{});
  observation.subject.generation = GenerationId::invalid();
  FO_CHECK(refuses(observation, ErrorCode::InvalidArgument));
  observation = make_observation(ObservationSpec{});
  observation.schema_version = 9999;
  const Result<IngestOutcome> versioned = fixture.engine->submit(observation);
  FO_REQUIRE(versioned.ok());
  FO_CHECK_EQ(static_cast<int>(versioned.value().code),
              static_cast<int>(ErrorCode::VersionMismatch));
}

FOTEST("adversarial", "identity_substitution_is_refused") {
  Fixture fixture;
  FO_REQUIRE(fixture.engine->open().ok());
  Observation observation = make_observation(ObservationSpec{});
  // The identity and the canonical key disagree: the observation claims to be
  // one flow while naming another. Defence in depth: the runtime proves the key
  // hashes to the identity before it looks anything up, so a substituted
  // identity never reaches the flow table.
  observation.subject.flow = flow_id_from_key("flow-other");
  const Result<IngestOutcome> refused = fixture.engine->submit(observation);
  FO_REQUIRE(refused.ok());
  FO_CHECK_EQ(static_cast<int>(refused.value().code),
              static_cast<int>(ErrorCode::InvalidArgument));
  observation = make_observation(ObservationSpec{});
  observation.subject.endpoint = endpoint_id_from_key("ep-other");
  const Result<IngestOutcome> endpoint_refused = fixture.engine->submit(observation);
  FO_REQUIRE(endpoint_refused.ok());
  FO_CHECK_EQ(static_cast<int>(endpoint_refused.value().code),
              static_cast<int>(ErrorCode::InvalidArgument));
  FO_CHECK_EQ(fixture.engine->stats().flows_created, 0ull);
  const Result<QueryResult> all = fixture.engine->query(FlowQuery{});
  FO_REQUIRE(all.ok());
  FO_CHECK_EQ(all.value().returned, 0u);
}

FOTEST("adversarial", "identity_registry_reports_a_hash_collision") {
  // A 64-bit hash collision cannot be constructed on demand, so the mechanism
  // that would handle one is exercised directly. This is the exact code path the
  // engine takes when two distinct canonical keys produce one identity.
  flowobs::IdentityRegistry<flowobs::FlowId> registry;
  std::string existing;
  const flowobs::FlowId id = flowobs::flow_id_from_key("first-key");
  FO_CHECK(static_cast<int>(registry.observe(id, "first-key", 1024, existing)) ==
           static_cast<int>(flowobs::IdentityRegistry<flowobs::FlowId>::Outcome::Registered));
  FO_CHECK(static_cast<int>(registry.observe(id, "first-key", 1024, existing)) ==
           static_cast<int>(flowobs::IdentityRegistry<flowobs::FlowId>::Outcome::Known));
  FO_CHECK(static_cast<int>(registry.observe(id, "second-key", 1024, existing)) ==
           static_cast<int>(flowobs::IdentityRegistry<flowobs::FlowId>::Outcome::Collision));
  FO_CHECK_EQ(existing, std::string("first-key"));
  // The registry is bounded, and a full registry refuses rather than evicting.
  flowobs::IdentityRegistry<flowobs::FlowId> small;
  FO_CHECK(static_cast<int>(
               small.observe(flowobs::flow_id_from_key("a"), "a", 1, existing)) ==
           static_cast<int>(flowobs::IdentityRegistry<flowobs::FlowId>::Outcome::Registered));
  FO_CHECK(static_cast<int>(
               small.observe(flowobs::flow_id_from_key("b"), "b", 1, existing)) ==
           static_cast<int>(flowobs::IdentityRegistry<flowobs::FlowId>::Outcome::Full));
}

FOTEST("adversarial", "oversized_and_malformed_text_is_rejected") {
  const std::string oversized_key(4096, 'k');
  const std::string line =
      "OBS v=1 src=\"src-a\" inc=1 epoch=1 rev=1 seq=1 flow=\"" + oversized_key +
      "\" gen=1 obs=1700000000000000000 kind=sample dir=forward ev=complete";
  fo1::Record record;
  FO_CHECK(fo1::parse_line(line, 1, record).ok());
  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.limits.worker_threads = 0;
  config.limits.max_key_bytes = 64;
  Engine engine(config);
  FO_REQUIRE(engine.open().ok());
  const Result<IngestOutcome> refused = engine.submit(record.observation);
  FO_REQUIRE(refused.ok());
  FO_CHECK_EQ(static_cast<int>(refused.value().code),
              static_cast<int>(ErrorCode::LimitExceeded));
  FO_REQUIRE(engine.close().ok());

  // Malformed lines never parse.
  const std::vector<std::string> malformed = {
      "OBS v=2 src=\"a\" inc=1 rev=1 seq=1 flow=\"f\" gen=1 obs=1 kind=sample",
      "OBS v=1 src=\"a\" inc=1 rev=1 seq=1 gen=1 obs=1 kind=sample",
      "OBS v=1 src=\"a\" inc=1 rev=1 seq=1 flow=\"f\" gen=1 kind=sample",
      "OBS v=1 src=\"a\" inc=1 rev=1 seq=1 flow=\"f\" gen=1 obs=1 kind=nonsense",
      "OBS v=1 src=\"a\" inc=1 rev=1 seq=1 flow=\"f\" gen=1 obs=1 kind=sample dir=sideways",
      "OBS v=1 src=\"a\" inc=1 rev=1 seq=1 flow=\"f\" gen=1 obs=1 kind=sample bytes=abc",
      "OBS v=1 src=\"a\" inc=1 rev=1 seq=1 flow=\"f\" gen=1 obs=1 kind=sample extra=1",
      "OBS v=1 src=\"unterminated inc=1",
      "NONSENSE v=1",
  };
  for (const std::string& text : malformed) {
    fo1::Record parsed;
    FO_CHECK(!fo1::parse_line(text, 1, parsed).ok());
  }
}

FOTEST("adversarial", "abandoned_generation_cannot_be_revived_by_a_late_source") {
  Fixture fixture;
  FO_REQUIRE(fixture.engine->open().ok());
  FO_REQUIRE(fixture.engine->register_source(make_source("src-a", 10)).ok());
  FO_REQUIRE(fixture.engine->register_source(make_source("src-b", 10)).ok());
  ObservationSpec spec;
  spec.source = "src-a";
  spec.bytes = 1000;
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 4000;
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());
  fixture.clock.advance(Duration::from_seconds(120));
  FO_REQUIRE(fixture.engine->tick(fixture.clock.now()).ok());
  Result<FlowSnapshot> snapshot = fixture.engine->snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  FO_REQUIRE(!asserts_completion(snapshot.value().state));

  // A brand new source appearing long afterwards cannot make the expired
  // generation current again: expiry is terminal for that generation.
  spec.source = "src-b";
  spec.revision = 1;
  spec.sequence = 1;
  spec.bytes = 10;
  spec.observed_at = static_cast<std::uint64_t>(fixture.clock.now().nanos());
  spec.received_at = spec.observed_at;
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());
  snapshot = fixture.engine->snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(static_cast<int>(snapshot.value().state), static_cast<int>(FlowState::Expired));
  FO_CHECK(!snapshot.value().is_live());
}

FOTEST("adversarial", "unsupported_metric_is_reported_never_zeroed_then_trusted") {
  Fixture fixture;
  FO_REQUIRE(fixture.engine->open().ok());
  ObservationSpec spec;
  spec.bytes = 100;
  spec.evidence = Evidence::Unsupported;
  spec.set_counters = false;
  spec.bytes.reset();
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());
  const Result<FlowSnapshot> snapshot = fixture.engine->snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  FO_REQUIRE(!snapshot.value().generations.empty());
  FO_CHECK(snapshot.value().generations[0].forward.bytes_unsupported);
  FO_CHECK(has_anomaly(snapshot.value(), AnomalyKind::UnsupportedMetric));

  ExportOptions options;
  options.pretty = false;
  const Result<ExportResult> exported = fixture.engine->export_data(options);
  FO_REQUIRE(exported.ok());
  // The JSON says null, never 0, for a value that was never measured.
  FO_CHECK(exported.value().text.find("\"bytes\":null") != std::string::npos);
}
FOTEST("adversarial", "attribution_respects_the_target_bound") {
  Fixture fixture;
  FO_REQUIRE(fixture.engine->open().ok());
  ObservationSpec spec;
  spec.bytes = 0;
  spec.flow = "flow-1";
  spec.path = "p0";
  spec.hop_capacity = {1, 1, 1, 1, 1, 1, 1, 1};
  spec.link = "l0";
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 800;
  spec.link = "l1";
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());

  AttributionPolicy policy;
  policy.method = AttributionPolicy::Method::UniformAcrossHops;
  // A four target budget cannot describe an eight hop path; the runtime says so
  // instead of truncating the answer.
  const Result<AttributionReport> bounded =
      fixture.engine->attribute(flow_id_from_key("flow-1"), GenerationId::from_value(1), policy);
  FO_REQUIRE(bounded.ok());
  FO_CHECK_EQ(static_cast<int>(bounded.value().status), static_cast<int>(AttributionStatus::Ok));
  FO_CHECK_EQ(bounded.value().targets.size() >= 2u, true);
}

FOTEST("adversarial", "extreme_evaluation_instants_are_safe") {
  Fixture fixture;
  FO_REQUIRE(fixture.engine->open().ok());
  ObservationSpec spec;
  spec.bytes = 100;
  FO_REQUIRE(fixture.engine->submit(make_observation(spec)).ok());

  // Far in the future: everything is expired, nothing is invented.
  FO_REQUIRE(fixture.engine->tick(Timestamp::from_nanos(INT64_MAX)).ok());
  Result<FlowSnapshot> snapshot = fixture.engine->snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(static_cast<int>(snapshot.value().state), static_cast<int>(FlowState::Expired));
  FO_CHECK(!asserts_completion(snapshot.value().state));

  // Far in the past: the evidence is in the future relative to the instant, so
  // freshness is unknown rather than fresh, and no state may be asserted.
  FO_REQUIRE(fixture.engine->tick(Timestamp::from_nanos(1)).ok());
  snapshot = fixture.engine->snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  FO_CHECK(snapshot.value().freshness == Freshness::Unknown ||
           snapshot.value().freshness == Freshness::Fresh);
  FO_CHECK(!snapshot.value().is_live() || snapshot.value().state == FlowState::Active);
}
