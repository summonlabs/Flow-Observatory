// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Two sources disagreeing about the same flow. The example shows the three
// outcomes the runtime distinguishes: resolved by authority, corroborated, and
// conflicting.

#include <cstdio>
#include <string>
#include <vector>

#include <flowobs/flowobs.hpp>

namespace {

struct Case final {
  const char* title;
  std::uint32_t authority_a;
  std::uint32_t authority_b;
  std::uint64_t bytes_a;
  std::uint64_t bytes_b;
  bool tolerance;
};

flowobs::Observation observation(const std::string& source_key, const std::string& flow_key,
                                 std::uint64_t bytes, flowobs::Timestamp at) {
  flowobs::Observation observation;
  observation.source_key = source_key;
  observation.key.source = flowobs::source_id_from_key(source_key);
  observation.key.incarnation = flowobs::IncarnationId::from_value(1);
  observation.key.epoch = flowobs::EpochId::from_value(1);
  observation.key.revision = flowobs::RevisionId::from_value(2);
  observation.key.sequence = flowobs::SequenceNo::from_value(2);
  observation.subject.flow_key = flow_key;
  observation.subject.flow = flowobs::flow_id_from_key(flow_key);
  observation.subject.generation = flowobs::GenerationId::from_value(1);
  observation.kind = flowobs::ObservationKind::Sample;
  observation.direction = flowobs::Direction::Forward;
  observation.evidence = flowobs::Evidence::Complete;
  observation.observed_at = at;
  observation.received_at = at;
  observation.counters.bytes = bytes;
  return observation;
}

void seed(flowobs::Engine& engine, const std::string& source_key, std::uint64_t bytes,
          flowobs::Timestamp at) {
  flowobs::Observation first = observation(source_key, "flow-1", 0, at);
  first.key.revision = flowobs::RevisionId::from_value(1);
  first.key.sequence = flowobs::SequenceNo::from_value(1);
  (void)engine.submit(first);
  (void)engine.submit(observation(source_key, "flow-1", bytes, at));
}

}  // namespace

int main() {
  const std::vector<Case> cases = {
      {"higher authority decides", 20, 5, 1000, 4000, false},
      {"equal authority, within tolerance", 10, 10, 10000, 10020, true},
      {"equal authority, real disagreement", 10, 10, 1000, 4000, false},
  };

  int failures = 0;
  for (const Case& item : cases) {
    flowobs::ManualClock clock;
    clock.set(flowobs::Timestamp::from_nanos(1700000000000000000LL));
    flowobs::EngineConfig config;
    config.clock = &clock;
    config.limits.worker_threads = 0;
    config.reconciliation.absolute_tolerance_bytes = item.tolerance ? 100u : 0u;
    flowobs::Engine engine(config);
    if (engine.open().failed()) {
      return 1;
    }
    flowobs::SourceDescriptor a;
    a.canonical_key = "alpha";
    a.id = flowobs::source_id_from_key(a.canonical_key);
    a.authority = item.authority_a;
    flowobs::SourceDescriptor b;
    b.canonical_key = "beta";
    b.id = flowobs::source_id_from_key(b.canonical_key);
    b.authority = item.authority_b;
    (void)engine.register_source(a);
    (void)engine.register_source(b);

    const flowobs::Timestamp at = clock.now();
    seed(engine, "alpha", item.bytes_a, at);
    seed(engine, "beta", item.bytes_b, at);

    const flowobs::Result<flowobs::FlowSnapshot> snapshot =
        engine.snapshot(flowobs::flow_id_from_key("flow-1"));
    if (!snapshot.ok()) {
      ++failures;
      continue;
    }
    std::printf("%-34s consistency=%-13s state=%-12s total_bytes=%llu\n", item.title,
                std::string(flowobs::to_string(snapshot.value().consistency)).c_str(),
                std::string(flowobs::to_string(snapshot.value().state)).c_str(),
                static_cast<unsigned long long>(snapshot.value().total.bytes));
    (void)engine.close();
  }
  return failures == 0 ? 0 : 1;
}
