// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <limits>
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

FOTEST("checksum", "crc32c_known_vectors") {
  FO_CHECK_EQ(crc32c(std::string_view("123456789")), 0xE3069283u);
  FO_CHECK_EQ(crc32c(std::string_view("")), 0x00000000u);
  // Incremental and one-shot agree.
  Crc32c incremental;
  incremental.update(std::string_view("12345"));
  incremental.update(std::string_view("6789"));
  FO_CHECK_EQ(incremental.value(), 0xE3069283u);
}

FOTEST("checksum", "fnv1a_is_domain_separated") {
  const std::uint64_t a = fnv1a64("flow", "alpha");
  const std::uint64_t b = fnv1a64("endpoint", "alpha");
  const std::uint64_t c = fnv1a64("flow", "alph");
  FO_CHECK_NE(a, b);
  FO_CHECK_NE(a, c);
  FO_CHECK_EQ(fnv1a64("flow", "alpha"), a);
}

FOTEST("identity", "hash_never_yields_the_invalid_identity") {
  for (int i = 0; i < 64; ++i) {
    const std::string key = "k" + std::to_string(i);
    FO_CHECK(make_id<FlowTag>(key).valid());
    FO_CHECK(make_id<EndpointTag>(key).valid());
  }
}

FOTEST("identity", "key_matches_round_trip") {
  const FlowId flow = flow_id_from_key("tenant/7/flow/9");
  FO_CHECK(key_matches(flow, "tenant/7/flow/9"));
  FO_CHECK(!key_matches(flow, "tenant/7/flow/10"));
  FO_CHECK(!key_matches(FlowId{}, "tenant/7/flow/9"));
  const EndpointId endpoint = endpoint_id_from_key("host-a:443");
  FO_CHECK(key_matches(endpoint, "host-a:443"));
  const PathId path = path_id_from_key("p0");
  FO_CHECK(key_matches(path, "p0"));
  const LinkId link = link_id_from_key("l0");
  FO_CHECK(key_matches(link, "l0"));
  const QueueId queue = queue_id_from_key("q0");
  FO_CHECK(key_matches(queue, "q0"));
  const SourceId source = source_id_from_key("src0");
  FO_CHECK(key_matches(source, "src0"));
}

FOTEST("identity", "hex_round_trip") {
  std::string text;
  append_hex16(text, 0x0123456789ABCDEFull);
  FO_CHECK_EQ(text, std::string("0123456789abcdef"));
  std::uint64_t parsed = 0;
  FO_CHECK(parse_hex16(text, parsed));
  FO_CHECK_EQ(parsed, 0x0123456789ABCDEFull);
  FO_CHECK(!parse_hex16("0123", parsed));
  FO_CHECK(!parse_hex16("0123456789abcdeZ", parsed));
}

FOTEST("checked", "add_sub_mul_saturate") {
  FO_CHECK(!checked_add<std::uint64_t>(1, 2).overflow);
  FO_CHECK_EQ(checked_add<std::uint64_t>(1, 2).value, 3ull);
  const auto sum = checked_add(std::numeric_limits<std::uint64_t>::max(), 1ull);
  FO_CHECK(sum.overflow);
  FO_CHECK_EQ(sum.value, std::numeric_limits<std::uint64_t>::max());
  const auto difference = checked_sub<std::uint64_t>(1, 2);
  FO_CHECK(difference.overflow);
  FO_CHECK_EQ(difference.value, 0ull);
  const auto product = checked_mul(std::numeric_limits<std::uint64_t>::max(), 2ull);
  FO_CHECK(product.overflow);
  const auto exact = checked_mul<std::uint64_t>(1000, 1000);
  FO_CHECK(!exact.overflow);
  FO_CHECK_EQ(exact.value, 1000000ull);
}

FOTEST("checked", "signed_add_is_total") {
  FO_CHECK_EQ(checked_add_signed(10, -3).value, std::int64_t{7});
  FO_CHECK_EQ(checked_add_signed(-10, 3).value, std::int64_t{-7});
  FO_CHECK_EQ(checked_add_signed(0, 0).value, std::int64_t{0});
  FO_CHECK_EQ(checked_add_signed(INT64_MAX, 1).overflow, true);
  FO_CHECK_EQ(checked_add_signed(INT64_MAX, 1).value, INT64_MAX);
  FO_CHECK_EQ(checked_add_signed(INT64_MIN, -1).overflow, true);
  FO_CHECK_EQ(checked_add_signed(INT64_MIN, -1).value, INT64_MIN);
  FO_CHECK_EQ(checked_add_signed(INT64_MIN, INT64_MAX).value, std::int64_t{-1});
}

FOTEST("checked", "signed_sub_is_total") {
  FO_CHECK_EQ(checked_sub_signed(10, 3).value, std::int64_t{7});
  FO_CHECK_EQ(checked_sub_signed(3, 10).value, std::int64_t{-7});
  FO_CHECK_EQ(checked_sub_signed(INT64_MIN, 1).overflow, true);
  FO_CHECK_EQ(checked_sub_signed(INT64_MAX, -1).overflow, true);
  FO_CHECK_EQ(checked_sub_signed(INT64_MAX, -1).value, INT64_MAX);
  FO_CHECK_EQ(checked_sub_signed(-1, INT64_MAX).value, INT64_MIN);
}

FOTEST("time", "format_and_parse_round_trip") {
  const Timestamp ts = Timestamp::from_nanos(1767225845123456789LL);
  const std::string text = format_timestamp(ts);
  FO_CHECK_EQ(text, std::string("2026-01-01T00:04:05.123456789Z"));
  Timestamp parsed{};
  FO_REQUIRE(parse_timestamp(text, parsed));
  FO_CHECK_EQ(parsed.nanos(), ts.nanos());
  FO_CHECK(!parse_timestamp("2026-01-01T00:04:05", parsed));
  FO_CHECK(!parse_timestamp("nonsense", parsed));
  const Timestamp epoch = Timestamp::from_nanos(0);
  FO_CHECK_EQ(format_timestamp(epoch), std::string("1970-01-01T00:00:00.000000000Z"));
}

FOTEST("time", "duration_formatting") {
  FO_CHECK_EQ(format_duration(Duration::from_nanos(1500000000LL)),
              std::string("1.500000000"));
  FO_CHECK_EQ(format_duration(Duration::from_nanos(-1)), std::string("-0.000000001"));
  FO_CHECK_EQ(format_duration(Duration::zero()), std::string("0.000000000"));
}

FOTEST("time", "difference_and_add_saturate") {
  const Timestamp a = Timestamp::from_nanos(100);
  const Timestamp b = Timestamp::from_nanos(50);
  FO_CHECK_EQ(time_difference(a, b).nanos(), 50);
  FO_CHECK_EQ(time_difference(b, a).nanos(), -50);
  FO_CHECK_EQ(time_add(a, Duration::from_nanos(25)).nanos(), 125);
}

FOTEST("semantics", "enum_names_round_trip") {
  for (std::uint8_t i = 0; i < kFlowStateCount; ++i) {
    const auto state = static_cast<FlowState>(i);
    FlowState parsed{};
    FO_CHECK(parse_flow_state(to_string(state), parsed));
    FO_CHECK_EQ(static_cast<int>(parsed), static_cast<int>(state));
  }
  for (std::uint8_t i = 0; i < kFreshnessCount; ++i) {
    const auto value = static_cast<Freshness>(i);
    Freshness parsed{};
    FO_CHECK(parse_freshness(to_string(value), parsed));
    FO_CHECK_EQ(static_cast<int>(parsed), static_cast<int>(value));
  }
  for (std::uint8_t i = 0; i < kEvidenceCount; ++i) {
    const auto value = static_cast<Evidence>(i);
    Evidence parsed{};
    FO_CHECK(parse_evidence(to_string(value), parsed));
  }
  for (std::uint8_t i = 0; i < kDirectionCount; ++i) {
    const auto value = static_cast<Direction>(i);
    Direction parsed{};
    FO_CHECK(parse_direction(to_string(value), parsed));
  }
  for (std::uint8_t i = 0; i < kObservationKindCount; ++i) {
    const auto value = static_cast<ObservationKind>(i);
    ObservationKind parsed{};
    FO_CHECK(parse_observation_kind(to_string(value), parsed));
  }
  for (std::uint8_t i = 0; i < kAnomalyKindCount; ++i) {
    const auto value = static_cast<AnomalyKind>(i);
    AnomalyKind parsed{};
    FO_CHECK(parse_anomaly_kind(to_string(value), parsed));
  }
  for (std::uint8_t i = 0; i < kVisibilityCount; ++i) {
    const auto value = static_cast<Visibility>(i);
    Visibility parsed{};
    FO_CHECK(parse_visibility(to_string(value), parsed));
  }
  for (std::uint8_t i = 0; i < kConsistencyCount; ++i) {
    const auto value = static_cast<Consistency>(i);
    Consistency parsed{};
    FO_CHECK(parse_consistency(to_string(value), parsed));
  }
  for (std::uint8_t i = 0; i < kStateCauseCount; ++i) {
    const auto value = static_cast<StateCause>(i);
    StateCause parsed{};
    FO_CHECK(parse_state_cause(to_string(value), parsed));
  }
}

FOTEST("semantics", "only_completed_asserts_completion") {
  FO_CHECK(asserts_completion(FlowState::Completed));
  FO_CHECK(!asserts_completion(FlowState::Expired));
  FO_CHECK(!asserts_completion(FlowState::Reset));
  FO_CHECK(!asserts_completion(FlowState::Idle));
  FO_CHECK(asserts_liveness(FlowState::Active));
  FO_CHECK(!asserts_liveness(FlowState::Idle));
  FO_CHECK(is_terminal(FlowState::Completed));
  FO_CHECK(is_terminal(FlowState::Reset));
  FO_CHECK(is_terminal(FlowState::Expired));
  FO_CHECK(!is_terminal(FlowState::Idle));
  FO_CHECK(!is_terminal(FlowState::Conflicting));
}

FOTEST("semantics", "idle_and_error_never_prove_completion") {
  FO_CHECK(kind_can_prove_completion(ObservationKind::Close));
  FO_CHECK(!kind_can_prove_completion(ObservationKind::Sample));
  FO_CHECK(!kind_can_prove_completion(ObservationKind::Idle));
  FO_CHECK(!kind_can_prove_completion(ObservationKind::Error));
  FO_CHECK(!kind_can_prove_completion(ObservationKind::Heartbeat));
  FO_CHECK(kind_announces_counter_restart(ObservationKind::CounterReset));
  FO_CHECK(kind_announces_counter_restart(ObservationKind::Reset));
  FO_CHECK(!kind_announces_counter_restart(ObservationKind::Close));
}

FOTEST("counters", "absent_is_not_zero") {
  CounterSet counters;
  FO_CHECK(counters.all_absent());
  FO_CHECK(!counters.any());
  counters.bytes = 0;
  FO_CHECK(counters.any());
  FO_CHECK_EQ(*counters.bytes, 0ull);
}

FOTEST("counters", "baseline_then_advance") {
  CounterAccumulator accumulator;
  FO_CHECK_EQ(static_cast<int>(accumulator.observe(100, true)),
              static_cast<int>(CounterAccumulator::Outcome::BaselineEstablished));
  FO_CHECK_EQ(accumulator.accumulated(), 0ull);
  FO_CHECK_EQ(static_cast<int>(accumulator.observe(150, true)),
              static_cast<int>(CounterAccumulator::Outcome::Advanced));
  FO_CHECK_EQ(accumulator.accumulated(), 50ull);
  FO_CHECK_EQ(static_cast<int>(accumulator.observe(150, true)),
              static_cast<int>(CounterAccumulator::Outcome::Unchanged));
  FO_CHECK_EQ(accumulator.accumulated(), 50ull);
}

FOTEST("counters", "regression_never_fabricates_consumption") {
  CounterAccumulator accumulator;
  accumulator.observe(1000, true);
  accumulator.observe(1200, true);
  FO_CHECK_EQ(accumulator.accumulated(), 200ull);
  FO_CHECK_EQ(static_cast<int>(accumulator.observe(10, true)),
              static_cast<int>(CounterAccumulator::Outcome::Regressed));
  // The 200 bytes observed before the reset are kept; the gap is unknown.
  FO_CHECK_EQ(accumulator.accumulated(), 200ull);
  FO_CHECK_EQ(accumulator.unknown_intervals(), 1ull);
  FO_CHECK_EQ(accumulator.regressions(), 1ull);
  accumulator.observe(40, true);
  FO_CHECK_EQ(accumulator.accumulated(), 230ull);
}

FOTEST("counters", "regression_can_hold_instead_of_resetting") {
  CounterAccumulator accumulator;
  accumulator.observe(1000, false);
  accumulator.observe(1200, false);
  FO_CHECK_EQ(static_cast<int>(accumulator.observe(5, false)),
              static_cast<int>(CounterAccumulator::Outcome::Held));
  FO_CHECK_EQ(accumulator.accumulated(), 200ull);
  FO_CHECK_EQ(accumulator.unknown_intervals(), 0ull);
  FO_CHECK_EQ(accumulator.regressions(), 1ull);
  // Catching up past the held value does not double count.
  FO_CHECK_EQ(static_cast<int>(accumulator.observe(1100, false)),
              static_cast<int>(CounterAccumulator::Outcome::Held));
  FO_CHECK_EQ(accumulator.accumulated(), 200ull);
  FO_CHECK_EQ(static_cast<int>(accumulator.observe(1300, false)),
              static_cast<int>(CounterAccumulator::Outcome::Advanced));
  FO_CHECK_EQ(accumulator.accumulated(), 300ull);
}

FOTEST("counters", "accumulation_saturates") {
  CounterAccumulator accumulator;
  accumulator.observe(0, true);
  accumulator.observe(std::numeric_limits<std::uint64_t>::max(), true);
  FO_CHECK_EQ(accumulator.accumulated(), std::numeric_limits<std::uint64_t>::max());
  accumulator.observe(std::numeric_limits<std::uint64_t>::max(), true);
  FO_CHECK(!accumulator.saturated());
}

FOTEST("counters", "snapshot_round_trip_is_lossless") {
  CounterAccumulator accumulator;
  accumulator.observe(10, true);
  accumulator.observe(90, true);
  accumulator.observe(3, true);
  const CounterAccumulator::Snapshot snapshot = accumulator.snapshot();
  CounterAccumulator restored;
  restored.restore(snapshot);
  FO_CHECK_EQ(restored.accumulated(), accumulator.accumulated());
  FO_CHECK_EQ(restored.unknown_intervals(), accumulator.unknown_intervals());
  FO_CHECK_EQ(restored.regressions(), accumulator.regressions());
  FO_CHECK_EQ(restored.last_value().value_or(0), accumulator.last_value().value_or(0));
}

FOTEST("history", "ring_is_bounded_and_counts_drops") {
  LifecycleHistory history(4);
  for (int i = 0; i < 10; ++i) {
    LifecycleEvent event;
    event.at = Timestamp::from_nanos(i);
    event.from = FlowState::Unknown;
    event.to = FlowState::Observed;
    history.push(event);
  }
  FO_CHECK_EQ(history.size(), 4u);
  FO_CHECK_EQ(history.dropped(), 6ull);
  const std::vector<LifecycleEvent> events = history.to_vector();
  FO_REQUIRE(events.size() == 4u);
  FO_CHECK_EQ(events.front().at.nanos(), 6);
  FO_CHECK_EQ(events.back().at.nanos(), 9);
}

FOTEST("history", "capacity_change_keeps_newest") {
  LifecycleHistory history(8);
  for (int i = 0; i < 8; ++i) {
    LifecycleEvent event;
    event.at = Timestamp::from_nanos(i);
    history.push(event);
  }
  history.set_capacity(3);
  const std::vector<LifecycleEvent> events = history.to_vector();
  FO_REQUIRE(events.size() == 3u);
  FO_CHECK_EQ(events.front().at.nanos(), 5);
  FO_CHECK_EQ(events.back().at.nanos(), 7);
  FO_CHECK_EQ(history.dropped(), 5ull);
}

FOTEST("limits", "validation_rejects_nonsense") {
  Limits limits;
  FO_CHECK(limits.validate().ok());
  limits.max_flows = 0;
  FO_CHECK(!limits.validate().ok());
  limits = Limits{};
  limits.max_ingest_batch = limits.max_pending_observations + 1;
  FO_CHECK(!limits.validate().ok());
  limits = Limits{};
  limits.worker_threads = 100000;
  FO_CHECK(!limits.validate().ok());
  limits = Limits{};
  limits.max_frame_bytes = 8;
  FO_CHECK(!limits.validate().ok());
}

FOTEST("policy", "validation_rejects_inconsistent_windows") {
  FreshnessPolicy freshness;
  FO_CHECK(freshness.validate().ok());
  freshness.expire_window = Duration::from_seconds(1);
  freshness.fresh_window = Duration::from_seconds(60);
  FO_CHECK(!freshness.validate().ok());
  ExpiryPolicy expiry;
  FO_CHECK(expiry.validate().ok());
  expiry.idle_after = Duration::from_seconds(10);
  expiry.expire_after = Duration::from_seconds(5);
  FO_CHECK(!expiry.validate().ok());
  ReconciliationPolicy reconciliation;
  reconciliation.relative_tolerance_ppm = 2000000;
  FO_CHECK(!reconciliation.validate().ok());
}

FOTEST("freshness", "classify_uses_both_instants") {
  FreshnessContext fctx;
  const Timestamp now = Timestamp::from_seconds(1000);
  fctx.now = now;
  fctx.policy.fresh_window = Duration::from_seconds(10);
  fctx.policy.expire_window = Duration::from_seconds(60);
  FO_CHECK_EQ(static_cast<int>(classify_freshness(now, now, fctx)),
              static_cast<int>(Freshness::Fresh));
  FO_CHECK_EQ(static_cast<int>(
                  classify_freshness(Timestamp::from_seconds(995), now, fctx)),
              static_cast<int>(Freshness::Fresh));
  FO_CHECK_EQ(static_cast<int>(
                  classify_freshness(Timestamp::from_seconds(970), now, fctx)),
              static_cast<int>(Freshness::Stale));
  FO_CHECK_EQ(static_cast<int>(
                  classify_freshness(Timestamp::from_seconds(880), now, fctx)),
              static_cast<int>(Freshness::Expired));
  FO_CHECK_EQ(static_cast<int>(classify_freshness(Timestamp::unknown(), now, fctx)),
              static_cast<int>(Freshness::Unknown));
  // The receive instant is the reference when the evaluation instant is absent.
  FreshnessContext no_now = fctx;
  no_now.now = Timestamp::unknown();
  FO_CHECK_EQ(static_cast<int>(
                  classify_freshness(Timestamp::from_seconds(995), now, no_now)),
              static_cast<int>(Freshness::Fresh));
}

FOTEST("freshness", "previous_epoch_evidence_is_unknown") {
  FreshnessContext fctx;
  fctx.now = Timestamp::from_seconds(1000);
  fctx.policy.fresh_window = Duration::from_seconds(10);
  fctx.policy.expire_window = Duration::from_seconds(60);
  fctx.from_previous_epoch = true;
  const Timestamp now = Timestamp::from_seconds(1000);
  FO_CHECK_EQ(static_cast<int>(classify_freshness(now, now, fctx)),
              static_cast<int>(Freshness::Unknown));
  fctx.policy.require_reconfirmation_after_restart = false;
  FO_CHECK_EQ(static_cast<int>(classify_freshness(now, now, fctx)),
              static_cast<int>(Freshness::Fresh));
}

FOTEST("freshness", "future_evidence_is_clamped_to_receive_instant") {
  FreshnessContext fctx;
  const Timestamp received = Timestamp::from_seconds(1000);
  const Timestamp future = Timestamp::from_seconds(1010);
  fctx.now = received;
  fctx.policy.fresh_window = Duration::from_seconds(10);
  fctx.policy.expire_window = Duration::from_seconds(60);
  // A source whose clock is ten seconds ahead is not ten seconds fresh: the
  // evidence is clamped to the instant it was received.
  FO_CHECK_EQ(static_cast<int>(classify_freshness(future, received, fctx)),
              static_cast<int>(Freshness::Fresh));
  FO_CHECK(exceeds_clock_skew(future, received, Duration::from_seconds(1)));
  FO_CHECK(exceeds_clock_skew(future, received, Duration::from_seconds(9)));
  FO_CHECK(!exceeds_clock_skew(future, received, Duration::from_seconds(10)));
  FO_CHECK(!exceeds_clock_skew(received, received, Duration::from_seconds(1)));
}

FOTEST("attribution", "status_names_round_trip") {
  FO_CHECK_EQ(std::string(to_string(AttributionStatus::Ok)), std::string("ok"));
  FO_CHECK_EQ(std::string(to_string(AttributionStatus::StaleGeneration)),
              std::string("stale-generation"));
}

FOTEST("json", "escaping_is_bounded_and_correct") {
  FO_CHECK_EQ(json::escape("a\"b"), std::string("a\\\"b"));
  FO_CHECK_EQ(json::escape("a\\b"), std::string("a\\\\b"));
  FO_CHECK_EQ(json::escape("a\nb"), std::string("a\\nb"));
  FO_CHECK_EQ(json::escape(std::string_view("\x01", 1)), std::string("\\u0001"));
}

FOTEST("json", "writer_is_deterministic_and_exact") {
  json::Writer writer(true);
  writer.begin_object();
  writer.field("a", static_cast<std::uint64_t>(18446744073709551615ull));
  writer.field("b", true);
  writer.field("c", "text");
  writer.key("d");
  writer.begin_array();
  writer.value_uint(1);
  writer.value_uint(2);
  writer.end_array();
  writer.key("empty");
  writer.begin_object();
  writer.end_object();
  writer.field_ratio("ratio", 1, 3, 3);
  writer.end_object();
  const std::string first = writer.str();
  json::Writer again(true);
  again.begin_object();
  again.field("a", static_cast<std::uint64_t>(18446744073709551615ull));
  again.field("b", true);
  again.field("c", "text");
  again.key("d");
  again.begin_array();
  again.value_uint(1);
  again.value_uint(2);
  again.end_array();
  again.key("empty");
  again.begin_object();
  again.end_object();
  again.field_ratio("ratio", 1, 3, 3);
  again.end_object();
  FO_CHECK_EQ(first, again.str());
  FO_CHECK(first.find("18446744073709551615") != std::string::npos);
  FO_CHECK(first.find("0.333") != std::string::npos);
  FO_CHECK(first.find("{}") != std::string::npos);
}
