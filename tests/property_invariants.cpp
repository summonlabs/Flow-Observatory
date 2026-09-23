// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Invariants that must hold for arbitrary randomised evidence.

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

struct RandomEngine final {
  ManualClock clock;
  EngineConfig config;
  std::unique_ptr<Engine> engine;

  explicit RandomEngine(std::uint64_t seed) : rng(seed) {
    config.clock = &clock;
    config.limits.worker_threads = 0;
    config.limits.max_flows = 64;
    config.limits.max_generations_per_flow = 3;
    config.limits.max_observations_per_source_flow = 8;
    config.limits.max_history_per_flow = 16;
    config.limits.max_anomalies_per_flow = 8;
    clock.set(fotest::recent_now());
    engine = std::make_unique<Engine>(config);
  }

  fotest::Rng rng;
};

}  // namespace

FOTEST("invariant", "consumption_never_exceeds_the_last_observed_counter") {
  // A cumulative counter that only ever advances: the accumulated total is then
  // bounded by the largest value the source ever reported, and it can never
  // exceed the sum of the observed deltas.
  for (std::uint64_t round = 0; round < 6; ++round) {
    RandomEngine fixture(ctx.seed + round * 4099);
    FO_REQUIRE(fixture.engine->open().ok());
    std::uint64_t highest = 0;
    std::uint64_t revision = 0;
    for (int i = 0; i < 120; ++i) {
      highest += fixture.rng.below(1000);
      const std::uint64_t value = highest;
      ObservationSpec spec;
      spec.bytes = value;
      spec.revision = ++revision;
      spec.sequence = revision;
      spec.observed_at = static_cast<std::uint64_t>(base_time().nanos() +
                                                    static_cast<std::int64_t>(revision) * 1000);
      spec.received_at = spec.observed_at;
      (void)fixture.engine->submit(make_observation(spec));
      const Result<FlowSnapshot> snapshot = fixture.engine->snapshot(flow_id_from_key("flow-1"));
      FO_REQUIRE(snapshot.ok());
      // The accumulated total can never exceed the highest cumulative counter
      // the source ever reported.
      FO_CHECK(snapshot.value().total.bytes <= highest);
    }
    FO_REQUIRE(fixture.engine->close().ok());
  }
}

FOTEST("invariant", "attribution_conserves_the_total") {
  for (std::uint64_t round = 0; round < 20; ++round) {
    fotest::Rng rng(ctx.seed + 65537 + round * 101);
    FlowSnapshot flow;
    flow.id = flow_id_from_key("flow-x");
    GenerationView generation;
    generation.generation = FlowGeneration(flow.id, GenerationId::from_value(1));
    generation.is_current = true;
    generation.freshness = Freshness::Fresh;
    generation.total.bytes = rng.below(1000000);
    generation.total.packets = rng.below(100000);
    generation.total.retransmissions = rng.below(1000);
    generation.path = path_id_from_key("p0");
    const std::size_t hops = 1 + static_cast<std::size_t>(rng.below(8));
    for (std::size_t i = 0; i < hops; ++i) {
      generation.links.push_back(link_id_from_key("l" + std::to_string(i)));
      generation.hop_capacity_units.push_back(1 + rng.below(1000));
    }
    for (const auto method : {AttributionPolicy::Method::UniformAcrossHops,
                              AttributionPolicy::Method::ProportionalToCapacity}) {
      AttributionPolicy policy;
      policy.method = method;
      const AttributionReport report = attribute_generation(flow, generation, policy, 1024);
      FO_REQUIRE(report.ok());
      std::uint64_t bytes = 0;
      std::uint64_t packets = 0;
      std::uint64_t retransmissions = 0;
      for (const AttributedTarget& target : report.targets) {
        if (target.kind != AttributedTarget::Kind::Link) {
          continue;
        }
        bytes += target.consumption.bytes;
        packets += target.consumption.packets;
        retransmissions += target.consumption.retransmissions;
      }
      FO_CHECK_EQ(bytes, generation.total.bytes);
      FO_CHECK_EQ(packets, generation.total.packets);
      FO_CHECK_EQ(retransmissions, generation.total.retransmissions);
    }
  }
}

FOTEST("invariant", "uniform_split_is_reproducible_for_every_remainder") {
  for (std::size_t hops = 1; hops <= 12; ++hops) {
    for (std::uint64_t total = 0; total < 40; ++total) {
      FlowSnapshot flow;
      flow.id = flow_id_from_key("flow-y");
      GenerationView generation;
      generation.generation = FlowGeneration(flow.id, GenerationId::from_value(1));
      generation.is_current = true;
      generation.freshness = Freshness::Fresh;
      generation.total.bytes = total;
      generation.path = path_id_from_key("p0");
      for (std::size_t i = 0; i < hops; ++i) {
        generation.links.push_back(link_id_from_key("l" + std::to_string(i)));
      }
      AttributionPolicy policy;
      policy.method = AttributionPolicy::Method::UniformAcrossHops;
      const AttributionReport first = attribute_generation(flow, generation, policy, 1024);
      const AttributionReport second = attribute_generation(flow, generation, policy, 1024);
      FO_REQUIRE(first.ok());
      std::uint64_t sum = 0;
      for (std::size_t i = 0; i < first.targets.size(); ++i) {
        if (first.targets[i].kind != AttributedTarget::Kind::Link) {
          continue;
        }
        sum += first.targets[i].consumption.bytes;
        FO_CHECK_EQ(first.targets[i].consumption.bytes, second.targets[i].consumption.bytes);
      }
      FO_CHECK_EQ(sum, total);
    }
  }
}

FOTEST("invariant", "bounded_state_stays_bounded_under_adversarial_churn") {
  // A source that keeps changing generation and source identity must not grow
  // the runtime without bound, and every eviction must be counted.
  RandomEngine fixture(ctx.seed + 999983);
  fixture.config.limits.max_flows = 8;
  fixture.config.limits.max_generations_per_flow = 2;
  fixture.config.limits.max_sources_per_flow = 2;
  fixture.engine = std::make_unique<Engine>(fixture.config);
  FO_REQUIRE(fixture.engine->open().ok());
  std::uint64_t revision = 0;
  for (int i = 0; i < 2000; ++i) {
    ObservationSpec spec;
    spec.flow = "flow-" + std::to_string(fixture.rng.below(16));
    spec.source = "src-" + std::to_string(fixture.rng.below(6));
    spec.generation = 1 + fixture.rng.below(4);
    spec.revision = ++revision;
    spec.sequence = revision;
    spec.bytes = fixture.rng.below(10000);
    spec.observed_at = static_cast<std::uint64_t>(base_time().nanos() +
                                                  static_cast<std::int64_t>(revision) * 1000);
    spec.received_at = spec.observed_at;
    (void)fixture.engine->submit(make_observation(spec));
  }
  const EngineStats stats = fixture.engine->stats();
  FO_CHECK(stats.total_flows <= fixture.config.limits.max_flows);
  FO_CHECK(stats.observations_received == 2000ull);
  FO_CHECK(stats.observations_applied + stats.observations_duplicate +
               stats.observations_refused + stats.observations_conflicting ==
           2000ull);
  FlowQuery query;
  query.limit = 1000;
  const Result<QueryResult> result = fixture.engine->query(query);
  FO_REQUIRE(result.ok());
  for (const FlowSnapshot& snapshot : result.value().flows) {
    FO_CHECK(snapshot.generations.size() <= fixture.config.limits.max_generations_per_flow);
    for (const GenerationView& generation : snapshot.generations) {
      FO_CHECK(generation.sources.size() <= fixture.config.limits.max_sources_per_flow);
    }
  }
  FO_REQUIRE(fixture.engine->close().ok());
}

FOTEST("invariant", "export_row_count_matches_the_result_set") {
  RandomEngine fixture(ctx.seed + 104729);
  FO_REQUIRE(fixture.engine->open().ok());
  std::uint64_t revision = 0;
  for (int i = 0; i < 60; ++i) {
    ObservationSpec spec;
    spec.flow = "flow-" + std::to_string(i % 12);
    spec.revision = ++revision;
    spec.sequence = revision;
    spec.bytes = static_cast<std::uint64_t>(i) * 10;
    spec.observed_at = static_cast<std::uint64_t>(base_time().nanos() +
                                                  static_cast<std::int64_t>(revision) * 1000);
    spec.received_at = spec.observed_at;
    (void)fixture.engine->submit(make_observation(spec));
  }
  ExportOptions options;
  options.pretty = false;
  options.query.limit = 5;
  const Result<ExportResult> exported = fixture.engine->export_data(options);
  FO_REQUIRE(exported.ok());
  FO_CHECK_EQ(exported.value().rows, 5u);
  FO_CHECK(exported.value().truncated);
  options.query.limit = 100;
  const Result<ExportResult> full = fixture.engine->export_data(options);
  FO_REQUIRE(full.ok());
  FO_CHECK_EQ(full.value().rows, 12u);
  FO_CHECK(!full.value().truncated);
  FO_REQUIRE(fixture.engine->close().ok());
}
