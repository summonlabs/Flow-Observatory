// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Shared helpers for the suite: deterministic clock, observation builders and a
// seeded PRNG so that every randomised test is reproducible from its seed.

#ifndef FLOWOBS_TESTS_SUPPORT_HPP
#define FLOWOBS_TESTS_SUPPORT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "flowobs/flowobs.hpp"

namespace fotest {

// xorshift64* - tiny, deterministic, and good enough for test input generation.
class Rng final {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  std::uint64_t next() {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 0x2545F4914F6CDD1Dull;
  }
  std::uint64_t below(std::uint64_t bound) { return bound == 0 ? 0 : next() % bound; }
  bool chance(unsigned percent) { return below(100) < percent; }

 private:
  std::uint64_t state_;
};

inline flowobs::Timestamp base_time() {
  return flowobs::Timestamp::from_nanos(1700000000000000000LL);
}

// An evaluation instant that is comfortably after any observation a test
// creates from base_time(), so that evidence is judged against a later instant
// exactly as it would be in production.
inline flowobs::Timestamp recent_now() {
  return flowobs::Timestamp::from_nanos(1700000000000000000LL + 1000000000LL);
}

inline flowobs::SourceDescriptor make_source(const std::string& key, std::uint32_t authority,
                                             bool completion = true, bool reset = true,
                                             bool counters = true, bool path = true) {
  flowobs::SourceDescriptor descriptor;
  descriptor.canonical_key = key;
  descriptor.id = flowobs::source_id_from_key(key);
  descriptor.authority = authority;
  descriptor.capabilities.asserts_counters = counters;
  descriptor.capabilities.asserts_completion = completion;
  descriptor.capabilities.asserts_reset = reset;
  descriptor.capabilities.asserts_path = path;
  return descriptor;
}

struct ObservationSpec final {
  std::string source = "src-a";
  std::string flow = "flow-1";
  std::uint64_t generation = 1;
  std::uint64_t incarnation = 1;
  std::uint64_t epoch = 1;
  std::uint64_t revision = 1;
  std::uint64_t sequence = 1;
  std::uint64_t observed_at = 0;  // 0 means \"base_time + revision nanos\"
  std::uint64_t received_at = 0;  // 0 means \"same as observed_at\"
  flowobs::ObservationKind kind = flowobs::ObservationKind::Sample;
  flowobs::Direction direction = flowobs::Direction::Forward;
  flowobs::Evidence evidence = flowobs::Evidence::Complete;
  std::optional<std::uint64_t> bytes;
  std::optional<std::uint64_t> packets;
  std::optional<std::uint64_t> retransmissions;
  std::string endpoint = "ep-a";
  std::string peer_endpoint = "ep-b";
  std::string path;
  std::string queue;
  std::string link;
  std::optional<std::uint64_t> path_generation;
  std::vector<std::uint64_t> hop_capacity;
  bool set_counters = true;
};

inline flowobs::Observation make_observation(const ObservationSpec& spec) {
  flowobs::Observation observation;
  observation.source_key = spec.source;
  observation.key.source = flowobs::source_id_from_key(spec.source);
  observation.key.incarnation = flowobs::IncarnationId::from_value(spec.incarnation);
  observation.key.epoch = flowobs::EpochId::from_value(spec.epoch);
  observation.key.revision = flowobs::RevisionId::from_value(spec.revision);
  observation.key.sequence = flowobs::SequenceNo::from_value(spec.sequence);
  observation.subject.flow_key = spec.flow;
  observation.subject.flow = flowobs::flow_id_from_key(spec.flow);
  observation.subject.generation = flowobs::GenerationId::from_value(spec.generation);
  if (!spec.endpoint.empty()) {
    observation.subject.endpoint_key = spec.endpoint;
    observation.subject.endpoint = flowobs::endpoint_id_from_key(spec.endpoint);
  }
  if (!spec.peer_endpoint.empty()) {
    observation.subject.peer_endpoint_key = spec.peer_endpoint;
    observation.subject.peer_endpoint = flowobs::endpoint_id_from_key(spec.peer_endpoint);
  }
  if (!spec.path.empty()) {
    observation.subject.path_key = spec.path;
    observation.subject.path = flowobs::path_id_from_key(spec.path);
  }
  if (!spec.queue.empty()) {
    observation.subject.queue_key = spec.queue;
    observation.subject.queue = flowobs::queue_id_from_key(spec.queue);
  }
  if (!spec.link.empty()) {
    observation.subject.link_key = spec.link;
    observation.subject.link = flowobs::link_id_from_key(spec.link);
  }
  if (spec.path_generation.has_value()) {
    observation.subject.path_generation =
        flowobs::GenerationId::from_value(*spec.path_generation);
  }
  observation.subject.hop_capacity_units = spec.hop_capacity;
  observation.kind = spec.kind;
  observation.direction = spec.direction;
  observation.evidence = spec.evidence;
  if (spec.set_counters) {
    observation.counters.bytes = spec.bytes;
    observation.counters.packets = spec.packets;
    observation.counters.retransmissions = spec.retransmissions;
  }
  const std::int64_t observed =
      spec.observed_at != 0
          ? static_cast<std::int64_t>(spec.observed_at)
          : base_time().nanos() + static_cast<std::int64_t>(spec.revision);
  observation.observed_at = flowobs::Timestamp::from_nanos(observed);
  observation.received_at =
      spec.received_at != 0 ? flowobs::Timestamp::from_nanos(static_cast<std::int64_t>(spec.received_at))
                            : observation.observed_at;
  return observation;
}

// An engine wired to a manual clock, with a small, explicit policy.
struct Harness final {
  flowobs::ManualClock clock;
  flowobs::EngineConfig config;
  std::unique_ptr<flowobs::Engine> engine;

  explicit Harness(bool workers = false) {
    config.clock = &clock;
    config.limits.worker_threads = workers ? 2u : 0u;
    config.expiry.idle_after = flowobs::Duration::from_seconds(30);
    config.expiry.expire_after = flowobs::Duration::from_seconds(120);
    config.freshness.fresh_window = flowobs::Duration::from_seconds(30);
    config.freshness.expire_window = flowobs::Duration::from_seconds(120);
    config.limits.max_clock_skew = flowobs::Duration::from_seconds(5);
    clock.set(base_time());
    engine = std::make_unique<flowobs::Engine>(config);
  }

  flowobs::Status open() { return engine->open(); }

  flowobs::Result<flowobs::IngestOutcome> feed(const ObservationSpec& spec) {
    return engine->submit(make_observation(spec));
  }

  flowobs::Result<flowobs::FlowSnapshot> snapshot_of(const std::string& flow) const {
    return engine->snapshot(flowobs::flow_id_from_key(flow));
  }
};

}  // namespace fotest

#endif  // FLOWOBS_TESTS_SUPPORT_HPP
