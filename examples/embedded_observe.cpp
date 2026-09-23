// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Minimal embedding: register a source, feed it evidence, read the view back.
//
// The example runs entirely against a deterministic ManualClock so that its
// output is byte identical on every machine.

#include <cstdio>
#include <string>

#include <flowobs/flowobs.hpp>

namespace {

flowobs::Observation make(flowobs::SourceId source, const std::string& flow_key,
                          std::uint64_t revision, std::uint64_t bytes,
                          flowobs::ObservationKind kind, flowobs::Direction direction,
                          flowobs::Timestamp at) {
  flowobs::Observation observation;
  observation.source_key = "nic0";
  observation.key.source = source;
  observation.key.incarnation = flowobs::IncarnationId::from_value(1);
  observation.key.epoch = flowobs::EpochId::from_value(1);
  observation.key.revision = flowobs::RevisionId::from_value(revision);
  observation.key.sequence = flowobs::SequenceNo::from_value(revision);
  observation.subject.flow_key = flow_key;
  observation.subject.flow = flowobs::flow_id_from_key(flow_key);
  observation.subject.generation = flowobs::GenerationId::from_value(1);
  observation.subject.endpoint_key = "host-a:443";
  observation.subject.endpoint = flowobs::endpoint_id_from_key("host-a:443");
  observation.kind = kind;
  observation.direction = direction;
  observation.evidence = flowobs::Evidence::Complete;
  observation.observed_at = at;
  observation.received_at = at;
  observation.counters.bytes = bytes;
  observation.counters.packets = bytes / 512;
  observation.counters.retransmissions = 0;
  return observation;
}

}  // namespace

int main() {
  flowobs::ManualClock clock;
  clock.set(flowobs::Timestamp::from_nanos(1700000000000000000LL));

  flowobs::EngineConfig config;
  config.clock = &clock;
  config.limits.worker_threads = 0;

  flowobs::Engine engine(config);
  if (engine.open().failed()) {
    std::printf("engine did not open\n");
    return 1;
  }

  flowobs::SourceDescriptor source;
  source.canonical_key = "nic0";
  source.id = flowobs::source_id_from_key(source.canonical_key);
  source.authority = 10;
  source.capabilities.asserts_counters = true;
  source.capabilities.asserts_completion = true;
  if (engine.register_source(source).failed()) {
    std::printf("source registration failed\n");
    return 1;
  }

  const std::string flow_key = "10.0.0.1:443 -> 10.0.0.9:5201";
  for (std::uint64_t revision = 1; revision <= 4; ++revision) {
    // The source clock advances with the flow, exactly as a real producer's
    // would.
    clock.advance(flowobs::Duration::from_seconds(1));
    const flowobs::Result<flowobs::IngestOutcome> applied = engine.submit(make(
        source.id, flow_key, revision, revision * 4096,
        revision == 1 ? flowobs::ObservationKind::Open : flowobs::ObservationKind::Sample,
        flowobs::Direction::Forward, clock.now()));
    if (!applied.ok()) {
      std::printf("ingest refused: %s\n", applied.status().describe().c_str());
      return 1;
    }
  }

  // Move the evaluation instant past the last observation and let the runtime
  // settle: state advances on accepted evidence or on an explicit tick, never
  // from an implicit clock read.
  clock.advance(flowobs::Duration::from_seconds(5));
  const flowobs::Result<flowobs::TickReport> ticked = engine.tick(clock.now());
  if (!ticked.ok()) {
    std::printf("tick failed: %s\n", ticked.status().describe().c_str());
    return 1;
  }

  const flowobs::FlowId flow = flowobs::flow_id_from_key(flow_key);
  const flowobs::Result<flowobs::FlowSnapshot> snapshot = engine.snapshot(flow);
  if (!snapshot.ok()) {
    std::printf("snapshot failed\n");
    return 1;
  }
  std::printf("%s\n", snapshot.value().one_line().c_str());

  const flowobs::Result<flowobs::AttributionReport> attributed = engine.attribute_current(flow);
  if (attributed.ok()) {
    std::printf("%s\n", attributed.value().describe().c_str());
  }

  const flowobs::Result<flowobs::Explanation> explanation = engine.explain(flow);
  if (explanation.ok()) {
    std::printf("%s", explanation.value().render().c_str());
  }
  return engine.close().ok() ? 0 : 1;
}
