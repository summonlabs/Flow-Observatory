// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Restart semantics: what survives, what does not, and why. The example writes
// a journal, reopens it, and prints the difference.

#include <cstdio>
#include <filesystem>
#include <string>

#include <flowobs/flowobs.hpp>

namespace {

flowobs::Observation observation(flowobs::Timestamp at, std::uint64_t revision,
                                 std::uint64_t bytes) {
  flowobs::Observation observation;
  observation.source_key = "nic0";
  observation.key.source = flowobs::source_id_from_key(observation.source_key);
  observation.key.incarnation = flowobs::IncarnationId::from_value(7);
  observation.key.epoch = flowobs::EpochId::from_value(1);
  observation.key.revision = flowobs::RevisionId::from_value(revision);
  observation.key.sequence = flowobs::SequenceNo::from_value(revision);
  observation.subject.flow_key = "flow-1";
  observation.subject.flow = flowobs::flow_id_from_key(observation.subject.flow_key);
  observation.subject.generation = flowobs::GenerationId::from_value(1);
  observation.kind = flowobs::ObservationKind::Sample;
  observation.direction = flowobs::Direction::Forward;
  observation.evidence = flowobs::Evidence::Complete;
  observation.observed_at = at;
  observation.received_at = at;
  observation.counters.bytes = bytes;
  observation.counters.packets = bytes / 1024;
  return observation;
}

void report(const char* label, const flowobs::Engine& engine) {
  const flowobs::Result<flowobs::FlowSnapshot> snapshot =
      engine.snapshot(flowobs::flow_id_from_key("flow-1"));
  if (!snapshot.ok()) {
    std::printf("%-10s <no flow>\n", label);
    return;
  }
  std::printf("%-10s state=%-11s live=%-5s restored=%-5s bytes=%llu unknown_intervals=%llu\n",
              label, std::string(flowobs::to_string(snapshot.value().state)).c_str(),
              snapshot.value().is_live() ? "yes" : "no",
              snapshot.value().restored_from_journal ? "yes" : "no",
              static_cast<unsigned long long>(snapshot.value().total.bytes),
              static_cast<unsigned long long>(snapshot.value().total.unknown_intervals));
}

}  // namespace

int main() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "flowobs-example-restart.foj";
  std::error_code ec;
  std::filesystem::remove(path, ec);

  flowobs::ManualClock clock;
  clock.set(flowobs::Timestamp::from_nanos(1700000000000000000LL));
  flowobs::EngineConfig config;
  config.clock = &clock;
  config.journal_path = path;
  config.limits.worker_threads = 0;

  {
    flowobs::Engine engine(config);
    if (engine.open().failed()) {
      return 1;
    }
    (void)engine.submit(observation(clock.now(), 1, 0));
    clock.advance(flowobs::Duration::from_seconds(1));
    (void)engine.submit(observation(clock.now(), 2, 1048576));
    // Evaluate two seconds after the last observation, then let the runtime
    // settle, so that the flow is genuinely live when the first report prints.
    clock.advance(flowobs::Duration::from_seconds(2));
    (void)engine.tick(clock.now());
    report("before", engine);
    (void)engine.close();
  }

  // A new runtime in a new epoch. The clock jumps forward by an hour and the
  // runtime is asked to settle: nothing carried over from the journal is
  // promoted to live, and the accounting is preserved.
  {
    clock.advance(flowobs::Duration::from_hours(1));
    flowobs::Engine engine(config);
    if (engine.open().failed()) {
      return 1;
    }
    std::printf("runtime epoch after reopen: %llu\n",
                static_cast<unsigned long long>(engine.runtime_epoch().value()));
    report("after", engine);
    const flowobs::Result<flowobs::Explanation> explanation =
        engine.explain(flowobs::flow_id_from_key("flow-1"));
    if (explanation.ok()) {
      std::printf("%s", explanation.value().render().c_str());
    }
    (void)engine.close();
  }
  std::filesystem::remove(path, ec);
  return 0;
}
