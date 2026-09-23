// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The evidence envelope. Every observation states what was observed, by which
// source, for which generation, at what observation and receive times, under
// which source incarnation and epoch, and what the source claims about the
// completeness of its own statement.

#ifndef FLOWOBS_OBSERVATION_HPP
#define FLOWOBS_OBSERVATION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "flowobs/counters.hpp"
#include "flowobs/export.hpp"
#include "flowobs/identity.hpp"
#include "flowobs/semantics.hpp"
#include "flowobs/time.hpp"
#include "flowobs/version.hpp"

namespace flowobs {

// Fixed-size identity of an observation. Two observations are the same
// observation exactly when their keys are equal. The digest is computed over
// the canonical serialization of every semantically significant field.
struct FLOWOBS_PUBLIC ObservationKey final {
  SourceId source{};
  IncarnationId incarnation{};
  EpochId epoch{};
  RevisionId revision{};
  SequenceNo sequence{};

  friend bool operator==(const ObservationKey& a, const ObservationKey& b) noexcept {
    return a.source == b.source && a.incarnation == b.incarnation && a.epoch == b.epoch &&
           a.revision == b.revision && a.sequence == b.sequence;
  }
  friend bool operator!=(const ObservationKey& a, const ObservationKey& b) noexcept {
    return !(a == b);
  }
  friend bool operator<(const ObservationKey& a, const ObservationKey& b) noexcept {
    if (a.source != b.source) return a.source < b.source;
    if (a.incarnation != b.incarnation) return a.incarnation < b.incarnation;
    if (a.epoch != b.epoch) return a.epoch < b.epoch;
    if (a.revision != b.revision) return a.revision < b.revision;
    return a.sequence < b.sequence;
  }
  [[nodiscard]] std::string describe() const;
};

// Source-declared capabilities. A source that does not hold a capability can
// never move a flow into the state that capability guards, no matter how
// confident its payload looks.
struct FLOWOBS_PUBLIC SourceCapabilities final {
  bool asserts_completion = false;
  bool asserts_reset = false;
  bool asserts_path = false;
  bool asserts_queue = false;
  bool asserts_counters = true;
  bool authoritative_identity = false;

  [[nodiscard]] bool empty() const noexcept {
    return !asserts_completion && !asserts_reset && !asserts_path && !asserts_queue &&
           !asserts_counters && !authoritative_identity;
  }
  [[nodiscard]] std::string describe() const;
};

// A source as declared to the runtime. Authority is an explicit ordinal: the
// higher the value, the more the runtime trusts the source when sources
// disagree. Authority is never inferred from arrival order or volume.
struct FLOWOBS_PUBLIC SourceDescriptor final {
  SourceId id{};
  std::string canonical_key;
  std::uint32_t authority = 0;
  SourceCapabilities capabilities{};
  // When true the source's observations may only enrich, never drive state.
  bool advisory_only = false;

  [[nodiscard]] bool valid() const noexcept { return id.valid(); }
};

// The subject of an observation: which flow, which generation, and which
// resource the reading is attached to.
struct FLOWOBS_PUBLIC ObservationSubject final {
  FlowId flow{};
  GenerationId generation{};
  EndpointId endpoint{};        // endpoint the reading was taken at (if any)
  EndpointId peer_endpoint{};   // the other endpoint, when known
  std::optional<PathId> path;
  std::optional<QueueId> queue;
  std::optional<LinkId> link;
  // The generation the source believes the path belongs to. When set and not
  // equal to `generation`, the observation describes a superseded path
  // generation and is refused: stale path generations cannot support current
  // attribution.
  std::optional<GenerationId> path_generation;
  // Capacity metadata for the declared path hops, in the same order as the
  // path's links. Supplied by the source; never inferred. Absent means
  // capacity-weighted attribution is unsupported for this observation.
  std::vector<std::uint64_t> hop_capacity_units;

  // Canonical keys. `flow_key` is required: it is the human facing identity of
  // the flow and the engine proves that it hashes to `flow`. The remaining keys
  // are optional enrichment, but when supplied they must hash to the matching
  // identity, so a caller cannot attach evidence to an identity it does not
  // actually name.
  std::string flow_key;
  std::string endpoint_key;
  std::string peer_endpoint_key;
  std::string path_key;
  std::string queue_key;
  std::string link_key;

  [[nodiscard]] bool valid() const noexcept { return flow.valid() && generation.valid(); }
};

// One piece of evidence.
struct FLOWOBS_PUBLIC Observation final {
  // --- provenance -------------------------------------------------------
  ObservationKey key{};
  // Canonical key of the producing source, when the producer knows it. It is
  // carried so that the text format round trips exactly; the runtime keys
  // provenance on `key.source` alone.
  std::string source_key;
  // Self declared capabilities. They are recorded for provenance only: the
  // authority a source actually holds comes from the descriptor supplied to
  // Engine::register_source, so a source can never grant itself the right to
  // declare completion by asserting it in a payload.
  SourceCapabilities capabilities{};

  // --- subject ----------------------------------------------------------
  ObservationSubject subject{};

  // --- timing -----------------------------------------------------------
  Timestamp observed_at{};   // when the source says the event happened
  Timestamp received_at{};   // when this runtime accepted the record

  // --- content ----------------------------------------------------------
  ObservationKind kind = ObservationKind::Sample;
  Direction direction = Direction::Unspecified;
  Evidence evidence = Evidence::Unknown;
  CounterSet counters{};

  // Duration covered by this observation when the source reports one (for
  // example an interval sample). Absent when the source reports a point
  // sample.
  std::optional<Duration> covered_duration;
  // Source-reported sequence/order hints that are not part of the key.
  std::optional<Duration> reported_rtt;

  std::uint32_t schema_version = kObservationSchemaVersion;

  [[nodiscard]] bool has_key() const noexcept { return key.source.valid(); }
  [[nodiscard]] bool valid() const noexcept { return has_key() && subject.valid(); }

  // Canonical serialization used for identity digests, idempotence checks and
  // exports. Deterministic: field order and separators are fixed by
  // docs/semantics.md.
  //
  // `include_receive_time` selects whether the instant this runtime received the
  // record is part of the serialization. The receive instant is runtime
  // bookkeeping, not something the source said, so evidence identity is defined
  // without it: that is what makes a redelivery idempotent even though it is
  // received at a different instant. The full form is still what exports and
  // diagnostics show.
  [[nodiscard]] std::string canonical_form(bool include_receive_time = true) const;

  // 64-bit digest of the full canonical form, including the receive instant.
  [[nodiscard]] std::uint64_t digest() const noexcept;

  // 64-bit digest of what the source actually said: every field except the
  // instant this runtime received the record. Two records with equal evidence
  // digests are the same evidence, whenever they arrived.
  [[nodiscard]] std::uint64_t evidence_digest() const noexcept;

  // Hard structural validation against the configured bounds. Does not consult
  // any state.
  [[nodiscard]] std::string describe() const;
};

// Compares two observations for evidence equality: same key and same
// source-declared content, regardless of when each copy was received.
[[nodiscard]] FLOWOBS_PUBLIC bool same_content(const Observation& a,
                                               const Observation& b) noexcept;

// Convenience constructors: the identity is derived from the key with the
// documented, versioned hash, so callers never invent an identity by hand.
FLOWOBS_PUBLIC FlowId flow_id_from_key(std::string_view key) noexcept;
FLOWOBS_PUBLIC EndpointId endpoint_id_from_key(std::string_view key) noexcept;
FLOWOBS_PUBLIC PathId path_id_from_key(std::string_view key) noexcept;
FLOWOBS_PUBLIC LinkId link_id_from_key(std::string_view key) noexcept;
FLOWOBS_PUBLIC QueueId queue_id_from_key(std::string_view key) noexcept;
FLOWOBS_PUBLIC SourceId source_id_from_key(std::string_view key) noexcept;

// True when `key` hashes to `id` under the current identity scheme.
FLOWOBS_PUBLIC bool key_matches(FlowId id, std::string_view key) noexcept;
FLOWOBS_PUBLIC bool key_matches(EndpointId id, std::string_view key) noexcept;
FLOWOBS_PUBLIC bool key_matches(PathId id, std::string_view key) noexcept;
FLOWOBS_PUBLIC bool key_matches(LinkId id, std::string_view key) noexcept;
FLOWOBS_PUBLIC bool key_matches(QueueId id, std::string_view key) noexcept;
FLOWOBS_PUBLIC bool key_matches(SourceId id, std::string_view key) noexcept;

// The digest that participates in duplicate and conflict detection. It covers
// everything the source declared but not the instant this runtime received the
// record, so a redelivery of the same evidence is recognised as such while a
// genuinely different record under the same key is still refused.
[[nodiscard]] FLOWOBS_PUBLIC std::uint64_t content_digest(const Observation& obs) noexcept;

}  // namespace flowobs

#endif  // FLOWOBS_OBSERVATION_HPP
