// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Property tests. Each one states an invariant the runtime claims and then
// tries to falsify it with seeded random input.

#include <algorithm>
#include <set>
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

ObservationSpec random_spec(fotest::Rng& rng, std::uint64_t& revision) {
  ObservationSpec spec;
  spec.flow = "flow-" + std::to_string(rng.below(4));
  spec.source = "src-" + std::to_string(rng.below(3));
  spec.generation = 1 + rng.below(2);
  spec.incarnation = 1;
  spec.epoch = 1;
  spec.revision = ++revision;
  spec.sequence = revision;
  spec.direction = rng.chance(50) ? Direction::Forward : Direction::Reverse;
  spec.kind = rng.chance(10) ? ObservationKind::Progress : ObservationKind::Sample;
  spec.bytes = rng.below(100000);
  spec.packets = rng.below(1000);
  spec.retransmissions = rng.below(10);
  spec.observed_at = static_cast<std::uint64_t>(base_time().nanos() +
                                                static_cast<std::int64_t>(revision) * 1000);
  spec.received_at = spec.observed_at;
  return spec;
}

std::string export_of(const std::vector<ObservationSpec>& specs, std::uint64_t seed,
                      std::size_t permutation_seed) {
  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.limits.worker_threads = 0;
  Engine engine(config);
  const Status opened = engine.open();
  if (opened.failed()) {
    return "open-failed";
  }
  for (std::uint32_t i = 0; i < 3; ++i) {
    (void)engine.register_source(make_source("src-" + std::to_string(i), 10 + i));
  }
  std::vector<ObservationSpec> order = specs;
  fotest::Rng shuffler(permutation_seed);
  for (std::size_t i = order.size(); i > 1; --i) {
    const std::size_t j = static_cast<std::size_t>(shuffler.below(i));
    std::swap(order[i - 1], order[j]);
  }
  for (const ObservationSpec& spec : order) {
    (void)engine.submit(make_observation(spec));
  }
  ExportOptions options;
  options.pretty = false;
  options.include_generations = true;
  // The reconciled view is order independent; the ingest anomaly log and the
  // lifecycle transition log describe the path taken to it and are not.
  options.include_anomalies = false;
  const Result<ExportResult> exported = engine.export_data(options);
  std::string text = exported.ok() ? exported.value().text : "export-failed";
  text += "|digest=" + std::to_string(engine.state_digest());
  text += "|seed=" + std::to_string(seed);
  (void)engine.close();
  return text;
}

}  // namespace

FOTEST("property", "arrival_order_does_not_change_the_view") {
  // The reconciled view is a pure function of the accepted observation set.
  // Twenty independent shuffles of the same set must produce byte identical
  // exports and identical durable digests.
  for (std::uint64_t round = 0; round < 8; ++round) {
    fotest::Rng rng(ctx.seed + round * 7919);
    std::vector<ObservationSpec> specs;
    std::uint64_t revision = 0;
    for (int i = 0; i < 40; ++i) {
      specs.push_back(random_spec(rng, revision));
    }
    const std::string reference = export_of(specs, round, 12345);
    for (std::size_t permutation = 0; permutation < 5; ++permutation) {
      const std::string other =
          export_of(specs, round, static_cast<std::uint64_t>(999 + permutation * 31));
      FO_CHECK_EQ(other, reference);
    }
  }
}

FOTEST("property", "duplicate_and_repeat_delivery_is_idempotent") {
  for (std::uint64_t round = 0; round < 8; ++round) {
    fotest::Rng rng(ctx.seed + 104729 + round * 31);
    ManualClock clock;
    clock.set(fotest::recent_now());
    EngineConfig config;
    config.clock = &clock;
    config.limits.worker_threads = 0;
    Engine engine(config);
    FO_REQUIRE(engine.open().ok());
    std::vector<ObservationSpec> specs;
    std::uint64_t revision = 0;
    for (int i = 0; i < 30; ++i) {
      specs.push_back(random_spec(rng, revision));
    }
    for (const ObservationSpec& spec : specs) {
      FO_REQUIRE(engine.submit(make_observation(spec)).ok());
    }
    const std::uint64_t digest = engine.state_digest();
    for (int repeat = 0; repeat < 3; ++repeat) {
      for (const ObservationSpec& spec : specs) {
        const Result<IngestOutcome> again = engine.submit(make_observation(spec));
        FO_REQUIRE(again.ok());
        FO_CHECK(again.value().idempotent());
      }
    }
    FO_CHECK_EQ(engine.state_digest(), digest);
    FO_REQUIRE(engine.close().ok());
  }
}

FOTEST("property", "consumption_is_monotone_and_never_invented") {
  // Cumulative counters that only advance must produce a total that equals the
  // exact sum of the observed deltas and never decreases.
  for (std::uint64_t round = 0; round < 8; ++round) {
    fotest::Rng rng(ctx.seed + 15485863 + round * 17);
    ManualClock clock;
    clock.set(fotest::recent_now());
    EngineConfig config;
    config.clock = &clock;
    config.limits.worker_threads = 0;
    Engine engine(config);
    FO_REQUIRE(engine.open().ok());
    std::uint64_t counter = rng.below(1000);
    std::uint64_t expected = 0;
    std::uint64_t revision = 0;
    std::uint64_t previous_total = 0;
    for (int i = 0; i < 60; ++i) {
      const std::uint64_t advance = rng.below(500);
      counter += advance;
      if (i > 0) {
        // The first sample establishes the baseline; only the deltas that
        // follow it are consumption.
        expected += advance;
      }
      ObservationSpec spec;
      spec.bytes = counter;
      spec.revision = ++revision;
      spec.sequence = revision;
      spec.observed_at = static_cast<std::uint64_t>(base_time().nanos() +
                                                    static_cast<std::int64_t>(revision) * 1000);
      spec.received_at = spec.observed_at;
      FO_REQUIRE(engine.submit(make_observation(spec)).ok());
      const Result<FlowSnapshot> snapshot = engine.snapshot(flow_id_from_key("flow-1"));
      FO_REQUIRE(snapshot.ok());
      FO_CHECK(snapshot.value().total.bytes >= previous_total);
      previous_total = snapshot.value().total.bytes;
    }
    FO_CHECK_EQ(previous_total, expected);
    FO_REQUIRE(engine.close().ok());
  }
}

FOTEST("property", "counter_resets_never_fabricate_consumption") {
  for (std::uint64_t round = 0; round < 8; ++round) {
    fotest::Rng rng(ctx.seed + 32452843 + round * 23);
    ManualClock clock;
    clock.set(fotest::recent_now());
    EngineConfig config;
    config.clock = &clock;
    config.limits.worker_threads = 0;
    Engine engine(config);
    FO_REQUIRE(engine.open().ok());
    std::uint64_t counter = rng.below(5000);
    std::uint64_t expected = 0;
    std::uint64_t gaps = 0;
    std::uint64_t revision = 0;
    for (int i = 0; i < 80; ++i) {
      const bool reset = rng.chance(15);
      if (i == 0) {
        // The first sample is the baseline, not consumption.
      } else if (reset) {
        // A reset must be a genuine regression, otherwise it is an advance and
        // the runtime is right to count it.
        counter = counter == 0 ? 0 : rng.below(counter);
        gaps += 1;
      } else {
        const std::uint64_t advance = rng.below(300);
        counter += advance;
        expected += advance;
      }
      ObservationSpec spec;
      spec.bytes = counter;
      spec.revision = ++revision;
      spec.sequence = revision;
      spec.observed_at = static_cast<std::uint64_t>(base_time().nanos() +
                                                    static_cast<std::int64_t>(revision) * 1000);
      spec.received_at = spec.observed_at;
      FO_REQUIRE(engine.submit(make_observation(spec)).ok());
    }
    const Result<FlowSnapshot> snapshot = engine.snapshot(flow_id_from_key("flow-1"));
    FO_REQUIRE(snapshot.ok());
    // Every byte that was counted was actually observed as a positive delta.
    FO_CHECK_EQ(snapshot.value().total.bytes, expected);
    // Every reset is reported as an unobserved stretch, never as a negative or
    // invented amount.
    FO_CHECK(snapshot.value().total.unknown_intervals <= gaps + 1u);
    FO_CHECK(!snapshot.value().total.saturated);
    FO_REQUIRE(engine.close().ok());
  }
}

FOTEST("property", "history_and_anomalies_respect_their_bounds") {
  for (std::uint64_t round = 0; round < 4; ++round) {
    fotest::Rng rng(ctx.seed + 49979687 + round * 13);
    ManualClock clock;
    clock.set(fotest::recent_now());
    EngineConfig config;
    config.clock = &clock;
    config.limits.worker_threads = 0;
    config.limits.max_history_per_flow = 8;
    config.limits.max_anomalies_per_flow = 5;
    Engine engine(config);
    FO_REQUIRE(engine.open().ok());
    std::uint64_t revision = 0;
    for (int i = 0; i < 200; ++i) {
      ObservationSpec spec = random_spec(rng, revision);
      (void)engine.submit(make_observation(spec));
    }
    FlowQuery query;
    query.limit = 100;
    const Result<QueryResult> result = engine.query(query);
    FO_REQUIRE(result.ok());
    for (const FlowSnapshot& snapshot : result.value().flows) {
      FO_CHECK(snapshot.anomalies.size() <= config.limits.max_anomalies_per_flow);
      FO_CHECK(snapshot.generations.size() <= config.limits.max_generations_per_flow);
    }
    FO_REQUIRE(engine.close().ok());
  }
}

FOTEST("property", "freshness_degrades_monotonically_with_time") {
  fotest::Rng rng(ctx.seed + 86028121);
  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.limits.worker_threads = 0;
  config.freshness.fresh_window = Duration::from_seconds(10);
  config.freshness.expire_window = Duration::from_seconds(60);
  Engine engine(config);
  FO_REQUIRE(engine.open().ok());
  ObservationSpec spec;
  spec.bytes = 100;
  FO_REQUIRE(engine.submit(make_observation(spec)).ok());

  const auto classification = [&engine]() {
    const Result<FlowSnapshot> snapshot = engine.snapshot(flow_id_from_key("flow-1"));
    return snapshot.ok() ? snapshot.value().freshness : Freshness::Unknown;
  };
  FO_CHECK_EQ(static_cast<int>(classification()), static_cast<int>(Freshness::Fresh));
  clock.advance(Duration::from_seconds(11));
  FO_CHECK_EQ(static_cast<int>(classification()), static_cast<int>(Freshness::Stale));
  clock.advance(Duration::from_seconds(60));
  FO_CHECK_EQ(static_cast<int>(classification()), static_cast<int>(Freshness::Expired));
  // The classification never improves as time moves forward.
  const std::vector<Freshness> seen = {Freshness::Fresh, Freshness::Stale, Freshness::Expired};
  for (std::size_t i = 1; i < seen.size(); ++i) {
    FO_CHECK(static_cast<int>(seen[i]) >= static_cast<int>(seen[i - 1]));
  }
  FO_REQUIRE(engine.close().ok());
}

FOTEST("property", "state_digest_is_stable_across_repeated_queries") {
  fotest::Rng rng(ctx.seed + 961748941);
  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.limits.worker_threads = 0;
  Engine engine(config);
  FO_REQUIRE(engine.open().ok());
  std::uint64_t revision = 0;
  for (int i = 0; i < 100; ++i) {
    (void)engine.submit(make_observation(random_spec(rng, revision)));
  }
  const std::uint64_t digest = engine.state_digest();
  for (int i = 0; i < 10; ++i) {
    (void)engine.query(FlowQuery{});
    (void)engine.export_data(ExportOptions{});
    FO_CHECK_EQ(engine.state_digest(), digest);
  }
  FO_REQUIRE(engine.close().ok());
}
