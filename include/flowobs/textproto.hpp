// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The canonical "FO1" line format. It exists so that observations can be
// produced, stored, diffed and replayed as text, and so that tests can drive
// the runtime deterministically (including the evaluation instant) without a
// live fabric.
//
// Grammar (all tokens separated by single spaces; keys are double quoted with
// backslash escapes for \" and \\):
//
//   SRC  v=1 key="<key>" auth=<u32> [caps=<a,b,...>] [advisory=0|1]
//   OBS  v=1 src="<key>" inc=<u64> epoch=<u64> rev=<u64> seq=<u64>
//        flow="<key>" gen=<u64> obs=<ns> [rx=<ns>] kind=<name> dir=<name>
//        ev=<name> [ep="<key>"] [peer="<key>"] [path="<key>"] [queue="<key>"]
//        [link="<key>"] [bytes=<u64>|-] [pkts=<u64>|-] [retx=<u64>|-]
//        [dur=<ns>] [rtt=<ns>] [caps=<a,b,...>] [schema=<u32>]
//   AT   v=1 now=<ns>
//   TICK v=1 [now=<ns>]
//   FREEZE
//   # comment
//
// A "-" counter means "not supplied"; it is never read as zero. Unknown tokens
// are rejected: the parser fails closed.

#ifndef FLOWOBS_TEXTPROTO_HPP
#define FLOWOBS_TEXTPROTO_HPP

#include <string>
#include <string_view>
#include <vector>

#include "flowobs/error.hpp"
#include "flowobs/export.hpp"
#include "flowobs/observation.hpp"
#include "flowobs/time.hpp"

namespace flowobs {
namespace fo1 {

// One parsed script record.
struct FLOWOBS_PUBLIC Record final {
  enum class Kind : std::uint8_t { Comment, Source, Observation, At, Tick, Freeze };
  Kind kind = Kind::Comment;
  std::size_t line = 0;
  SourceDescriptor source{};
  Observation observation{};
  Timestamp now{};
};

struct FLOWOBS_PUBLIC Script final {
  std::vector<Record> records;
  std::size_t source_records = 0;
  std::size_t observation_records = 0;
  std::size_t at_records = 0;
  std::size_t tick_records = 0;
  std::size_t freeze_records = 0;
};

// Parses one line. `line_number` is used only for error messages. Empty lines
// and lines starting with '#' are Comment records and never fail.
FLOWOBS_PUBLIC Status parse_line(std::string_view line, std::size_t line_number,
                                 Record& out);

// Parses a whole script. Bounded by `max_records`.
FLOWOBS_PUBLIC Result<Script> parse_script(std::string_view text, std::size_t max_records);

// Deterministic rendering of an observation (without the SRC line).
FLOWOBS_PUBLIC std::string format_observation(const Observation& observation);

// Deterministic rendering of a source descriptor.
FLOWOBS_PUBLIC std::string format_source(const SourceDescriptor& descriptor);

// Canonical name lists for capability flags.
FLOWOBS_PUBLIC std::string format_capabilities(const SourceCapabilities& capabilities);
FLOWOBS_PUBLIC bool parse_capabilities(std::string_view text,
                                       SourceCapabilities& out) noexcept;

}  // namespace fo1
}  // namespace flowobs

#endif  // FLOWOBS_TEXTPROTO_HPP
