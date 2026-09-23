// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Downstream consumer. It links only through the exported target and exercises
// enough of the API to prove that the installed headers, the library and the
// package configuration are complete and self consistent.

#include <cstdio>
#include <memory>
#include <string>

#include <flowobs/flowobs.hpp>

int main() {
  std::printf("consumer: flow-observatory %s\n", std::string(flowobs::version_string()).c_str());
  if (!flowobs::compatible_with(flowobs::kApiVersion)) {
    std::printf("consumer: api version mismatch\n");
    return 1;
  }

  flowobs::ManualClock clock;
  flowobs::EngineConfig config;
  config.clock = &clock;
  config.limits.worker_threads = 0;
  flowobs::Engine engine(config);
  if (engine.open().failed()) {
    std::printf("consumer: engine did not open\n");
    return 1;
  }

  flowobs::SourceDescriptor source;
  source.canonical_key = "consumer-source";
  source.id = flowobs::source_id_from_key(source.canonical_key);
  source.authority = 5;
  source.capabilities.asserts_completion = true;
  source.capabilities.asserts_counters = true;
  if (engine.register_source(source).failed()) {
    std::printf("consumer: source registration failed\n");
    return 1;
  }

  flowobs::Observation observation;
  observation.source_key = source.canonical_key;
  observation.key.source = source.id;
  observation.key.incarnation = flowobs::IncarnationId::from_value(1);
  observation.key.epoch = flowobs::EpochId::from_value(1);
  observation.key.revision = flowobs::RevisionId::from_value(1);
  observation.key.sequence = flowobs::SequenceNo::from_value(1);
  observation.subject.flow_key = "consumer-flow";
  observation.subject.flow = flowobs::flow_id_from_key(observation.subject.flow_key);
  observation.subject.generation = flowobs::GenerationId::from_value(1);
  observation.kind = flowobs::ObservationKind::Sample;
  observation.direction = flowobs::Direction::Forward;
  observation.evidence = flowobs::Evidence::Complete;
  observation.observed_at = clock.now();
  observation.received_at = clock.now();
  observation.counters.bytes = 4096;
  observation.counters.packets = 8;

  const flowobs::Result<flowobs::IngestOutcome> applied = engine.submit(observation);
  if (!applied.ok() || !applied.value().applied()) {
    std::printf("consumer: ingest refused: %s\n", applied.status().describe().c_str());
    return 1;
  }

  const flowobs::Result<flowobs::FlowSnapshot> snapshot = engine.snapshot(observation.subject.flow);
  if (!snapshot.ok()) {
    std::printf("consumer: snapshot failed\n");
    return 1;
  }
  std::printf("consumer: flow state=%s freshness=%s visibility=%s\n",
              std::string(flowobs::to_string(snapshot.value().state)).c_str(),
              std::string(flowobs::to_string(snapshot.value().freshness)).c_str(),
              std::string(flowobs::to_string(snapshot.value().visibility)).c_str());

  flowobs::ExportOptions options;
  options.format = flowobs::ExportOptions::Format::Json;
  options.pretty = false;
  const flowobs::Result<flowobs::ExportResult> exported = engine.export_data(options);
  if (!exported.ok() || exported.value().text.empty()) {
    std::printf("consumer: export failed\n");
    return 1;
  }
  std::printf("consumer: export bytes=%zu rows=%zu\n", exported.value().text.size(),
              exported.value().rows);
  if (engine.close().failed()) {
    std::printf("consumer: close failed\n");
    return 1;
  }
  std::printf("consumer: ok\n");
  return 0;
}
