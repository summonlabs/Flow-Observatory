// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Versioned, integrity-checked, append-only journal.
//
// Layout
// ------
//   file header (64 bytes, CRC protected)
//   record*       (kind u16 | reserved u16 | length u32 | crc u32 | payload)
//   ...
//
// Every record carries its own CRC-32C computed over kind, reserved, length and
// payload. The reader refuses anything it does not fully understand: an unknown
// record kind, a bad CRC, a length beyond the configured bound or a payload
// that does not decode are all hard failures. Only a *torn tail* (a record
// whose header or payload is incomplete at end of file) may be dropped, and
// only when the recovery policy explicitly allows it.

#ifndef FLOWOBS_PERSISTENCE_HPP
#define FLOWOBS_PERSISTENCE_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "flowobs/counters.hpp"
#include "flowobs/export.hpp"
#include "flowobs/flow.hpp"
#include "flowobs/identity.hpp"
#include "flowobs/limits.hpp"
#include "flowobs/observation.hpp"
#include "flowobs/policy.hpp"
#include "flowobs/semantics.hpp"
#include "flowobs/time.hpp"

namespace flowobs {

// ---------------------------------------------------------------------------
// Durable projection of the runtime state.
// ---------------------------------------------------------------------------

// Durable state of one source's evidence about one flow generation. Freshness
// is deliberately absent: it is recomputed from the evaluation instant and the
// runtime epoch, never restored.
struct FLOWOBS_PUBLIC PersistedSource final {
  SourceId source{};
  std::string canonical_key;
  std::uint32_t authority = 0;
  SourceCapabilities capabilities{};
  bool advisory_only = false;

  IncarnationId incarnation{};
  EpochId epoch{};
  RevisionId high_revision{};
  SequenceNo high_sequence{};

  Timestamp first_observed_at{};
  Timestamp last_observed_at{};
  Timestamp last_received_at{};
  Timestamp last_progress_at{};

  Coverage coverage{};
  std::vector<std::uint64_t> hop_capacity_units;
  DirectionalAccounting forward{};
  DirectionalAccounting reverse{};
  DirectionalAccounting aggregate{};
  Consumption forward_totals{};
  Consumption reverse_totals{};
  Consumption aggregate_totals{};
  bool observed_progress = false;

  ObservationKind last_kind = ObservationKind::Unknown;
  Direction last_direction = Direction::Unspecified;
  Evidence last_evidence = Evidence::Unknown;
  std::optional<FlowState> declared_terminal{};
  StateCause declared_cause = StateCause::None;

  std::uint32_t accepted_observations = 0;
  std::uint32_t refused_observations = 0;
  std::uint32_t dropped_observations = 0;
  std::uint32_t duplicates_suppressed = 0;
  std::uint32_t content_conflicts = 0;
  std::uint32_t historical_incarnations = 0;
  std::uint32_t after_terminal_observations = 0;
};

struct FLOWOBS_PUBLIC PersistedGeneration final {
  GenerationId generation{};
  bool is_current = false;

  Timestamp first_observed_at{};
  Timestamp last_observed_at{};
  Timestamp last_received_at{};
  Timestamp last_progress_at{};
  Timestamp closed_at{};

  FlowState state = FlowState::Unknown;
  StateCause cause = StateCause::None;
  Coverage coverage{};

  std::optional<PathId> path;
  std::vector<LinkId> links;
  std::vector<QueueId> queues;
  EndpointId endpoint_a{};
  EndpointId endpoint_b{};
  std::uint32_t declared_path_generation = 0;

  Consumption forward{};
  Consumption reverse{};
  Consumption aggregate{};
  Consumption total{};

  bool unresolved_conflict = false;
  std::string conflict_detail;

  std::vector<PersistedSource> sources;
};

struct FLOWOBS_PUBLIC PersistedFlow final {
  FlowId id{};
  std::string canonical_key;
  std::uint64_t key_collisions = 0;

  GenerationId current_generation{};
  RevisionId revision{};

  FlowState state = FlowState::Unknown;
  StateCause cause = StateCause::None;
  Coverage coverage{};

  Timestamp created_at{};
  Timestamp state_changed_at{};
  Timestamp first_observed_at{};
  Timestamp last_observed_at{};
  Timestamp last_received_at{};
  Timestamp last_progress_at{};

  EndpointId endpoint_a{};
  EndpointId endpoint_b{};

  std::vector<PersistedGeneration> generations;
  std::vector<LifecycleEvent> history;
  std::uint64_t history_dropped = 0;
  std::vector<Anomaly> anomalies;
  std::uint64_t anomalies_dropped = 0;
};

// Whole-runtime durable projection. A snapshot is written as one contiguous
// record group bracketed by SnapshotBegin/SnapshotEnd so that a crash in the
// middle of a snapshot is detectable.
struct FLOWOBS_PUBLIC PersistedState final {
  std::uint16_t format_version = kJournalFormatVersion;
  std::uint32_t identity_scheme = kIdentitySchemeVersion;
  std::uint32_t schema_version = kObservationSchemaVersion;
  // Digest of every policy field in force when the snapshot was written. A
  // snapshot folded under different policy is refused rather than
  // reinterpreted.
  std::uint64_t policy_digest = 0;
  RuntimeEpoch runtime_epoch{};
  Timestamp created_at{};
  Timestamp snapshot_at{};
  std::uint64_t snapshot_sequence = 0;

  std::vector<SourceDescriptor> sources;
  std::vector<PersistedFlow> flows;
};

// ---------------------------------------------------------------------------
// Journal.
// ---------------------------------------------------------------------------

namespace journal {

enum class RecordKind : std::uint16_t {
  SnapshotBegin = 1,
  SourceDescriptor = 2,
  Flow = 3,
  SnapshotEnd = 4,
  LimitsRecord = 5,
  EngineCounters = 6,
  Tombstone = 7,
  // One accepted observation. The journal is an observation log with periodic
  // snapshots, so recovery replays evidence instead of trusting a serialized
  // view, and the recovered state is produced by exactly the same code path as
  // the live state.
  ObservationRecord = 8,
};

FLOWOBS_PUBLIC std::string_view to_string(RecordKind kind) noexcept;
FLOWOBS_PUBLIC bool is_known_record_kind(std::uint16_t raw) noexcept;

struct FLOWOBS_PUBLIC Record final {
  RecordKind kind = RecordKind::SnapshotBegin;
  std::vector<std::byte> payload;
};

}  // namespace journal

struct FLOWOBS_PUBLIC JournalHeader final {
  std::uint16_t format_version = kJournalFormatVersion;
  std::uint16_t identity_scheme = static_cast<std::uint16_t>(kIdentitySchemeVersion);
  std::uint32_t flags = 0;
  RuntimeEpoch runtime_epoch{};
  Timestamp created_at{};
  std::uint64_t record_count = 0;
};

struct FLOWOBS_PUBLIC JournalOpenOptions final {
  Limits limits{};
  RecoveryPolicy recovery{};
  bool read_only = false;
  bool create_if_missing = true;
  bool truncate_existing = false;
};

struct FLOWOBS_PUBLIC JournalStats final {
  std::uint64_t bytes_written = 0;
  std::uint64_t bytes_read = 0;
  std::uint64_t records_written = 0;
  std::uint64_t records_read = 0;
  std::uint64_t records_rejected = 0;
  std::uint64_t torn_tail_bytes_dropped = 0;
  std::uint64_t compactions = 0;
  std::size_t file_size = 0;
};

// Append-only record log. Not internally synchronised: the engine serialises
// access under its own lock. This is deliberate and is part of the lock-order
// audit in docs/concurrency.md.
class FLOWOBS_PUBLIC Journal final {
 public:
  Journal() = default;
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;
  ~Journal();

  // Opens (creating when allowed). Performs header validation and, per the
  // recovery policy, may truncate a torn tail. Never repairs a corrupt
  // non-tail record.
  [[nodiscard]] static Result<std::unique_ptr<Journal>> open(
      const std::filesystem::path& path, const JournalOpenOptions& options);

  [[nodiscard]] Status append(const journal::Record& record);
  [[nodiscard]] Status append(const std::vector<journal::Record>& records);
  [[nodiscard]] Status flush();
  [[nodiscard]] Status close();

  // Reads every record, stopping at the first torn tail. Bounded by
  // options.limits.max_journal_bytes and max_result_set * 1024 records.
  [[nodiscard]] Result<std::vector<journal::Record>> read_all() const;

  // Verifies header and every record CRC without returning payloads.
  [[nodiscard]] Status verify() const;

  [[nodiscard]] const JournalHeader& header() const noexcept { return header_; }
  // Updates the recorded runtime epoch and creation instant. Both are part of
  // the CRC protected header and are written on the next flush.
  void set_runtime_epoch(RuntimeEpoch epoch) noexcept {
    header_.runtime_epoch = epoch;
    dirty_ = true;
  }
  void set_created_at(Timestamp created_at) noexcept {
    header_.created_at = created_at;
    dirty_ = true;
  }
  [[nodiscard]] const JournalStats& stats() const noexcept { return stats_; }
  [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  // Rewrites the journal atomically: writes `records` to a sibling temp file,
  // flushes it, then replaces the original. The header is regenerated.
  [[nodiscard]] Status rewrite(const std::vector<journal::Record>& records,
                               RuntimeEpoch new_epoch, Timestamp created_at);

 private:
  [[nodiscard]] Status write_header();

  std::filesystem::path path_;
  void* file_ = nullptr;  // std::FILE*
  JournalHeader header_{};
  JournalStats stats_{};
  Limits limits_{};
  RecoveryPolicy recovery_{};
  bool read_only_ = false;
  bool dirty_ = false;
};

// ---------------------------------------------------------------------------
// Encoding of the durable projection.
// ---------------------------------------------------------------------------

// Encodes `state` into a record group. Bounded: refuses when the encoded size
// would exceed `limits.max_journal_bytes / 2` or any per-field bound.
FLOWOBS_PUBLIC Status encode_state(const PersistedState& state, const Limits& limits,
                                   std::vector<journal::Record>& out);

// Decodes a record group. Requires a well-formed SnapshotBegin ... SnapshotEnd
// bracket. Returns IntegrityFailure when the group is incomplete and
// FormatMismatch when a payload does not decode.
FLOWOBS_PUBLIC Result<PersistedState> decode_state(const std::vector<journal::Record>& records,
                                                   const Limits& limits,
                                                   const RecoveryPolicy& recovery);

// Replays a record sequence: snapshot groups replace the accumulated state,
// bare records (source descriptors, observations) are applied in order. This is
// the journal recovery entry point.
struct FLOWOBS_PUBLIC ReplayResult final {
  PersistedState state;
  std::vector<Observation> observations;
  std::uint64_t snapshots = 0;
  std::uint64_t observation_records = 0;
  std::uint64_t source_records = 0;
  bool saw_snapshot = false;
};

FLOWOBS_PUBLIC Result<ReplayResult> replay_records(const std::vector<journal::Record>& records,
                                                   const Limits& limits,
                                                   const RecoveryPolicy& recovery);

// Deterministic digest of a durable projection, used to prove that a
// save/load round trip is lossless.
FLOWOBS_PUBLIC std::uint64_t digest_state(const PersistedState& state) noexcept;

}  // namespace flowobs

#endif  // FLOWOBS_PERSISTENCE_HPP
