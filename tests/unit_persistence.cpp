// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
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

namespace {

std::filesystem::path scratch(const std::string& name) {
  std::filesystem::path directory = std::filesystem::temp_directory_path() / "flowobs-tests";
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  std::filesystem::path file = directory / name;
  std::filesystem::remove(file, ec);
  return file;
}

void write_bytes(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read_bytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

FOTEST("journal", "create_append_reopen") {
  const std::filesystem::path path = scratch("basic.foj");
  JournalOpenOptions options;
  {
    Result<std::unique_ptr<Journal>> opened = Journal::open(path, options);
    FO_REQUIRE(opened.ok());
    std::unique_ptr<Journal> journal = std::move(opened.value());
    journal::Record record;
    record.kind = journal::RecordKind::Tombstone;
    record.payload = {std::byte{1}, std::byte{2}, std::byte{3}};
    FO_CHECK(journal->append(record).ok());
    FO_CHECK(journal->append(record).ok());
    FO_CHECK(journal->flush().ok());
    FO_CHECK_EQ(journal->header().record_count, 2ull);
    FO_CHECK(journal->close().ok());
  }
  {
    Result<std::unique_ptr<Journal>> opened = Journal::open(path, options);
    FO_REQUIRE(opened.ok());
    std::unique_ptr<Journal> journal = std::move(opened.value());
    Result<std::vector<journal::Record>> records = journal->read_all();
    FO_REQUIRE(records.ok());
    FO_CHECK_EQ(records.value().size(), 2u);
    FO_CHECK_EQ(records.value()[0].payload.size(), 3u);
    FO_CHECK(journal->close().ok());
  }
}

FOTEST("journal", "truncated_tail_is_dropped_only_when_allowed") {
  const std::filesystem::path path = scratch("torn.foj");
  JournalOpenOptions options;
  {
    Result<std::unique_ptr<Journal>> opened = Journal::open(path, options);
    FO_REQUIRE(opened.ok());
    journal::Record record;
    record.kind = journal::RecordKind::Tombstone;
    record.payload = {std::byte{7}, std::byte{7}, std::byte{7}, std::byte{7}};
    FO_CHECK(opened.value()->append(record).ok());
    FO_CHECK(opened.value()->close().ok());
  }
  std::string bytes = read_bytes(path);
  FO_REQUIRE(bytes.size() > 4u);
  bytes.resize(bytes.size() - 2u);  // tear the last record
  write_bytes(path, bytes);

  JournalOpenOptions reject;
  reject.recovery.torn_tail = RecoveryPolicy::TornTail::Reject;
  Result<std::unique_ptr<Journal>> rejected = Journal::open(path, reject);
  FO_REQUIRE(rejected.ok());
  FO_CHECK(!rejected.value()->read_all().ok());
  FO_CHECK(rejected.value()->close().ok());

  JournalOpenOptions truncate;
  truncate.recovery.torn_tail = RecoveryPolicy::TornTail::TruncateTornTail;
  Result<std::unique_ptr<Journal>> allowed = Journal::open(path, truncate);
  FO_REQUIRE(allowed.ok());
  Result<std::vector<journal::Record>> records = allowed.value()->read_all();
  FO_REQUIRE(records.ok());
  FO_CHECK_EQ(records.value().size(), 0u);
  FO_CHECK(allowed.value()->close().ok());
}

FOTEST("journal", "corrupt_payload_is_an_integrity_failure") {
  const std::filesystem::path path = scratch("corrupt.foj");
  JournalOpenOptions options;
  {
    Result<std::unique_ptr<Journal>> opened = Journal::open(path, options);
    FO_REQUIRE(opened.ok());
    journal::Record record;
    record.kind = journal::RecordKind::Tombstone;
    record.payload = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
    FO_CHECK(opened.value()->append(record).ok());
    FO_CHECK(opened.value()->close().ok());
  }
  std::string bytes = read_bytes(path);
  FO_REQUIRE(bytes.size() > 76u);
  // 64 byte file header + 12 byte record header + 1: the first payload byte.
  bytes[77] = static_cast<char>(bytes[77] ^ 0x40);
  write_bytes(path, bytes);

  Result<std::unique_ptr<Journal>> opened = Journal::open(path, options);
  FO_REQUIRE(opened.ok());
  Result<std::vector<journal::Record>> records = opened.value()->read_all();
  FO_REQUIRE(!records.ok());
  const ErrorCode read_code = records.status().code();
  FO_CHECK_EQ(static_cast<int>(read_code), static_cast<int>(ErrorCode::IntegrityFailure));
  FO_CHECK(opened.value()->close().ok());
}

FOTEST("journal", "corrupt_header_is_refused") {
  const std::filesystem::path path = scratch("header.foj");
  JournalOpenOptions options;
  {
    Result<std::unique_ptr<Journal>> opened = Journal::open(path, options);
    FO_REQUIRE(opened.ok());
    FO_CHECK(opened.value()->close().ok());
  }
  std::string bytes = read_bytes(path);
  FO_REQUIRE(bytes.size() >= 64u);
  bytes[20] = static_cast<char>(bytes[20] ^ 0x01);
  write_bytes(path, bytes);
  Result<std::unique_ptr<Journal>> opened = Journal::open(path, options);
  FO_REQUIRE(!opened.ok());
  const ErrorCode open_code = opened.status().code();
  FO_CHECK_EQ(static_cast<int>(open_code), static_cast<int>(ErrorCode::IntegrityFailure));
}

FOTEST("journal", "bad_magic_is_a_format_mismatch") {
  const std::filesystem::path path = scratch("magic.foj");
  write_bytes(path, std::string(64, 'x'));
  JournalOpenOptions options;
  options.create_if_missing = false;
  Result<std::unique_ptr<Journal>> opened = Journal::open(path, options);
  FO_REQUIRE(!opened.ok());
  const ErrorCode format_code = opened.status().code();
  FO_CHECK_EQ(static_cast<int>(format_code), static_cast<int>(ErrorCode::FormatMismatch));
}

FOTEST("codec", "state_round_trip_is_lossless") {
  PersistedState state;
  state.runtime_epoch = RuntimeEpoch::from_value(7);
  state.created_at = base_time();
  state.snapshot_at = base_time() + Duration::from_seconds(1);
  state.snapshot_sequence = 3;
  state.policy_digest = 0x123456789ABCDEFull;

  SourceDescriptor descriptor = make_source("src-a", 4);
  state.sources.push_back(descriptor);

  PersistedFlow flow;
  flow.id = flow_id_from_key("flow-1");
  flow.canonical_key = "flow-1";
  flow.current_generation = GenerationId::from_value(2);
  flow.revision = RevisionId::from_value(9);
  flow.state = FlowState::Active;
  flow.cause = StateCause::ProgressObserved;
  flow.coverage.forward_complete = true;
  flow.created_at = base_time();
  flow.last_observed_at = base_time() + Duration::from_seconds(3);

  PersistedGeneration generation;
  generation.generation = GenerationId::from_value(2);
  generation.is_current = true;
  generation.state = FlowState::Active;
  generation.cause = StateCause::ProgressObserved;
  generation.coverage.forward_complete = true;
  generation.last_observed_at = base_time() + Duration::from_seconds(3);
  generation.forward.bytes = 4096;
  generation.forward.bytes = 4096;
  generation.total.bytes = 4096;
  generation.path = path_id_from_key("p0");
  generation.links = {link_id_from_key("l0"), link_id_from_key("l1")};
  generation.queues = {queue_id_from_key("q0")};
  generation.endpoint_a = endpoint_id_from_key("ep-a");
  generation.endpoint_b = endpoint_id_from_key("ep-b");

  PersistedSource source;
  source.source = descriptor.id;
  source.canonical_key = descriptor.canonical_key;
  source.authority = descriptor.authority;
  source.capabilities = descriptor.capabilities;
  source.incarnation = IncarnationId::from_value(3);
  source.epoch = EpochId::from_value(2);
  source.high_revision = RevisionId::from_value(11);
  source.high_sequence = SequenceNo::from_value(12);
  source.first_observed_at = base_time();
  source.last_observed_at = base_time() + Duration::from_seconds(3);
  source.last_received_at = source.last_observed_at;
  source.last_progress_at = source.last_observed_at;
  source.coverage.forward_complete = true;
  source.hop_capacity_units = {100, 200};
  source.forward.bytes.restore(CounterAccumulator::Snapshot{0, 4096, 4096, 1, 2, true});
  source.last_kind = ObservationKind::Sample;
  source.last_direction = Direction::Forward;
  source.last_evidence = Evidence::Complete;
  source.declared_terminal = FlowState::Completed;
  source.declared_cause = StateCause::CompletionObserved;
  source.accepted_observations = 12;
  source.observed_progress = true;
  generation.sources.push_back(source);
  flow.generations.push_back(generation);

  LifecycleEvent event;
  event.at = base_time();
  event.generation = FlowGeneration(flow.id, GenerationId::from_value(2));
  event.from = FlowState::Observed;
  event.to = FlowState::Active;
  event.cause = StateCause::ProgressObserved;
  flow.history.push_back(event);
  flow.history_dropped = 2;

  Anomaly anomaly;
  anomaly.kind = AnomalyKind::CounterRegression;
  anomaly.at = base_time();
  anomaly.source = descriptor.id;
  anomaly.generation = FlowGeneration(flow.id, GenerationId::from_value(2));
  anomaly.detail = "counter went backwards";
  flow.anomalies.push_back(anomaly);
  flow.anomalies_dropped = 1;
  state.flows.push_back(flow);

  Limits limits;
  std::vector<journal::Record> records;
  FO_REQUIRE(encode_state(state, limits, records).ok());
  FO_CHECK(records.front().kind == journal::RecordKind::SnapshotBegin);
  FO_CHECK(records.back().kind == journal::RecordKind::SnapshotEnd);

  Result<PersistedState> decoded = decode_state(records, limits, RecoveryPolicy{});
  FO_REQUIRE(decoded.ok());
  FO_CHECK_EQ(digest_state(decoded.value()), digest_state(state));
  const PersistedFlow& restored = decoded.value().flows.at(0);
  FO_CHECK_EQ(restored.canonical_key, std::string("flow-1"));
  FO_CHECK_EQ(restored.generations.size(), 1u);
  FO_CHECK_EQ(restored.generations[0].sources.size(), 1u);
  FO_CHECK_EQ(restored.generations[0].sources[0].forward.bytes.accumulated(), 4096ull);
  FO_CHECK_EQ(restored.generations[0].sources[0].forward.bytes.regressions(), 2ull);
  FO_CHECK(restored.generations[0].sources[0].forward.bytes.saturated());
  FO_CHECK_EQ(restored.generations[0].sources[0].hop_capacity_units.size(), 2u);
  FO_CHECK_EQ(restored.history.size(), 1u);
  FO_CHECK_EQ(restored.anomalies.size(), 1u);
}

FOTEST("codec", "record_group_order_and_count_are_verified") {
  PersistedState state;
  state.runtime_epoch = RuntimeEpoch::from_value(1);
  PersistedFlow flow;
  flow.id = flow_id_from_key("flow-1");
  flow.canonical_key = "flow-1";
  state.flows.push_back(flow);
  Limits limits;
  std::vector<journal::Record> records;
  FO_REQUIRE(encode_state(state, limits, records).ok());

  // Reordering two records keeps every CRC valid but must be caught by the
  // group digest.
  std::vector<journal::Record> reordered = records;
  std::swap(reordered[0], reordered[1]);
  Result<PersistedState> decoded = decode_state(reordered, limits, RecoveryPolicy{});
  FO_REQUIRE(!decoded.ok());

  // Dropping the end record is an incomplete group.
  std::vector<journal::Record> truncated = records;
  truncated.pop_back();
  FO_CHECK(!decode_state(truncated, limits, RecoveryPolicy{}).ok());

  // Lying about the record count is caught too.
  std::vector<journal::Record> lied = records;
  lied.back().payload[12] = std::byte{9};
  FO_CHECK(!decode_state(lied, limits, RecoveryPolicy{}).ok());
}

FOTEST("codec", "unknown_record_kind_fails_closed") {
  const std::filesystem::path path = scratch("unknown.foj");
  JournalOpenOptions options;
  {
    Result<std::unique_ptr<Journal>> opened = Journal::open(path, options);
    FO_REQUIRE(opened.ok());
    journal::Record record;
    record.kind = journal::RecordKind::Tombstone;
    record.payload = {std::byte{1}};
    FO_CHECK(opened.value()->append(record).ok());
    FO_CHECK(opened.value()->close().ok());
  }
  std::string bytes = read_bytes(path);
  FO_REQUIRE(bytes.size() > 64u);
  bytes[64] = static_cast<char>(200);  // unknown record kind
  write_bytes(path, bytes);
  Result<std::unique_ptr<Journal>> opened = Journal::open(path, options);
  FO_REQUIRE(opened.ok());
  Result<std::vector<journal::Record>> records = opened.value()->read_all();
  FO_REQUIRE(!records.ok());
  const ErrorCode kind_code = records.status().code();
  FO_CHECK_EQ(static_cast<int>(kind_code), static_cast<int>(ErrorCode::FormatMismatch));
  FO_CHECK(opened.value()->close().ok());
}

FOTEST("codec", "rewrite_is_atomic_and_preserves_order") {
  const std::filesystem::path path = scratch("rewrite.foj");
  JournalOpenOptions options;
  Result<std::unique_ptr<Journal>> opened = Journal::open(path, options);
  FO_REQUIRE(opened.ok());
  std::unique_ptr<Journal> journal = std::move(opened.value());
  std::vector<journal::Record> records;
  for (int i = 0; i < 5; ++i) {
    journal::Record record;
    record.kind = journal::RecordKind::Tombstone;
    record.payload = {static_cast<std::byte>(i)};
    records.push_back(record);
  }
  FO_REQUIRE(journal->rewrite(records, RuntimeEpoch::from_value(4), base_time()).ok());
  FO_CHECK_EQ(journal->header().runtime_epoch.value(), 4ull);
  Result<std::vector<journal::Record>> read_back = journal->read_all();
  FO_REQUIRE(read_back.ok());
  FO_CHECK_EQ(read_back.value().size(), 5u);
  for (int i = 0; i < 5; ++i) {
    FO_CHECK_EQ(std::to_integer<int>(read_back.value()[static_cast<std::size_t>(i)].payload[0]), i);
  }
  FO_CHECK(journal->close().ok());
}
