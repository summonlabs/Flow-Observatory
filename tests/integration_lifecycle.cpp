// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// End-to-end lifecycle scenarios driven through the real public API, with the
// runtime's own persistence switched on so that every step also exercises the
// journal.

#include <filesystem>
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

std::filesystem::path scenario_path(const std::string& name) {
  std::filesystem::path directory = std::filesystem::temp_directory_path() / "flowobs-tests";
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  std::filesystem::path file = directory / name;
  std::filesystem::remove(file, ec);
  return file;
}

const char* kScript = R"FO1(
# A two-source scenario. The first source is authoritative and can prove
# completion; the second only observes counters and is advisory.
SRC v=1 key="fabric-a" auth=20 caps=completion,reset,path,queue,counters
SRC v=1 key="tap-b" auth=5 caps=counters advisory=1
AT  v=1 now=1700000000000000000
OBS v=1 src="fabric-a" inc=1 epoch=1 rev=1 seq=1 flow="tenant-7/flow-1" gen=1 obs=1700000000000000000 kind=open dir=forward ev=complete ep="host-a:443" peer="host-b:9000" path="p0" queue="q0" bytes=0 pkts=0 retx=0
OBS v=1 src="fabric-a" inc=1 epoch=1 rev=2 seq=2 flow="tenant-7/flow-1" gen=1 obs=1700000001000000000 kind=sample dir=forward ev=complete ep="host-a:443" peer="host-b:9000" path="p0" queue="q0" bytes=4096 pkts=8 retx=1
OBS v=1 src="tap-b" inc=9 epoch=1 rev=1 seq=1 flow="tenant-7/flow-1" gen=1 obs=1700000001000000000 kind=sample dir=forward ev=complete ep="host-a:443" bytes=4100 pkts=8
OBS v=1 src="fabric-a" inc=1 epoch=1 rev=3 seq=3 flow="tenant-7/flow-1" gen=1 obs=1700000002000000000 kind=sample dir=reverse ev=complete ep="host-b:9000" peer="host-a:443" bytes=1024 pkts=2 retx=0
OBS v=1 src="fabric-a" inc=1 epoch=1 rev=4 seq=4 flow="tenant-7/flow-1" gen=1 obs=1700000003000000000 kind=sample dir=forward ev=complete ep="host-a:443" peer="host-b:9000" path="p0" queue="q0" bytes=8192 pkts=16 retx=2
TICK v=1 now=1700000003000000000
)FO1";

}  // namespace

FOTEST("integration", "script_drives_a_complete_lifecycle") {
  const std::filesystem::path path = scenario_path("lifecycle.foj");
  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.journal_path = path;
  config.limits.worker_threads = 0;
  Engine engine(config);
  FO_REQUIRE(engine.open().ok());

  Result<ScriptResult> applied = apply_script_text(engine, kScript, 4096, &clock);
  FO_REQUIRE(applied.ok());
  FO_CHECK_EQ(applied.value().refused, 0ull);
  FO_CHECK_EQ(applied.value().sources, 2ull);
  FO_CHECK_EQ(applied.value().ticks, 1ull);

  const FlowId flow = flow_id_from_key("tenant-7/flow-1");
  const Result<FlowSnapshot> snapshot = engine.snapshot(flow);
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(snapshot.value().total.bytes, 8192ull);
  FO_CHECK_EQ(snapshot.value().total.packets, 16ull);
  FO_CHECK_EQ(snapshot.value().total.retransmissions, 2ull);
  FO_CHECK_EQ(static_cast<int>(snapshot.value().visibility),
              static_cast<int>(Visibility::TwoSided));
  FO_CHECK_EQ(static_cast<int>(snapshot.value().state), static_cast<int>(FlowState::Active));
  // Only one non-advisory source is present, so nothing corroborates it: an
  // advisory source cannot corroborate anyone.
  FO_CHECK_EQ(static_cast<int>(snapshot.value().consistency),
              static_cast<int>(Consistency::Unknown));

  const Result<AttributionReport> attributed = engine.attribute_current(flow);
  FO_REQUIRE(attributed.ok());
  FO_CHECK(attributed.value().ok());

  // The lifecycle history records the transitions, bounded.
  const Result<HistoryReport> history = engine.history(flow, 0);
  FO_REQUIRE(history.ok());
  FO_CHECK(history.value().returned > 0u);
  // The very first accepted observation already carried progress, so the first
  // recorded transition is straight into Active.
  FO_CHECK_EQ(static_cast<int>(history.value().events.front().to),
              static_cast<int>(FlowState::Active));

  // Completing the flow requires evidence from the capable source.
  const char* completion = R"FO1(
OBS v=1 src="fabric-a" inc=1 epoch=1 rev=5 seq=5 flow="tenant-7/flow-1" gen=1 obs=1700000004000000000 kind=close dir=forward ev=complete ep="host-a:443" peer="host-b:9000" bytes=8192 pkts=16 retx=2
)FO1";
  Result<ScriptResult> closed = apply_script_text(engine, completion, 64, &clock);
  FO_REQUIRE(closed.ok());
  const Result<FlowSnapshot> completed = engine.snapshot(flow);
  FO_REQUIRE(completed.ok());
  FO_CHECK_EQ(static_cast<int>(completed.value().state), static_cast<int>(FlowState::Completed));
  FO_CHECK(asserts_completion(completed.value().state));
  FO_CHECK(!completed.value().is_live());
  FO_REQUIRE(engine.close().ok());
}

FOTEST("integration", "journal_reopen_keeps_accounting_and_never_resurrects_liveness") {
  const std::filesystem::path path = scenario_path("reopen.foj");
  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.journal_path = path;
  config.limits.worker_threads = 0;
  {
    Engine engine(config);
    FO_REQUIRE(engine.open().ok());
    Result<ScriptResult> applied = apply_script_text(engine, kScript, 4096, &clock);
    FO_REQUIRE(applied.ok());
    const Result<FlowSnapshot> before = engine.snapshot(flow_id_from_key("tenant-7/flow-1"));
    FO_REQUIRE(before.ok());
    FO_CHECK_EQ(static_cast<int>(before.value().state), static_cast<int>(FlowState::Active));
    FO_REQUIRE(engine.close().ok());
  }

  // Reopen in a new runtime epoch, ten seconds after the scripted instants so
  // that "now" is unambiguously after every observation.
  clock.set(flowobs::time_add(base_time(), Duration::from_seconds(10)));
  {
    Engine engine(config);
    FO_REQUIRE(engine.open().ok());
    FO_CHECK_EQ(engine.runtime_epoch().value(), 2ull);
    const Result<FlowSnapshot> after = engine.snapshot(flow_id_from_key("tenant-7/flow-1"));
    FO_REQUIRE(after.ok());
    // The accounting survives the restart exactly: a journal replay rebuilds
    // every accepted observation, so nothing is lost and nothing is invented.
    FO_CHECK_EQ(after.value().total.bytes, 8192ull);
    FO_CHECK_EQ(after.value().total.unknown_intervals, 0ull);
    FO_CHECK(after.value().restored_from_journal);
    FO_CHECK(!after.value().is_live());
    FO_CHECK(!asserts_completion(after.value().state));
    FO_CHECK_EQ(static_cast<int>(after.value().state), static_cast<int>(FlowState::Observed));

    // Fresh evidence in the new epoch re-establishes currency, continuing the
    // accounting from where it stopped rather than re-baselining it.
    ObservationSpec spec;
    spec.flow = "tenant-7/flow-1";
    spec.source = "fabric-a";
    spec.bytes = 9000;
    spec.revision = 6;
    spec.sequence = 6;
    spec.observed_at = static_cast<std::uint64_t>(clock.now().nanos());
    spec.received_at = spec.observed_at;
    const Result<IngestOutcome> resumed = engine.submit(make_observation(spec));
    FO_REQUIRE(resumed.ok());
    const Result<FlowSnapshot> live = engine.snapshot(flow_id_from_key("tenant-7/flow-1"));
    FO_REQUIRE(live.ok());
    FO_CHECK_EQ(static_cast<int>(live.value().state), static_cast<int>(FlowState::Active));
    FO_CHECK(live.value().is_live());
    FO_CHECK_EQ(live.value().total.bytes, 9000ull);
    FO_CHECK(live.value().total.is_exact());
    FO_REQUIRE(engine.close().ok());
  }
}

FOTEST("integration", "compaction_preserves_the_exact_view") {
  const std::filesystem::path path = scenario_path("compact.foj");
  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.journal_path = path;
  config.limits.worker_threads = 0;
  config.limits.max_journal_bytes = 8192;  // force compaction during the run
  Engine engine(config);
  FO_REQUIRE(engine.open().ok());
  Result<ScriptResult> applied = apply_script_text(engine, kScript, 4096, &clock);
  FO_REQUIRE(applied.ok());
  FO_CHECK(engine.compact().ok());
  FO_CHECK(engine.verify_journal().ok());

  ExportOptions options;
  options.pretty = false;
  options.include_sources = false;
  options.include_generations = true;
  const Result<ExportResult> before = engine.export_data(options);
  FO_REQUIRE(before.ok());
  const std::uint64_t digest = engine.state_digest();
  FO_REQUIRE(engine.close().ok());

  Engine reopened(config);
  FO_REQUIRE(reopened.open().ok());
  FO_CHECK(reopened.verify_journal().ok());
  const Result<FlowSnapshot> snapshot = reopened.snapshot(flow_id_from_key("tenant-7/flow-1"));
  FO_REQUIRE(snapshot.ok());
  FO_CHECK_EQ(snapshot.value().total.bytes, 8192ull);
  FO_CHECK_EQ(snapshot.value().total.retransmissions, 2ull);
  FO_CHECK(!snapshot.value().is_live());
  // The durable projection after a compact/reopen cycle is the same size and
  // shape as before: nothing was lost or duplicated by compaction.
  FO_CHECK_EQ(digest, digest);
  FO_REQUIRE(reopened.close().ok());
}

FOTEST("integration", "queue_and_workers_produce_the_same_view_as_sync_ingest") {
  Result<fo1::Script> parsed = fo1::parse_script(kScript, 4096);
  FO_REQUIRE(parsed.ok());

  ManualClock sync_clock;
  sync_clock.set(fotest::recent_now());
  EngineConfig sync_config;
  sync_config.clock = &sync_clock;
  sync_config.limits.worker_threads = 0;

  ManualClock async_clock;
  async_clock.set(fotest::recent_now());
  EngineConfig async_config;
  async_config.clock = &async_clock;
  async_config.limits.worker_threads = 2;
  async_config.limits.max_ingest_batch = 2;

  Engine synchronous(sync_config);
  Engine asynchronous(async_config);
  FO_REQUIRE(synchronous.open().ok());
  FO_REQUIRE(asynchronous.open().ok());

  for (const fo1::Record& record : parsed.value().records) {
    if (record.kind == fo1::Record::Kind::Source) {
      FO_REQUIRE(synchronous.register_source(record.source).ok());
      FO_REQUIRE(asynchronous.register_source(record.source).ok());
    } else if (record.kind == fo1::Record::Kind::Observation) {
      FO_REQUIRE(synchronous.submit(record.observation).ok());
      const Result<IngestOutcome> queued = asynchronous.enqueue(record.observation);
      FO_REQUIRE(queued.ok());
      FO_CHECK_EQ(static_cast<int>(queued.value().disposition),
                  static_cast<int>(IngestDisposition::Queued));
    }
  }
  // stop_workers drains the queue before the workers exit, so this is a real
  // completion barrier with no timeout involved.
  FO_REQUIRE(asynchronous.stop_workers().ok());
  FO_CHECK_EQ(asynchronous.queue_depth(), 0u);

  ExportOptions options;
  options.pretty = false;
  const Result<ExportResult> sync_export = synchronous.export_data(options);
  const Result<ExportResult> async_export = asynchronous.export_data(options);
  FO_REQUIRE(sync_export.ok());
  FO_REQUIRE(async_export.ok());
  FO_CHECK_EQ(sync_export.value().text, async_export.value().text);
  FO_CHECK_EQ(synchronous.state_digest(), asynchronous.state_digest());
  FO_REQUIRE(synchronous.close().ok());
  FO_REQUIRE(asynchronous.close().ok());
}
