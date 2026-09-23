// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The deadlock and lock-reentrancy audit is a mechanism, not a claim: this file
// tests the mechanism itself and then exercises every documented acquisition
// order.

#include <thread>
#include <vector>

#include "flowobs/flowobs.hpp"
#include "fotest.hpp"
#include "lockorder.hpp"
#include "support.hpp"

using namespace flowobs;
using fotest::ObservationSpec;
using fotest::Rng;
using fotest::base_time;
using fotest::make_observation;
using fotest::make_source;

FOTEST("lockorder", "checker_detects_inversion_and_reentrancy") {
  detail::LockAudit::reset_violations();
  const std::uint64_t before = detail::LockAudit::violations();
  const char* violation = nullptr;

  // Correct order: engine then queue.
  FO_CHECK(detail::LockAudit::enter(detail::LockRank::Engine, &violation));
  FO_CHECK(detail::LockAudit::enter(detail::LockRank::Queue, &violation));
  detail::LockAudit::leave(detail::LockRank::Queue);
  detail::LockAudit::leave(detail::LockRank::Engine);
  FO_CHECK_EQ(detail::LockAudit::depth(), 0);

  // Inversion: queue then engine.
  FO_CHECK(detail::LockAudit::enter(detail::LockRank::Queue, &violation));
  FO_CHECK(!detail::LockAudit::enter(detail::LockRank::Engine, &violation));
  detail::LockAudit::leave(detail::LockRank::Engine);
  detail::LockAudit::leave(detail::LockRank::Queue);

  // Re-entrancy of the same rank.
  FO_CHECK(detail::LockAudit::enter(detail::LockRank::Engine, &violation));
  FO_CHECK(!detail::LockAudit::enter(detail::LockRank::Engine, &violation));
  detail::LockAudit::leave(detail::LockRank::Engine);
  detail::LockAudit::leave(detail::LockRank::Engine);

  FO_CHECK(detail::LockAudit::violations() >= before + 2);
  FO_CHECK_EQ(detail::LockAudit::depth(), 0);
  detail::LockAudit::reset_violations();
}

FOTEST("lockorder", "real_engine_operations_never_violate_the_order") {
  detail::LockAudit::reset_violations();
  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.limits.worker_threads = 2;
  Engine engine(config);
  FO_REQUIRE(engine.open().ok());

  // Every documented entry point, including the nested engine -> queue order
  // taken by Engine::stats.
  for (int i = 0; i < 50; ++i) {
    ObservationSpec spec;
    spec.bytes = static_cast<std::uint64_t>(i);
    spec.revision = static_cast<std::uint64_t>(i + 1);
    spec.sequence = spec.revision;
    spec.observed_at = static_cast<std::uint64_t>(base_time().nanos() +
                                                  static_cast<std::int64_t>(i + 1) * 1000);
    spec.received_at = spec.observed_at;
    FO_REQUIRE(engine.submit(make_observation(spec)).ok());
    FO_REQUIRE(engine.enqueue(make_observation(spec)).ok());
    (void)engine.stats();
    (void)engine.query(FlowQuery{});
    (void)engine.snapshot(flow_id_from_key("flow-1"));
    (void)engine.explain(flow_id_from_key("flow-1"));
    (void)engine.export_data(ExportOptions{});
    (void)engine.attribute_current(flow_id_from_key("flow-1"));
    FO_REQUIRE(engine.tick(clock.now()).ok());
    FO_REQUIRE(engine.drain(64).ok());
  }
  FO_REQUIRE(engine.stop_workers().ok());
  FO_REQUIRE(engine.close().ok());
  FO_CHECK_EQ(detail::LockAudit::violations(), 0ull);
}

FOTEST("lockorder", "many_threads_contending_never_violate_the_order") {
  detail::LockAudit::reset_violations();
  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.limits.worker_threads = 3;
  Engine engine(config);
  FO_REQUIRE(engine.open().ok());

  std::vector<std::thread> threads;
  for (int thread = 0; thread < 6; ++thread) {
    threads.emplace_back([&, thread]() {
      for (int i = 0; i < 150; ++i) {
        ObservationSpec spec;
        spec.source = "src-" + std::to_string(thread);
        spec.flow = "flow-" + std::to_string(i % 3);
        spec.bytes = static_cast<std::uint64_t>(i);
        spec.revision = static_cast<std::uint64_t>(i + 1);
        spec.sequence = spec.revision;
        spec.observed_at = static_cast<std::uint64_t>(base_time().nanos() +
                                                      static_cast<std::int64_t>(i + 1) * 1000);
        spec.received_at = spec.observed_at;
        (void)engine.enqueue(make_observation(spec));
        if (thread % 2 == 0) {
          (void)engine.query(FlowQuery{});
          (void)engine.stats();
        } else {
          (void)engine.drain(4);
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  FO_REQUIRE(engine.stop_workers().ok());
  FO_REQUIRE(engine.drain(1 << 20).ok());
  FO_REQUIRE(engine.close().ok());
  FO_CHECK_EQ(detail::LockAudit::violations(), 0ull);
}

FOTEST("lockorder", "documented_rank_order_is_total") {
  FO_CHECK(static_cast<int>(detail::LockRank::Engine) <
           static_cast<int>(detail::LockRank::Queue));
  FO_CHECK(static_cast<int>(detail::LockRank::Queue) <
           static_cast<int>(detail::LockRank::Journal));
  FO_CHECK_EQ(std::string(detail::lock_rank_name(detail::LockRank::Engine)),
              std::string("engine"));
  FO_CHECK_EQ(std::string(detail::lock_rank_name(detail::LockRank::Queue)),
              std::string("queue"));
  FO_CHECK_EQ(std::string(detail::lock_rank_name(detail::LockRank::Journal)),
              std::string("journal"));
}
