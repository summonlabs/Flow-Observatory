// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Restart and recovery. The defining requirement is that a restart must not
// resurrect active-flow liveness and must not silently promote persisted
// evidence to fresh.

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

struct Session final {
  ManualClock clock;
  EngineConfig config;
  std::unique_ptr<Engine> engine;

  explicit Session(const std::filesystem::path& path) {
    config.clock = &clock;
    config.journal_path = path;
    config.limits.worker_threads = 0;
    config.expiry.idle_after = Duration::from_seconds(10);
    config.expiry.expire_after = Duration::from_seconds(60);
    config.freshness.fresh_window = Duration::from_seconds(10);
    config.freshness.expire_window = Duration::from_seconds(60);
    clock.set(fotest::recent_now());
    engine = std::make_unique<Engine>(config);
  }
};

}  // namespace

FOTEST("restart", "accounting_survives_liveness_does_not") {
  const std::filesystem::path path = scratch("liveness.foj");
  {
    Session session(path);
    FO_REQUIRE(session.engine->open().ok());
    FO_REQUIRE(session.engine->register_source(make_source("src-a", 10)).ok());
    ObservationSpec spec;
    spec.bytes = 0;
    spec.kind = ObservationKind::Open;
    FO_REQUIRE(session.engine->submit(make_observation(spec)).ok());
    spec.revision = 2;
    spec.sequence = 2;
    spec.bytes = 5000;
    FO_REQUIRE(session.engine->submit(make_observation(spec)).ok());
    const Result<FlowSnapshot> snapshot = session.engine->snapshot(flow_id_from_key("flow-1"));
    FO_REQUIRE(snapshot.ok());
    FO_REQUIRE(snapshot.value().is_live());
    FO_CHECK_EQ(snapshot.value().total.bytes, 5000ull);
    FO_REQUIRE(session.engine->close().ok());
  }

  Session restarted(path);
  FO_REQUIRE(restarted.engine->open().ok());
  FO_CHECK_EQ(restarted.engine->runtime_epoch().value(), 2ull);
  const Result<FlowSnapshot> snapshot = restarted.engine->snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(snapshot.value().total.bytes, 5000ull);
  FO_CHECK(snapshot.value().restored_from_journal);
  FO_CHECK(!snapshot.value().is_live());
  FO_CHECK(!asserts_completion(snapshot.value().state));
  FO_CHECK_EQ(snapshot.value().current_epoch.value(), 2ull);
  FO_CHECK_EQ(snapshot.value().restored_epoch.value(), 1ull);
  FO_REQUIRE(restarted.engine->close().ok());
}

FOTEST("restart", "fresh_evidence_after_restart_reconfirms_the_flow") {
  const std::filesystem::path path = scratch("reconfirm.foj");
  {
    Session session(path);
    FO_REQUIRE(session.engine->open().ok());
    ObservationSpec spec;
    spec.bytes = 100;
    FO_REQUIRE(session.engine->submit(make_observation(spec)).ok());
    spec.revision = 2;
    spec.sequence = 2;
    spec.bytes = 700;
    FO_REQUIRE(session.engine->submit(make_observation(spec)).ok());
    FO_REQUIRE(session.engine->close().ok());
  }
  Session restarted(path);
  FO_REQUIRE(restarted.engine->open().ok());
  ObservationSpec spec;
  spec.bytes = 900;
  spec.revision = 3;
  spec.sequence = 3;
  spec.observed_at = static_cast<std::uint64_t>(restarted.clock.now().nanos());
  spec.received_at = spec.observed_at;
  const Result<IngestOutcome> accepted = restarted.engine->submit(make_observation(spec));
  FO_REQUIRE(accepted.ok());
  const Result<FlowSnapshot> snapshot = restarted.engine->snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  FO_CHECK(snapshot.value().is_live());
  FO_CHECK(!snapshot.value().restored_from_journal);
  // A journal replay reconstructs every accepted observation, so the restart
  // itself introduces no gap and the accounting stays exact.
  FO_CHECK_EQ(snapshot.value().total.unknown_intervals, 0ull);
  FO_CHECK(snapshot.value().total.is_exact());
  FO_CHECK_EQ(snapshot.value().total.bytes, 800ull);
  FO_REQUIRE(restarted.engine->close().ok());
}

FOTEST("restart", "replayed_evidence_from_before_the_snapshot_is_fenced") {
  const std::filesystem::path path = scratch("fence.foj");
  {
    Session session(path);
    FO_REQUIRE(session.engine->open().ok());
    ObservationSpec spec;
    spec.bytes = 100;
    FO_REQUIRE(session.engine->submit(make_observation(spec)).ok());
    spec.revision = 2;
    spec.sequence = 2;
    spec.bytes = 900;
    FO_REQUIRE(session.engine->submit(make_observation(spec)).ok());
    FO_REQUIRE(session.engine->compact().ok());
    FO_REQUIRE(session.engine->close().ok());
  }
  Session restarted(path);
  FO_REQUIRE(restarted.engine->open().ok());
  const std::uint64_t before = restarted.engine->state_digest();
  // Re-submitting an observation that was already folded into the snapshot must
  // not double count it.
  ObservationSpec spec;
  spec.bytes = 900;
  spec.revision = 2;
  spec.sequence = 2;
  spec.observed_at = static_cast<std::uint64_t>(base_time().nanos() + 2);
  spec.received_at = spec.observed_at;
  const Result<IngestOutcome> replayed = restarted.engine->submit(make_observation(spec));
  FO_REQUIRE(replayed.ok());
  FO_CHECK_EQ(static_cast<int>(replayed.value().disposition),
              static_cast<int>(IngestDisposition::Refused));
  const Result<FlowSnapshot> snapshot = restarted.engine->snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(snapshot.value().total.bytes, 800ull);
  FO_CHECK_EQ(restarted.engine->state_digest(), before);
  FO_REQUIRE(restarted.engine->close().ok());
}

FOTEST("restart", "completed_stays_completed_across_a_restart") {
  const std::filesystem::path path = scratch("completed.foj");
  {
    Session session(path);
    FO_REQUIRE(session.engine->open().ok());
    FO_REQUIRE(session.engine->register_source(make_source("src-a", 10)).ok());
    ObservationSpec spec;
    spec.bytes = 100;
    FO_REQUIRE(session.engine->submit(make_observation(spec)).ok());
    spec.revision = 2;
    spec.sequence = 2;
    spec.bytes = 900;
    spec.kind = ObservationKind::Close;
    FO_REQUIRE(session.engine->submit(make_observation(spec)).ok());
    const Result<FlowSnapshot> snapshot = session.engine->snapshot(flow_id_from_key("flow-1"));
    FO_REQUIRE(snapshot.ok());
    FO_REQUIRE(asserts_completion(snapshot.value().state));
    FO_REQUIRE(session.engine->close().ok());
  }
  Session restarted(path);
  FO_REQUIRE(restarted.engine->open().ok());
  const Result<FlowSnapshot> snapshot = restarted.engine->snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  // A proven completion is a durable fact, unlike liveness.
  FO_CHECK_EQ(static_cast<int>(snapshot.value().state),
              static_cast<int>(FlowState::Completed));
  FO_CHECK(asserts_completion(snapshot.value().state));
  FO_REQUIRE(restarted.engine->close().ok());
}

FOTEST("restart", "repeated_restarts_increment_the_epoch_and_stay_consistent") {
  const std::filesystem::path path = scratch("epochs.foj");
  for (int round = 0; round < 5; ++round) {
    Session session(path);
    FO_REQUIRE(session.engine->open().ok());
    FO_CHECK_EQ(session.engine->runtime_epoch().value(),
                static_cast<std::uint64_t>(round + 1));
    ObservationSpec spec;
    spec.bytes = static_cast<std::uint64_t>(round + 1) * 10;
    spec.revision = static_cast<std::uint64_t>(round + 1);
    spec.sequence = spec.revision;
    spec.observed_at = static_cast<std::uint64_t>(session.clock.now().nanos());
    spec.received_at = spec.observed_at;
    FO_REQUIRE(session.engine->submit(make_observation(spec)).ok());
    FO_CHECK(session.engine->verify_journal().ok());
    FO_REQUIRE(session.engine->close().ok());
  }
  Session final_session(path);
  FO_REQUIRE(final_session.engine->open().ok());
  const Result<FlowSnapshot> snapshot = final_session.engine->snapshot(flow_id_from_key("flow-1"));
  FO_REQUIRE(snapshot.ok());
  // Replaying the journal rebuilds every observation, so the accounting stays
  // exact across restarts; only the currency of the evidence is denied.
  FO_CHECK_EQ(snapshot.value().total.unknown_intervals, 0ull);
  FO_CHECK(!snapshot.value().is_live());
  FO_CHECK_EQ(final_session.engine->runtime_epoch().value(), 6ull);
  FO_REQUIRE(final_session.engine->close().ok());
}

FOTEST("restart", "journal_of_a_different_identity_scheme_is_refused") {
  const std::filesystem::path path = scratch("scheme.foj");
  {
    Session session(path);
    FO_REQUIRE(session.engine->open().ok());
    ObservationSpec spec;
    spec.bytes = 100;
    FO_REQUIRE(session.engine->submit(make_observation(spec)).ok());
    FO_REQUIRE(session.engine->close().ok());
  }
  EngineConfig config;
  config.journal_path = path;
  config.limits.worker_threads = 0;
  config.recovery.enforce_identity_scheme = true;
  // A journal written under a different scheme cannot be reinterpreted.
  {
    std::filesystem::path tampered = scratch("scheme-tampered.foj");
    std::filesystem::copy_file(path, tampered,
                               std::filesystem::copy_options::overwrite_existing);
    std::fstream file(tampered, std::ios::in | std::ios::out | std::ios::binary);
    FO_REQUIRE(file.good());
    file.seekp(10);
    const char byte = static_cast<char>(9);
    file.write(&byte, 1);
    file.close();
    config.journal_path = tampered;
    Engine engine(config);
    const Status opened = engine.open();
    FO_REQUIRE(!opened.ok());
    FO_CHECK(opened.code() == ErrorCode::IntegrityFailure ||
             opened.code() == ErrorCode::VersionMismatch);
  }
}
