// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Benchmarks. Every benchmark reports *completed* work: the number of units the
// measured loop actually finished inside the measured interval. Nothing here is
// extrapolated from a partial iteration, and nothing is reported as a rate
// without a completed count next to it.
//
// The numbers describe this machine and this build only. The runner prints the
// build description and the limits so a reader can tell exactly what was
// measured.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <flowobs/flowobs.hpp>

namespace {

using Clock = std::chrono::steady_clock;

struct Result final {
  std::string name;
  std::uint64_t completed = 0;
  std::string unit;
  double seconds = 0.0;

  [[nodiscard]] double per_second() const {
    return seconds > 0.0 ? static_cast<double>(completed) / seconds : 0.0;
  }
};

void print(const Result& result) {
  std::printf("%-42s %12llu %-12s %9.3f s  %14.0f /s\n", result.name.c_str(),
              static_cast<unsigned long long>(result.completed), result.unit.c_str(),
              result.seconds, result.per_second());
}

flowobs::Observation make_observation(std::uint64_t index) {
  flowobs::Observation observation;
  observation.source_key = "bench-source";
  observation.key.source = flowobs::source_id_from_key(observation.source_key);
  observation.key.incarnation = flowobs::IncarnationId::from_value(1);
  observation.key.epoch = flowobs::EpochId::from_value(1);
  observation.key.revision = flowobs::RevisionId::from_value(index + 1);
  observation.key.sequence = flowobs::SequenceNo::from_value(index + 1);
  const std::string flow_key = "bench-flow-" + std::to_string(index);
  observation.subject.flow_key = flow_key;
  observation.subject.flow = flowobs::flow_id_from_key(flow_key);
  observation.subject.generation = flowobs::GenerationId::from_value(1);
  observation.subject.endpoint_key = "bench-endpoint";
  observation.subject.endpoint = flowobs::endpoint_id_from_key("bench-endpoint");
  observation.kind = flowobs::ObservationKind::Sample;
  observation.direction = (index % 3 == 0) ? flowobs::Direction::Reverse
                                           : flowobs::Direction::Forward;
  observation.evidence = flowobs::Evidence::Complete;
  observation.observed_at = flowobs::Timestamp::from_nanos(
      1700000000000000000LL + static_cast<std::int64_t>(index) * 1000);
  observation.received_at = observation.observed_at;
  observation.counters.bytes = index * 512;
  observation.counters.packets = index;
  observation.counters.retransmissions = index % 7;
  return observation;
}

Result bench_ingest_sync(std::uint64_t count) {
  flowobs::ManualClock clock;
  clock.set(flowobs::Timestamp::from_nanos(1700000000000000000LL));
  flowobs::EngineConfig config;
  config.clock = &clock;
  config.limits.worker_threads = 0;
  flowobs::Engine engine(config);
  if (engine.open().failed()) {
    return {"ingest (single threaded)", 0, "observations", 0.0};
  }
  Result result;
  result.name = "ingest (single threaded)";
  result.unit = "observations";
  const auto start = Clock::now();
  for (std::uint64_t i = 0; i < count; ++i) {
    const flowobs::Result<flowobs::IngestOutcome> applied = engine.submit(make_observation(i));
    if (applied.ok() && applied.value().applied()) {
      ++result.completed;
    }
  }
  result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  (void)engine.close();
  return result;
}

Result bench_ingest_queued(std::uint64_t count, std::uint32_t workers) {
  flowobs::ManualClock clock;
  clock.set(flowobs::Timestamp::from_nanos(1700000000000000000LL));
  flowobs::EngineConfig config;
  config.clock = &clock;
  config.limits.worker_threads = workers;
  config.limits.max_pending_observations = 1u << 16;
  flowobs::Engine engine(config);
  if (engine.open().failed()) {
    return {"ingest (queued, workers)", 0, "observations", 0.0};
  }
  Result result;
  result.name = "ingest (queued, workers=" + std::to_string(workers) + ")";
  result.unit = "observations";
  const auto start = Clock::now();
  std::uint64_t queued = 0;
  for (std::uint64_t i = 0; i < count; ++i) {
    const flowobs::Result<flowobs::IngestOutcome> outcome = engine.enqueue(make_observation(i));
    if (!outcome.ok()) {
      break;
    }
    if (outcome.value().disposition == flowobs::IngestDisposition::DroppedQueueFull) {
      if (engine.drain(4096).failed()) {
        break;
      }
    }
    ++queued;
  }
  if (engine.stop_workers().failed()) {
    (void)engine.close();
    return result;
  }
  result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  result.completed = engine.stats().observations_applied;
  (void)queued;
  (void)engine.close();
  return result;
}

Result bench_query(std::uint64_t flows, std::uint64_t iterations) {
  flowobs::ManualClock clock;
  clock.set(flowobs::Timestamp::from_nanos(1700000000000000000LL));
  flowobs::EngineConfig config;
  config.clock = &clock;
  config.limits.worker_threads = 0;
  config.limits.max_flows = static_cast<std::uint32_t>(flows + 16);
  config.limits.max_result_set = static_cast<std::size_t>(flows + 16);
  config.query.max_rows = static_cast<std::size_t>(flows + 16);
  flowobs::Engine engine(config);
  if (engine.open().failed()) {
    return {"query", 0, "queries", 0.0};
  }
  for (std::uint64_t i = 0; i < flows; ++i) {
    (void)engine.submit(make_observation(i));
  }
  flowobs::FlowQuery query;
  query.limit = static_cast<std::size_t>(flows + 16);
  Result result;
  result.name = "query (all " + std::to_string(flows) + " flows)";
  result.unit = "queries";
  const auto start = Clock::now();
  for (std::uint64_t i = 0; i < iterations; ++i) {
    const flowobs::Result<flowobs::QueryResult> outcome = engine.query(query);
    if (outcome.ok()) {
      ++result.completed;
    }
  }
  result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  (void)engine.close();
  return result;
}

Result bench_export(std::uint64_t flows, std::uint64_t iterations) {
  flowobs::ManualClock clock;
  clock.set(flowobs::Timestamp::from_nanos(1700000000000000000LL));
  flowobs::EngineConfig config;
  config.clock = &clock;
  config.limits.worker_threads = 0;
  config.limits.max_flows = static_cast<std::uint32_t>(flows + 16);
  config.limits.max_result_set = static_cast<std::size_t>(flows + 16);
  config.query.max_rows = static_cast<std::size_t>(flows + 16);
  flowobs::Engine engine(config);
  if (engine.open().failed()) {
    return {"export", 0, "exports", 0.0};
  }
  for (std::uint64_t i = 0; i < flows; ++i) {
    (void)engine.submit(make_observation(i));
  }
  flowobs::ExportOptions options;
  options.pretty = false;
  options.query.limit = static_cast<std::size_t>(flows + 16);
  options.include_generations = true;
  Result result;
  result.name = "export json (" + std::to_string(flows) + " flows)";
  result.unit = "exports";
  const auto start = Clock::now();
  for (std::uint64_t i = 0; i < iterations; ++i) {
    const flowobs::Result<flowobs::ExportResult> outcome = engine.export_data(options);
    if (outcome.ok()) {
      ++result.completed;
    }
  }
  result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  (void)engine.close();
  return result;
}

Result bench_journal(std::uint64_t count, const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  flowobs::ManualClock clock;
  clock.set(flowobs::Timestamp::from_nanos(1700000000000000000LL));
  flowobs::EngineConfig config;
  config.clock = &clock;
  config.journal_path = path;
  config.limits.worker_threads = 0;
  config.limits.max_journal_bytes = 1ull << 30;
  flowobs::Engine engine(config);
  if (engine.open().failed()) {
    return {"journal append", 0, "records", 0.0};
  }
  Result result;
  result.name = "journal append + sync";
  result.unit = "records";
  const auto start = Clock::now();
  for (std::uint64_t i = 0; i < count; ++i) {
    const flowobs::Result<flowobs::IngestOutcome> applied = engine.submit(make_observation(i));
    if (applied.ok() && applied.value().applied()) {
      ++result.completed;
    }
  }
  result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  (void)engine.close();
  std::filesystem::remove(path, ec);
  return result;
}

Result bench_restart(std::uint64_t flows, const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  flowobs::ManualClock clock;
  clock.set(flowobs::Timestamp::from_nanos(1700000000000000000LL));
  flowobs::EngineConfig config;
  config.clock = &clock;
  config.journal_path = path;
  config.limits.worker_threads = 0;
  config.limits.max_flows = static_cast<std::uint32_t>(flows + 16);
  {
    flowobs::Engine engine(config);
    if (engine.open().failed()) {
      return {"restart recovery", 0, "flows", 0.0};
    }
    for (std::uint64_t i = 0; i < flows; ++i) {
      (void)engine.submit(make_observation(i));
    }
    (void)engine.compact();
    (void)engine.close();
  }
  Result result;
  result.name = "restart recovery (" + std::to_string(flows) + " flows)";
  result.unit = "flows";
  const auto start = Clock::now();
  flowobs::Engine engine(config);
  if (engine.open().failed()) {
    return result;
  }
  result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  result.completed = engine.stats().total_flows;
  (void)engine.close();
  std::filesystem::remove(path, ec);
  return result;
}

Result bench_attribution(std::uint64_t iterations) {
  flowobs::ManualClock clock;
  clock.set(flowobs::Timestamp::from_nanos(1700000000000000000LL));
  flowobs::EngineConfig config;
  config.clock = &clock;
  config.limits.worker_threads = 0;
  flowobs::Engine engine(config);
  if (engine.open().failed()) {
    return {"attribution", 0, "reports", 0.0};
  }
  flowobs::Observation observation = make_observation(64);
  std::vector<std::uint64_t> capacity;
  for (int i = 0; i < 16; ++i) {
    capacity.push_back(static_cast<std::uint64_t>(i + 1));
  }
  observation.subject.link_key = "bench-link";
  observation.subject.link = flowobs::link_id_from_key(observation.subject.link_key);
  observation.subject.hop_capacity_units = capacity;
  (void)engine.submit(observation);

  flowobs::AttributionPolicy policy;
  policy.method = flowobs::AttributionPolicy::Method::UniformAcrossHops;
  const flowobs::FlowId flow = observation.subject.flow;
  Result result;
  result.name = "attribution (16 hops)";
  result.unit = "reports";
  const auto start = Clock::now();
  for (std::uint64_t i = 0; i < iterations; ++i) {
    const flowobs::Result<flowobs::AttributionReport> report =
        engine.attribute_current(flow);
    if (report.ok()) {
      ++result.completed;
    }
  }
  result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  (void)engine.close();
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t scale = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument.rfind("--scale=", 0) == 0) {
      scale = std::strtoull(argument.c_str() + 8, nullptr, 10);
      if (scale == 0) {
        scale = 1;
      }
    }
  }

  std::printf("%s", flowobs::describe_build().c_str());
  flowobs::Limits limits;
  std::printf("scale=%llu\n", static_cast<unsigned long long>(scale));
  std::printf("%-42s %12s %-12s %11s  %14s\n", "benchmark", "completed", "unit", "elapsed",
              "throughput");

  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "flowobs-bench";
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);

  const std::vector<Result> results = {
      bench_ingest_sync(200000 * scale),
      bench_ingest_queued(200000 * scale, 4),
      bench_query(2000, 50 * scale),
      bench_export(2000, 20 * scale),
      bench_journal(50000 * scale, directory / "bench-journal.foj"),
      bench_restart(20000 * scale, directory / "bench-restart.foj"),
      bench_attribution(20000 * scale),
  };
  std::uint64_t completed_total = 0;
  for (const Result& result : results) {
    print(result);
    completed_total += result.completed;
  }
  std::printf("%-42s %12llu completed units in total\n", "TOTAL",
              static_cast<unsigned long long>(completed_total));
  std::printf("limits: max_flows=%u max_journal_bytes=%llu worker_threads=%u\n",
              limits.max_flows, static_cast<unsigned long long>(limits.max_journal_bytes),
              limits.worker_threads);
  return 0;
}
