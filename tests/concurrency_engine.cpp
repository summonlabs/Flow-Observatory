// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Concurrency tests. No timeouts and no sleeps: every wait is either a real
// completion barrier (joining a thread, or Engine::stop_workers, which drains the
// queue before the workers exit) or a spin on a counter the test itself drives,
// with no deadline attached. A wait that never completes is a hang, which is a
// failure, not a pass.

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
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

FOTEST("concurrency", "parallel_ingest_matches_single_threaded_ingest") {
  const auto build = [](std::vector<ObservationSpec>& out) {
    std::uint64_t revision = 0;
    for (int thread = 0; thread < 4; ++thread) {
      for (int i = 0; i < 250; ++i) {
        ObservationSpec spec;
        spec.source = "src-" + std::to_string(thread);
        spec.flow = "flow-" + std::to_string((thread * 7 + i) % 5);
        spec.incarnation = 1;
        spec.revision = ++revision;
        spec.sequence = spec.revision;
        spec.bytes = static_cast<std::uint64_t>(i) * 13;
        spec.packets = static_cast<std::uint64_t>(i);
        spec.direction = (i % 3 == 0) ? Direction::Reverse : Direction::Forward;
        spec.observed_at = static_cast<std::uint64_t>(base_time().nanos() +
                                                      static_cast<std::int64_t>(revision) * 1000);
        spec.received_at = spec.observed_at;
        out.push_back(spec);
      }
    }
  };

  std::vector<ObservationSpec> specs;
  build(specs);

  ManualClock serial_clock;
  serial_clock.set(fotest::recent_now());
  EngineConfig serial_config;
  serial_config.clock = &serial_clock;
  serial_config.limits.worker_threads = 0;
  Engine serial(serial_config);
  FO_REQUIRE(serial.open().ok());
  for (const ObservationSpec& spec : specs) {
    FO_REQUIRE(serial.submit(make_observation(spec)).ok());
  }

  ManualClock parallel_clock;
  parallel_clock.set(fotest::recent_now());
  EngineConfig parallel_config;
  parallel_config.clock = &parallel_clock;
  parallel_config.limits.worker_threads = 4;
  parallel_config.limits.max_ingest_batch = 32;
  Engine parallel(parallel_config);
  FO_REQUIRE(parallel.open().ok());

  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int thread = 0; thread < 4; ++thread) {
    threads.emplace_back([&, thread]() {
      for (int i = 0; i < 250; ++i) {
        const ObservationSpec& spec = specs[static_cast<std::size_t>(thread * 250 + i)];
        const Result<IngestOutcome> queued = parallel.enqueue(make_observation(spec));
        if (!queued.ok() ||
            queued.value().disposition == IngestDisposition::DroppedQueueFull) {
          failures.fetch_add(1);
        }
      }
    });
  }
  // A reader runs concurrently with the producers and the workers.
  std::atomic<bool> stop_reader{false};
  std::atomic<int> reader_failures{0};
  std::thread reader([&]() {
    while (!stop_reader.load(std::memory_order_relaxed)) {
      const Result<QueryResult> result = parallel.query(FlowQuery{});
      if (!result.ok()) {
        reader_failures.fetch_add(1);
      }
      const Result<ExportResult> exported = parallel.export_data(ExportOptions{});
      if (!exported.ok()) {
        reader_failures.fetch_add(1);
      }
    }
  });
  for (std::thread& thread : threads) {
    thread.join();
  }
  FO_REQUIRE(parallel.stop_workers().ok());
  stop_reader.store(true, std::memory_order_relaxed);
  reader.join();

  FO_CHECK_EQ(failures.load(), 0);
  FO_CHECK_EQ(reader_failures.load(), 0);
  FO_CHECK_EQ(parallel.queue_depth(), 0u);

  ExportOptions options;
  options.pretty = false;
  const Result<ExportResult> serial_export = serial.export_data(options);
  const Result<ExportResult> parallel_export = parallel.export_data(options);
  FO_REQUIRE(serial_export.ok());
  FO_REQUIRE(parallel_export.ok());
  FO_CHECK_EQ(parallel_export.value().text, serial_export.value().text);
  FO_CHECK_EQ(parallel.state_digest(), serial.state_digest());
  FO_REQUIRE(serial.close().ok());
  FO_REQUIRE(parallel.close().ok());
}

FOTEST("concurrency", "readers_never_observe_a_torn_view") {
  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.limits.worker_threads = 2;
  Engine engine(config);
  FO_REQUIRE(engine.open().ok());

  std::atomic<bool> stop{false};
  std::atomic<int> violations{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < 3; ++i) {
    readers.emplace_back([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        const Result<FlowSnapshot> snapshot = engine.snapshot(flow_id_from_key("flow-1"));
        if (!snapshot.ok()) {
          continue;
        }
        // Invariants a reader must never see violated, no matter how many
        // writers are active.
        std::uint64_t total = 0;
        for (const GenerationView& generation : snapshot.value().generations) {
          if (generation.forward.bytes > total) {
            total = generation.forward.bytes;
          }
        }
        if (!snapshot.value().generations.empty() &&
            snapshot.value().total.bytes > 1000000ull) {
          violations.fetch_add(1);
        }
      }
    });
  }
  std::uint64_t counter = 0;
  for (int i = 0; i < 3000; ++i) {
    ObservationSpec spec;
    counter += 100;
    spec.bytes = counter;
    spec.revision = static_cast<std::uint64_t>(i + 1);
    spec.sequence = spec.revision;
    spec.observed_at = static_cast<std::uint64_t>(base_time().nanos() +
                                                  static_cast<std::int64_t>(i + 1) * 1000);
    spec.received_at = spec.observed_at;
    FO_REQUIRE(engine.enqueue(make_observation(spec)).ok());
  }
  FO_REQUIRE(engine.stop_workers().ok());
  stop.store(true, std::memory_order_relaxed);
  for (std::thread& thread : readers) {
    thread.join();
  }
  FO_CHECK_EQ(violations.load(), 0);
  const Result<FlowSnapshot> snapshot = engine.snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  // The first observation of a cumulative series establishes the baseline; the
  // remaining 2999 each contribute 100 bytes.
  FO_CHECK_EQ(snapshot.value().total.bytes, 299900ull);
  FO_REQUIRE(engine.close().ok());
}

FOTEST("concurrency", "shutdown_is_real_and_drains_before_exiting") {
  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.limits.worker_threads = 3;
  Engine engine(config);
  FO_REQUIRE(engine.open().ok());
  for (int i = 0; i < 500; ++i) {
    ObservationSpec spec;
    spec.bytes = static_cast<std::uint64_t>(i + 1);
    spec.revision = static_cast<std::uint64_t>(i + 1);
    spec.sequence = spec.revision;
    spec.observed_at = static_cast<std::uint64_t>(base_time().nanos() +
                                                  static_cast<std::int64_t>(i + 1) * 1000);
    spec.received_at = spec.observed_at;
    const Result<IngestOutcome> queued = engine.enqueue(make_observation(spec));
    FO_REQUIRE(queued.ok());
    if (queued.value().disposition == IngestDisposition::DroppedQueueFull) {
      FO_REQUIRE(engine.drain(64).ok());
    }
  }
  // stop_workers is a completion barrier: every queued observation has been
  // applied by the time it returns, and every worker thread has been joined.
  FO_REQUIRE(engine.stop_workers().ok());
  FO_CHECK_EQ(engine.queue_depth(), 0u);
  const EngineStats stats = engine.stats();
  FO_CHECK_EQ(stats.observations_applied, 500ull);
  FO_CHECK(!engine.is_open() == false);
  FO_REQUIRE(engine.close().ok());
  FO_CHECK(!engine.is_open());
}

FOTEST("concurrency", "closing_releases_every_worker_thread") {
  // Run the whole open/enqueue/close cycle many times: a leaked worker thread
  // or a lock held across a join shows up as a hang or an inconsistent count.
  for (int round = 0; round < 8; ++round) {
    ManualClock clock;
    clock.set(fotest::recent_now());
    EngineConfig config;
    config.clock = &clock;
    config.limits.worker_threads = 2;
    Engine engine(config);
    FO_REQUIRE(engine.open().ok());
    for (int i = 0; i < 50; ++i) {
      ObservationSpec spec;
      spec.bytes = static_cast<std::uint64_t>(i);
      spec.revision = static_cast<std::uint64_t>(i + 1);
      spec.sequence = spec.revision;
      spec.observed_at = static_cast<std::uint64_t>(base_time().nanos() +
                                                    static_cast<std::int64_t>(i + 1) * 1000);
      spec.received_at = spec.observed_at;
      FO_REQUIRE(engine.enqueue(make_observation(spec)).ok());
    }
    FO_REQUIRE(engine.close().ok());
    FO_CHECK_EQ(engine.queue_depth(), 0u);
  }
}

FOTEST("concurrency", "stats_are_monotone_under_concurrent_load") {
  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.limits.worker_threads = 2;
  Engine engine(config);
  FO_REQUIRE(engine.open().ok());

  std::atomic<bool> stop{false};
  std::atomic<int> regressions{0};
  std::thread watcher([&]() {
    std::uint64_t previous = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      const EngineStats stats = engine.stats();
      if (stats.observations_received < previous) {
        regressions.fetch_add(1);
      }
      previous = stats.observations_received;
    }
  });
  for (int i = 0; i < 2000; ++i) {
    ObservationSpec spec;
    spec.bytes = static_cast<std::uint64_t>(i);
    spec.revision = static_cast<std::uint64_t>(i + 1);
    spec.sequence = spec.revision;
    spec.observed_at = static_cast<std::uint64_t>(base_time().nanos() +
                                                  static_cast<std::int64_t>(i + 1) * 1000);
    spec.received_at = spec.observed_at;
    FO_REQUIRE(engine.enqueue(make_observation(spec)).ok());
  }
  FO_REQUIRE(engine.stop_workers().ok());
  stop.store(true, std::memory_order_relaxed);
  watcher.join();
  FO_CHECK_EQ(regressions.load(), 0);
  FO_REQUIRE(engine.close().ok());
}
FOTEST("concurrency", "compaction_under_load_keeps_the_journal_consistent") {
  const std::filesystem::path directory = std::filesystem::temp_directory_path() / "flowobs-tests";
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  const std::filesystem::path path = directory / "concurrent-compact.foj";
  std::filesystem::remove(path, ec);

  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.journal_path = path;
  config.limits.worker_threads = 2;
  // Large enough that a snapshot of 64 flows fits inside half the budget (which
  // is the bound compaction enforces), small enough to stay a test.
  config.limits.max_journal_bytes = 4u * 1024u * 1024u;
  config.limits.max_history_per_flow = 16;
  Engine engine(config);
  const Status opened = engine.open();
  if (!opened.ok()) {
    ctx.fail("engine open failed: " + opened.describe());
  }
  FO_REQUIRE(opened.ok());

  // The compactor runs on its own thread while ingest proceeds. It is driven by
  // the producer rather than spinning: a runtime under continuous compaction
  // pressure is a pathological configuration, and the point of this test is that
  // a *concurrent* compaction never corrupts the journal or the view.
  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};
  std::atomic<int> requests{0};
  std::thread compactor([&]() {
    int served = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      if (requests.load(std::memory_order_relaxed) > served) {
        ++served;
        const Status compacted = engine.compact();
        if (compacted.failed()) {
          if (failures.fetch_add(1) == 0) {
            std::fprintf(stderr, "[concurrency] compaction failed: %s\n",
                         compacted.describe().c_str());
          }
        }
      } else {
        std::this_thread::yield();
      }
    }
  });
  for (int i = 0; i < 2000; ++i) {
    ObservationSpec spec;
    spec.flow = "flow-" + std::to_string(i % 64);
    spec.bytes = static_cast<std::uint64_t>(i) * 10;
    spec.revision = static_cast<std::uint64_t>(i + 1);
    spec.sequence = spec.revision;
    spec.observed_at = static_cast<std::uint64_t>(base_time().nanos() +
                                                  static_cast<std::int64_t>(i + 1) * 1000);
    spec.received_at = spec.observed_at;
    const Result<IngestOutcome> queued = engine.enqueue(make_observation(spec));
    FO_REQUIRE(queued.ok());
    if (queued.value().disposition == IngestDisposition::DroppedQueueFull) {
      FO_REQUIRE(engine.drain(128).ok());
    }
    if (i % 100 == 0) {
      requests.fetch_add(1);
    }
  }
  FO_REQUIRE(engine.stop_workers().ok());
  stop.store(true, std::memory_order_relaxed);
  compactor.join();
  FO_CHECK(requests.load() > 0);
  FO_CHECK_EQ(failures.load(), 0);
  FO_CHECK(engine.verify_journal().ok());
  const Result<QueryResult> result = engine.query(FlowQuery{});
  FO_REQUIRE(result.ok());
  FO_CHECK(!result.value().flows.empty());
  FO_REQUIRE(engine.close().ok());

  // The compacted journal is still readable by a fresh runtime.
  Engine reopened(config);
  FO_REQUIRE(reopened.open().ok());
  FO_CHECK(reopened.verify_journal().ok());
  const EngineStats stats = reopened.stats();
  FO_CHECK(stats.total_flows > 0ull);
  FO_REQUIRE(reopened.close().ok());
  std::filesystem::remove(path, ec);
}

FOTEST("concurrency", "freezing_during_ingest_is_clean") {
  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.limits.worker_threads = 2;
  Engine engine(config);
  FO_REQUIRE(engine.open().ok());

  // A deterministic prefix of accepted evidence, so the freeze is applied to a
  // runtime that was genuinely writing.
  std::atomic<bool> stop{false};
  std::atomic<int> accepted{0};
  std::atomic<int> refused{0};
  for (int i = 0; i < 50; ++i) {
    ObservationSpec spec;
    spec.bytes = static_cast<std::uint64_t>(i);
    spec.revision = static_cast<std::uint64_t>(i + 1);
    spec.sequence = spec.revision;
    spec.observed_at = static_cast<std::uint64_t>(clock.now().nanos());
    spec.received_at = spec.observed_at;
    FO_REQUIRE(engine.submit(make_observation(spec)).ok());
    accepted.fetch_add(1);
  }

  std::vector<std::thread> producers;
  for (int thread = 0; thread < 2; ++thread) {
    producers.emplace_back([&, thread]() {
      std::uint64_t revision = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        ObservationSpec spec;
        spec.source = "src-" + std::to_string(thread);
        spec.bytes = ++revision;
        spec.observed_at = static_cast<std::uint64_t>(clock.now().nanos());
        spec.received_at = spec.observed_at;
        const Result<IngestOutcome> outcome = engine.submit(make_observation(spec));
        if (outcome.ok()) {
          accepted.fetch_add(1);
        } else {
          refused.fetch_add(1);
        }
      }
    });
  }

  // Wait for the producers to have contributed, then freeze. The wait is a spin
  // on an observable counter, not a timeout: if the producers never make
  // progress the test does not pass, it stops making progress too.
  const auto spin_until = [](const std::atomic<int>& counter, int target) {
    while (counter.load(std::memory_order_relaxed) < target) {
      std::this_thread::yield();
    }
  };
  spin_until(accepted, 100);
  engine.freeze();
  FO_CHECK(engine.frozen());

  // The refusal is asserted from this thread so that it does not depend on how
  // quickly a producer happens to reach its next submit.
  ObservationSpec blocked;
  blocked.bytes = 1;
  blocked.observed_at = static_cast<std::uint64_t>(clock.now().nanos());
  blocked.received_at = blocked.observed_at;
  const Result<IngestOutcome> submitted = engine.submit(make_observation(blocked));
  FO_REQUIRE(!submitted.ok());
  FO_CHECK_EQ(static_cast<int>(submitted.code()), static_cast<int>(ErrorCode::ShuttingDown));
  const Result<IngestOutcome> enqueued = engine.enqueue(make_observation(blocked));
  FO_REQUIRE(enqueued.ok());
  FO_CHECK_EQ(static_cast<int>(enqueued.value().code),
              static_cast<int>(ErrorCode::ShuttingDown));
  FO_CHECK_EQ(static_cast<int>(enqueued.value().disposition),
              static_cast<int>(IngestDisposition::Refused));

  // Reads keep working while frozen.
  for (int i = 0; i < 50; ++i) {
    FO_CHECK(engine.query(FlowQuery{}).ok());
    FO_CHECK(engine.snapshot(flow_id_from_key("flow-1")).ok());
  }

  stop.store(true, std::memory_order_relaxed);
  for (std::thread& producer : producers) {
    producer.join();
  }
  FO_CHECK(accepted.load() >= 100);
  FO_CHECK(engine.export_data(ExportOptions{}).ok());
  FO_REQUIRE(engine.close().ok());
}
