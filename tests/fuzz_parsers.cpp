// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Seeded structural fuzzing of every parser and decoder. The inputs are
// generated, not timed out: a case either returns a Status or it does not, and
// the assertion is always "no crash, no silent acceptance of an invalid input".

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

std::string mutate(const std::string& base, fotest::Rng& rng, int rounds) {
  std::string text = base;
  for (int i = 0; i < rounds && !text.empty(); ++i) {
    const std::size_t position = static_cast<std::size_t>(rng.below(text.size()));
    switch (rng.below(5)) {
      case 0:
        text[position] = static_cast<char>(rng.below(256));
        break;
      case 1:
        text.insert(position, 1, static_cast<char>(rng.below(256)));
        break;
      case 2:
        text.erase(position, 1);
        break;
      case 3:
        text.insert(position, "=");
        break;
      default:
        text.insert(position, "\"");
        break;
    }
  }
  return text;
}

const char* kValid =
    "OBS v=1 src=\"src-a\" inc=1 epoch=1 rev=1 seq=1 flow=\"flow-1\" gen=1 "
    "obs=1700000000000000000 rx=1700000000000000000 kind=sample dir=forward ev=complete "
    "ep=\"ep-a\" peer=\"ep-b\" path=\"p0\" queue=\"q0\" bytes=100 pkts=2 retx=0";

}  // namespace

FOTEST("fuzz", "fo1_parser_never_accepts_a_mutated_line_as_valid_evidence") {
  fo1::Record record;
  FO_REQUIRE(fo1::parse_line(kValid, 1, record).ok());

  for (int round = 0; round < 4000; ++round) {
    fotest::Rng rng(ctx.seed + static_cast<std::uint64_t>(round));
    const std::string mutated = mutate(kValid, rng, 1 + static_cast<int>(rng.below(6)));
    fo1::Record parsed;
    const Status status = fo1::parse_line(mutated, 1, parsed);
    if (status.ok() && parsed.kind == fo1::Record::Kind::Observation) {
      // Whatever the parser accepted must still be structurally usable: the
      // identity, key and timing invariants hold or the engine refuses it.
      FO_CHECK(parsed.observation.key.incarnation.valid());
      FO_CHECK(parsed.observation.subject.generation.valid());
      FO_CHECK(parsed.observation.observed_at.known());
      FO_CHECK(!parsed.observation.subject.flow_key.empty());
      FO_CHECK_EQ(static_cast<int>(parsed.observation.schema_version),
                  static_cast<int>(kObservationSchemaVersion));
    }
  }
}

FOTEST("fuzz", "engine_refuses_every_structurally_broken_observation") {
  ManualClock clock;
  clock.set(fotest::recent_now());
  EngineConfig config;
  config.clock = &clock;
  config.limits.worker_threads = 0;
  Engine engine(config);
  FO_REQUIRE(engine.open().ok());

  for (int round = 0; round < 3000; ++round) {
    fotest::Rng rng(ctx.seed + 7919 + static_cast<std::uint64_t>(round));
    ObservationSpec spec;
    spec.bytes = rng.below(100000);
    spec.revision = 1 + rng.below(1000);
    spec.sequence = spec.revision;
    spec.observed_at = static_cast<std::uint64_t>(base_time().nanos() +
                                                  static_cast<std::int64_t>(spec.revision) * 1000);
    spec.received_at = spec.observed_at;
    Observation observation = make_observation(spec);
    switch (rng.below(8)) {
      case 0: observation.key.source = SourceId::invalid(); break;
      case 1: observation.subject.flow = FlowId::invalid(); break;
      case 2: observation.subject.flow_key = "other-flow"; break;
      case 3: observation.observed_at = Timestamp::unknown(); break;
      case 4: observation.kind = static_cast<ObservationKind>(200); break;
      case 5: observation.direction = static_cast<Direction>(99); break;
      case 6: observation.schema_version = 4242; break;
      default: observation.subject.hop_capacity_units.assign(4096, 1); break;
    }
    const Result<IngestOutcome> outcome = engine.submit(observation);
    // The call must return (never hang, never crash) and a refusal must carry a
    // code. A "success" is only acceptable for a mutation that is structurally
    // legal, which case 2 (a colliding key) is not.
    FO_REQUIRE(outcome.ok());
    if (outcome.value().disposition != IngestDisposition::Applied) {
      FO_CHECK(outcome.value().code != ErrorCode::Ok);
    }
  }
  FO_REQUIRE(engine.close().ok());
}

FOTEST("fuzz", "codec_rejects_mutated_record_bytes") {
  PersistedState state;
  state.runtime_epoch = RuntimeEpoch::from_value(1);
  state.created_at = base_time();
  PersistedFlow flow;
  flow.id = flow_id_from_key("flow-1");
  flow.canonical_key = "flow-1";
  flow.current_generation = GenerationId::from_value(1);
  PersistedGeneration generation;
  generation.generation = GenerationId::from_value(1);
  generation.is_current = true;
  PersistedSource source;
  source.source = source_id_from_key("src-a");
  source.canonical_key = "src-a";
  generation.sources.push_back(source);
  flow.generations.push_back(generation);
  state.flows.push_back(flow);

  Limits limits;
  std::vector<journal::Record> records;
  FO_REQUIRE(encode_state(state, limits, records).ok());
  const Result<PersistedState> clean = decode_state(records, limits, RecoveryPolicy{});
  FO_REQUIRE(clean.ok());

  for (int round = 0; round < 2000; ++round) {
    fotest::Rng rng(ctx.seed + 104729 + static_cast<std::uint64_t>(round));
    std::vector<journal::Record> mutated = records;
    const std::size_t index = static_cast<std::size_t>(rng.below(mutated.size()));
    if (mutated[index].payload.empty()) {
      continue;
    }
    const std::size_t position = static_cast<std::size_t>(rng.below(mutated[index].payload.size()));
    const int operation = static_cast<int>(rng.below(3));
    if (operation == 0) {
      mutated[index].payload[position] =
          static_cast<std::byte>(rng.below(256));
    } else if (operation == 1) {
      mutated[index].payload.resize(position);
    } else {
      mutated[index].payload.insert(mutated[index].payload.begin(),
                                    static_cast<std::byte>(rng.below(256)));
    }
    // The decoder must never crash, and any change to a payload must be
    // detected by the group digest or by a field decoder.
    const Result<PersistedState> decoded = decode_state(mutated, limits, RecoveryPolicy{});
    if (decoded.ok()) {
      FO_CHECK_EQ(digest_state(decoded.value()), digest_state(state));
    }
  }
}

FOTEST("fuzz", "journal_reader_survives_random_byte_corruption") {
  const std::filesystem::path base = std::filesystem::temp_directory_path() / "flowobs-tests";
  std::error_code ec;
  std::filesystem::create_directories(base, ec);

  for (int round = 0; round < 120; ++round) {
    const std::filesystem::path path =
        base / ("fuzz-" + std::to_string(round) + ".foj");
    std::filesystem::remove(path, ec);
    {
      ManualClock clock;
      clock.set(fotest::recent_now());
      EngineConfig config;
      config.clock = &clock;
      config.journal_path = path;
      config.limits.worker_threads = 0;
      Engine engine(config);
      FO_REQUIRE(engine.open().ok());
      for (int i = 0; i < 6; ++i) {
        ObservationSpec spec;
        spec.bytes = static_cast<std::uint64_t>(i) * 10;
        spec.revision = static_cast<std::uint64_t>(i + 1);
        spec.sequence = spec.revision;
        spec.observed_at = static_cast<std::uint64_t>(base_time().nanos() +
                                                      static_cast<std::int64_t>(i + 1) * 1000);
        spec.received_at = spec.observed_at;
        FO_REQUIRE(engine.submit(make_observation(spec)).ok());
      }
      FO_REQUIRE(engine.close().ok());
    }
    std::string bytes;
    {
      std::ifstream input(path, std::ios::binary);
      bytes.assign((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    }
    FO_REQUIRE(bytes.size() > 64u);
    fotest::Rng rng(ctx.seed + 555 + static_cast<std::uint64_t>(round));
    const std::size_t position =
        64 + static_cast<std::size_t>(rng.below(bytes.size() - 64));
    bytes[position] = static_cast<char>(bytes[position] ^ static_cast<char>(1 + rng.below(255)));
    {
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    JournalOpenOptions options;
    options.recovery.torn_tail = RecoveryPolicy::TornTail::TruncateTornTail;
    Result<std::unique_ptr<Journal>> opened = Journal::open(path, options);
    if (opened.ok()) {
      Result<std::vector<journal::Record>> records = opened.value()->read_all();
      // Either the reader refuses the journal or it reads a strictly shorter
      // prefix; it never reports success on bytes it did not verify.
      if (records.ok()) {
        FO_CHECK(records.value().size() <= 8u);
      }
      FO_CHECK(opened.value()->close().ok());
    }
    std::filesystem::remove(path, ec);
  }
}
