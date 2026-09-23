// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

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

std::filesystem::path scratch(const std::string& name, bool remove) {
  std::filesystem::path directory = std::filesystem::temp_directory_path() / "flowobs-tests";
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  std::filesystem::path file = directory / name;
  if (remove) {
    std::filesystem::remove(file, ec);
  }
  return file;
}

std::string read_bytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_bytes(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

const char* kScript = R"FO1(
SRC v=1 key="fabric-a" auth=20 caps=completion,reset,counters
AT  v=1 now=1700000000000000000
OBS v=1 src="fabric-a" inc=1 epoch=1 rev=1 seq=1 flow="flow-1" gen=1 obs=1700000000000000000 kind=open dir=forward ev=complete bytes=0 pkts=0
OBS v=1 src="fabric-a" inc=1 epoch=1 rev=2 seq=2 flow="flow-1" gen=1 obs=1700000001000000000 kind=sample dir=forward ev=complete bytes=1024 pkts=2
OBS v=1 src="fabric-a" inc=1 epoch=1 rev=3 seq=3 flow="flow-1" gen=1 obs=1700000002000000000 kind=sample dir=forward ev=complete bytes=4096 pkts=8
)FO1";

}  // namespace

FOTEST("adversarial.persistence", "flipping_one_payload_bit_is_detected") {
  const std::filesystem::path path = scratch("bitflip.foj", true);
  {
    ManualClock clock;
    clock.set(fotest::recent_now());
    EngineConfig config;
    config.clock = &clock;
    config.journal_path = path;
    config.limits.worker_threads = 0;
    Engine engine(config);
    FO_REQUIRE(engine.open().ok());
    FO_REQUIRE(apply_script_text(engine, kScript, 256, &clock).ok());
    FO_REQUIRE(engine.close().ok());
  }
  std::string bytes = read_bytes(path);
  FO_REQUIRE(bytes.size() > 120u);
  // Corrupt a byte in the middle of the observation log, past the header.
  bytes[bytes.size() / 2] = static_cast<char>(bytes[bytes.size() / 2] ^ 0x20);
  write_bytes(path, bytes);

  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.journal_path = path;
  config.limits.worker_threads = 0;
  Engine engine(config);
  const Status opened = engine.open();
  FO_REQUIRE(!opened.ok());
  FO_CHECK_EQ(static_cast<int>(opened.code()), static_cast<int>(ErrorCode::IntegrityFailure));
}

FOTEST("adversarial.persistence", "a_torn_tail_drops_only_the_tail") {
  const std::filesystem::path path = scratch("torn2.foj", true);
  {
    ManualClock clock;
    clock.set(fotest::recent_now());
    EngineConfig config;
    config.clock = &clock;
    config.journal_path = path;
    config.limits.worker_threads = 0;
    Engine engine(config);
    FO_REQUIRE(engine.open().ok());
    FO_REQUIRE(apply_script_text(engine, kScript, 256, &clock).ok());
    FO_REQUIRE(engine.close().ok());
  }
  std::string bytes = read_bytes(path);
  FO_REQUIRE(bytes.size() > 80u);
  bytes.resize(bytes.size() - 1u);
  write_bytes(path, bytes);

  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.journal_path = path;
  config.limits.worker_threads = 0;
  config.recovery.torn_tail = RecoveryPolicy::TornTail::TruncateTornTail;
  Engine engine(config);
  FO_REQUIRE(engine.open().ok());
  const Result<FlowSnapshot> snapshot = engine.snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  // The surviving prefix is intact; the lost tail is not invented.
  FO_CHECK(snapshot.value().total.bytes <= 4096ull);
  FO_CHECK(!snapshot.value().is_live());
  FO_REQUIRE(engine.close().ok());
}

FOTEST("adversarial.persistence", "reject_policy_refuses_a_torn_tail") {
  const std::filesystem::path path = scratch("torn3.foj", true);
  {
    ManualClock clock;
    clock.set(fotest::recent_now());
    EngineConfig config;
    config.clock = &clock;
    config.journal_path = path;
    config.limits.worker_threads = 0;
    Engine engine(config);
    FO_REQUIRE(engine.open().ok());
    FO_REQUIRE(apply_script_text(engine, kScript, 256, &clock).ok());
    FO_REQUIRE(engine.close().ok());
  }
  std::string bytes = read_bytes(path);
  bytes.resize(bytes.size() - 3u);
  write_bytes(path, bytes);
  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.journal_path = path;
  config.limits.worker_threads = 0;
  config.recovery.torn_tail = RecoveryPolicy::TornTail::Reject;
  Engine engine(config);
  const Status opened = engine.open();
  FO_REQUIRE(!opened.ok());
  FO_CHECK_EQ(static_cast<int>(opened.code()), static_cast<int>(ErrorCode::TruncatedRecord));
}

FOTEST("adversarial.persistence", "unknown_record_kind_fails_closed_on_open") {
  const std::filesystem::path path = scratch("unknown2.foj", true);
  {
    ManualClock clock;
    clock.set(fotest::recent_now());
    EngineConfig config;
    config.clock = &clock;
    config.journal_path = path;
    config.limits.worker_threads = 0;
    Engine engine(config);
    FO_REQUIRE(engine.open().ok());
    FO_REQUIRE(apply_script_text(engine, kScript, 256, &clock).ok());
    FO_REQUIRE(engine.close().ok());
  }
  std::string bytes = read_bytes(path);
  FO_REQUIRE(bytes.size() > 64u);
  bytes[64] = static_cast<char>(123);  // a kind no build knows
  write_bytes(path, bytes);
  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.journal_path = path;
  config.limits.worker_threads = 0;
  Engine engine(config);
  const Status opened = engine.open();
  FO_REQUIRE(!opened.ok());
  FO_CHECK_EQ(static_cast<int>(opened.code()), static_cast<int>(ErrorCode::FormatMismatch));
}

FOTEST("adversarial.persistence", "journal_cannot_grow_past_its_bound") {
  const std::filesystem::path path = scratch("growth.foj", true);
  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.journal_path = path;
  config.limits.worker_threads = 0;
  config.limits.max_journal_bytes = 8192;
  Engine engine(config);
  FO_REQUIRE(engine.open().ok());
  for (int i = 0; i < 400; ++i) {
    ObservationSpec spec;
    spec.revision = static_cast<std::uint64_t>(i + 1);
    spec.sequence = spec.revision;
    spec.bytes = static_cast<std::uint64_t>(i) * 100;
    spec.observed_at = static_cast<std::uint64_t>(base_time().nanos() +
                                                  static_cast<std::int64_t>(i + 1) * 1000);
    spec.received_at = spec.observed_at;
    const Result<IngestOutcome> outcome = engine.submit(make_observation(spec));
    FO_REQUIRE(outcome.ok());
    FO_CHECK(outcome.value().applied() || outcome.value().idempotent());
  }
  const EngineStats stats = engine.stats();
  FO_CHECK(stats.journal_file_bytes <= config.limits.max_journal_bytes);
  FO_CHECK(stats.journal_compactions > 0ull);
  FO_CHECK_EQ(stats.journal_failures, 0ull);
  FO_CHECK(engine.verify_journal().ok());
  const Result<FlowSnapshot> snapshot = engine.snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(snapshot.value().total.bytes, 39900ull);
  FO_REQUIRE(engine.close().ok());
}

FOTEST("adversarial.persistence", "duplicate_observations_do_not_inflate_the_journal_view") {
  const std::filesystem::path path = scratch("dupes.foj", true);
  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.journal_path = path;
  config.limits.worker_threads = 0;
  Engine engine(config);
  FO_REQUIRE(engine.open().ok());
  ObservationSpec spec;
  spec.bytes = 1000;
  FO_REQUIRE(engine.submit(make_observation(spec)).ok());
  spec.revision = 2;
  spec.sequence = 2;
  spec.bytes = 3000;
  FO_REQUIRE(engine.submit(make_observation(spec)).ok());
  for (int i = 0; i < 10; ++i) {
    FO_REQUIRE(engine.submit(make_observation(spec)).ok());
  }
  FO_REQUIRE(engine.close().ok());

  Engine reopened(config);
  FO_REQUIRE(reopened.open().ok());
  const Result<FlowSnapshot> snapshot = reopened.snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(snapshot.value().total.bytes, 2000ull);
  FO_REQUIRE(reopened.close().ok());
}

FOTEST("adversarial.persistence", "policy_change_invalidates_a_folded_snapshot") {
  const std::filesystem::path path = scratch("policy.foj", true);
  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.journal_path = path;
  config.limits.worker_threads = 0;
  {
    Engine engine(config);
    FO_REQUIRE(engine.open().ok());
    FO_REQUIRE(apply_script_text(engine, kScript, 256, &clock).ok());
    FO_REQUIRE(engine.compact().ok());
    FO_REQUIRE(engine.close().ok());
  }
  config.counters.treat_regression_as_reset = false;  // different fold policy
  Engine engine(config);
  const Status opened = engine.open();
  FO_REQUIRE(!opened.ok());
  FO_CHECK_EQ(static_cast<int>(opened.code()), static_cast<int>(ErrorCode::VersionMismatch));
}

FOTEST("adversarial.persistence", "restored_evidence_never_becomes_fresh_by_itself") {
  const std::filesystem::path path = scratch("restored.foj", true);
  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.journal_path = path;
  config.limits.worker_threads = 0;
  {
    Engine engine(config);
    FO_REQUIRE(engine.open().ok());
    FO_REQUIRE(apply_script_text(engine, kScript, 256, &clock).ok());
    const Result<FlowSnapshot> before = engine.snapshot(flow_id_from_key("flow-1"));
    FO_REQUIRE(before.ok());
    FO_REQUIRE(before.value().is_live());
    FO_REQUIRE(engine.close().ok());
  }
  // Reopen immediately: the wall clock has barely moved, yet the restored flow
  // must not be reported live.
  Engine reopened(config);
  FO_REQUIRE(reopened.open().ok());
  const Result<FlowSnapshot> after = reopened.snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(after.ok());
  FO_CHECK(after.value().restored_from_journal);
  FO_CHECK(!after.value().is_live());
  FO_CHECK_EQ(static_cast<int>(after.value().freshness), static_cast<int>(Freshness::Unknown));
  const Result<QueryResult> live_only = reopened.query([] {
    FlowQuery query;
    query.only_live = true;
    return query;
  }());
  FO_REQUIRE(live_only.ok());
  FO_CHECK_EQ(live_only.value().returned, 0u);
  FO_REQUIRE(reopened.close().ok());
}
FOTEST("adversarial.persistence", "two_runtimes_cannot_share_one_journal") {
  const std::filesystem::path path = scratch("exclusive.foj", true);
  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.journal_path = path;
  config.limits.worker_threads = 0;

  Engine first(config);
  FO_REQUIRE(first.open().ok());
  ObservationSpec spec;
  spec.bytes = 100;
  FO_REQUIRE(first.submit(make_observation(spec)).ok());

  // A second runtime appending to the same journal would interleave records and
  // corrupt it, so it is refused rather than trusted.
  Engine second(config);
  const Status conflicted = second.open();
  FO_REQUIRE(!conflicted.ok());
  FO_CHECK_EQ(static_cast<int>(conflicted.code()), static_cast<int>(ErrorCode::AlreadyOpen));

  // A read-only observer is still allowed while the writer holds the journal.
  {
    JournalOpenOptions options;
    options.read_only = true;
    options.create_if_missing = false;
    Result<std::unique_ptr<Journal>> reader = Journal::open(path, options);
    FO_REQUIRE(reader.ok());
    FO_CHECK(reader.value()->verify().ok());
    FO_REQUIRE(reader.value()->close().ok());
  }

  FO_REQUIRE(first.close().ok());
  // Once the writer has closed, the journal is available again.
  Engine third(config);
  FO_REQUIRE(third.open().ok());
  const Result<FlowSnapshot> snapshot = third.snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(snapshot.value().total.bytes, 0ull);
  FO_REQUIRE(third.close().ok());
}
