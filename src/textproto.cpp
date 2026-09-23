// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowobs/textproto.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "flowobs/checked.hpp"

namespace flowobs {
namespace fo1 {

namespace {

struct Token final {
  std::string key;
  std::string value;
  bool quoted = false;
  bool has_value = false;
  std::size_t column = 0;
};

Status tokenize(std::string_view line, std::vector<Token>& out, std::size_t max_tokens) {
  out.clear();
  std::size_t i = 0;
  const std::size_t size = line.size();
  while (i < size) {
    while (i < size && (line[i] == ' ' || line[i] == '\t')) {
      ++i;
    }
    if (i >= size) {
      break;
    }
    if (out.size() >= max_tokens) {
      return limit_exceeded("FO1 line has more tokens than the configured bound");
    }
    Token token;
    token.column = i;
    const std::size_t key_start = i;
    while (i < size && line[i] != '=' && line[i] != ' ' && line[i] != '\t') {
      ++i;
    }
    token.key.assign(line.substr(key_start, i - key_start));
    if (i < size && line[i] == '=') {
      ++i;
      token.has_value = true;
      if (i < size && line[i] == '"') {
        token.quoted = true;
        ++i;
        std::string value;
        bool closed = false;
        while (i < size) {
          const char c = line[i];
          if (c == '\\' && i + 1 < size) {
            const char next = line[i + 1];
            if (next == '"' || next == '\\') {
              value.push_back(next);
              i += 2;
              continue;
            }
            return invalid_argument("FO1 line has an unsupported escape sequence");
          }
          if (c == '"') {
            closed = true;
            ++i;
            break;
          }
          value.push_back(c);
          ++i;
        }
        if (!closed) {
          return invalid_argument("FO1 line has an unterminated quoted value");
        }
        token.value = std::move(value);
      } else {
        const std::size_t value_start = i;
        while (i < size && line[i] != ' ' && line[i] != '\t') {
          ++i;
        }
        token.value.assign(line.substr(value_start, i - value_start));
      }
    }
    out.push_back(std::move(token));
  }
  return make_ok();
}

bool parse_u64(const std::string& text, std::uint64_t& out) noexcept {
  if (text.empty() || text.size() > 20) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    const Checked<std::uint64_t> scaled = checked_mul(value, 10ull);
    if (scaled.overflow) {
      return false;
    }
    const Checked<std::uint64_t> added = checked_add(scaled.value, static_cast<std::uint64_t>(c - '0'));
    if (added.overflow) {
      return false;
    }
    value = added.value;
  }
  out = value;
  return true;
}

bool parse_i64(const std::string& text, std::int64_t& out) noexcept {
  if (text.empty()) {
    return false;
  }
  bool negative = false;
  std::string_view body(text);
  if (body.front() == '-') {
    negative = true;
    body.remove_prefix(1);
  }
  std::uint64_t magnitude = 0;
  if (!parse_u64(std::string(body), magnitude)) {
    return false;
  }
  if (negative) {
    if (magnitude > 9223372036854775808ull) {
      return false;
    }
    out = (magnitude == 9223372036854775808ull)
              ? INT64_MIN
              : -static_cast<std::int64_t>(magnitude);
    return true;
  }
  if (magnitude > 9223372036854775807ull) {
    return false;
  }
  out = static_cast<std::int64_t>(magnitude);
  return true;
}

bool parse_u32(const std::string& text, std::uint32_t& out) noexcept {
  std::uint64_t value = 0;
  if (!parse_u64(text, value) || value > 0xFFFFFFFFull) {
    return false;
  }
  out = static_cast<std::uint32_t>(value);
  return true;
}

// Reads an optional counter token: "-" means "not supplied".
bool parse_optional_counter(const std::string& text, std::optional<std::uint64_t>& out) noexcept {
  if (text == "-") {
    out.reset();
    return true;
  }
  std::uint64_t value = 0;
  if (!parse_u64(text, value)) {
    return false;
  }
  out = value;
  return true;
}

Status expect_version(const std::vector<Token>& tokens, std::size_t index) {
  if (index >= tokens.size() || tokens[index].key != "v") {
    return invalid_argument("FO1 record must begin with v=<version>");
  }
  std::uint32_t version = 0;
  if (!parse_u32(tokens[index].value, version)) {
    return invalid_argument("FO1 version is not a number");
  }
  if (version != kTextObservationVersion) {
    return Status(ErrorCode::VersionMismatch, "unsupported FO1 version");
  }
  return make_ok();
}

void append_quoted(std::string& out, std::string_view value) {
  out.push_back('"');
  for (const char c : value) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  out.push_back('"');
}

void append_id_or_key(std::string& out, std::string_view key, std::uint64_t id) {
  if (!key.empty()) {
    append_quoted(out, key);
    return;
  }
  out.push_back('@');
  append_hex16(out, id);
}

void append_optional(std::string& out, const char* name,
                     const std::optional<std::uint64_t>& value) {
  out.push_back(' ');
  out.append(name);
  out.push_back('=');
  if (value.has_value()) {
    out.append(std::to_string(*value));
  } else {
    out.push_back('-');
  }
}

}  // namespace

Status parse_line(std::string_view line, std::size_t line_number, Record& out) {
  out = Record{};
  out.line = line_number;

  // Trim trailing CR so that CRLF files work unchanged.
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
    line.remove_suffix(1);
  }
  std::size_t first = 0;
  while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) {
    ++first;
  }
  if (first >= line.size() || line[first] == '#') {
    out.kind = Record::Kind::Comment;
    return make_ok();
  }

  std::vector<Token> tokens;
  const Status status = tokenize(line, tokens, 64);
  if (status.failed()) {
    return Status(status.code(), "line " + std::to_string(line_number) + ": " + status.message());
  }
  if (tokens.empty()) {
    out.kind = Record::Kind::Comment;
    return make_ok();
  }

  const std::string directive = tokens[0].has_value ? tokens[0].key : tokens[0].key;
  const auto fail = [line_number](std::string message) {
    return invalid_argument("line " + std::to_string(line_number) + ": " + std::move(message));
  };

  if (directive == "FREEZE") {
    out.kind = Record::Kind::Freeze;
    return make_ok();
  }
  if (directive == "SRC") {
    const Status version = expect_version(tokens, 1);
    if (version.failed()) {
      return fail(version.message());
    }
    out.kind = Record::Kind::Source;
    SourceDescriptor descriptor;
    bool have_key = false;
    for (std::size_t i = 2; i < tokens.size(); ++i) {
      const Token& token = tokens[i];
      if (!token.has_value) {
        return fail("SRC token '" + token.key + "' has no value");
      }
      if (token.key == "key") {
        descriptor.canonical_key = token.value;
        have_key = true;
      } else if (token.key == "auth") {
        if (!parse_u32(token.value, descriptor.authority)) {
          return fail("SRC auth is not a valid u32");
        }
      } else if (token.key == "caps") {
        if (!parse_capabilities(token.value, descriptor.capabilities)) {
          return fail("SRC caps has an unknown capability name");
        }
      } else if (token.key == "advisory") {
        if (token.value == "1" || token.value == "true") {
          descriptor.advisory_only = true;
        } else if (token.value == "0" || token.value == "false") {
          descriptor.advisory_only = false;
        } else {
          return fail("SRC advisory must be 0/1 or true/false");
        }
      } else {
        return fail("SRC has an unknown token '" + token.key + "'");
      }
    }
    if (!have_key || descriptor.canonical_key.empty()) {
      return fail("SRC requires key=\"...\"");
    }
    descriptor.id = source_id_from_key(descriptor.canonical_key);
    out.source = descriptor;
    return make_ok();
  }
  if (directive == "AT") {
    const Status version = expect_version(tokens, 1);
    if (version.failed()) {
      return fail(version.message());
    }
    out.kind = Record::Kind::At;
    bool have_now = false;
    for (std::size_t i = 2; i < tokens.size(); ++i) {
      if (tokens[i].key != "now") {
        return fail("AT has an unknown token '" + tokens[i].key + "'");
      }
      std::int64_t nanos = 0;
      if (!parse_i64(tokens[i].value, nanos)) {
        return fail("AT now is not a valid nanosecond instant");
      }
      out.now = Timestamp::from_nanos(nanos);
      have_now = true;
    }
    if (!have_now) {
      return fail("AT requires now=<nanoseconds>");
    }
    return make_ok();
  }
  if (directive == "TICK") {
    const Status version = expect_version(tokens, 1);
    if (version.failed()) {
      return fail(version.message());
    }
    out.kind = Record::Kind::Tick;
    out.now = Timestamp::unknown();
    for (std::size_t i = 2; i < tokens.size(); ++i) {
      if (tokens[i].key != "now") {
        return fail("TICK has an unknown token '" + tokens[i].key + "'");
      }
      std::int64_t nanos = 0;
      if (!parse_i64(tokens[i].value, nanos)) {
        return fail("TICK now is not a valid nanosecond instant");
      }
      out.now = Timestamp::from_nanos(nanos);
    }
    return make_ok();
  }
  if (directive != "OBS") {
    return fail("unknown FO1 directive '" + directive + "'");
  }

  const Status version = expect_version(tokens, 1);
  if (version.failed()) {
    return fail(version.message());
  }
  out.kind = Record::Kind::Observation;
  Observation observation;
  bool have_src = false;
  bool have_flow = false;
  bool have_obs_at = false;
  bool have_kind = false;
  for (std::size_t i = 2; i < tokens.size(); ++i) {
    const Token& token = tokens[i];
    if (!token.has_value) {
      return fail("OBS token '" + token.key + "' has no value");
    }
    if (token.key == "src") {
      observation.key.source = source_id_from_key(token.value);
      have_src = true;
    } else if (token.key == "inc") {
      std::uint64_t value = 0;
      if (!parse_u64(token.value, value)) return fail("OBS inc is not a u64");
      observation.key.incarnation = IncarnationId::from_value(value);
    } else if (token.key == "epoch") {
      std::uint64_t value = 0;
      if (!parse_u64(token.value, value)) return fail("OBS epoch is not a u64");
      observation.key.epoch = EpochId::from_value(value);
    } else if (token.key == "rev") {
      std::uint64_t value = 0;
      if (!parse_u64(token.value, value)) return fail("OBS rev is not a u64");
      observation.key.revision = RevisionId::from_value(value);
    } else if (token.key == "seq") {
      std::uint64_t value = 0;
      if (!parse_u64(token.value, value)) return fail("OBS seq is not a u64");
      observation.key.sequence = SequenceNo::from_value(value);
    } else if (token.key == "flow") {
      observation.subject.flow_key = token.value;
      observation.subject.flow = flow_id_from_key(token.value);
      have_flow = true;
    } else if (token.key == "gen") {
      std::uint64_t value = 0;
      if (!parse_u64(token.value, value)) return fail("OBS gen is not a u64");
      observation.subject.generation = GenerationId::from_value(value);
    } else if (token.key == "ep") {
      observation.subject.endpoint_key = token.value;
      observation.subject.endpoint = endpoint_id_from_key(token.value);
    } else if (token.key == "peer") {
      observation.subject.peer_endpoint_key = token.value;
      observation.subject.peer_endpoint = endpoint_id_from_key(token.value);
    } else if (token.key == "path") {
      observation.subject.path_key = token.value;
      observation.subject.path = path_id_from_key(token.value);
    } else if (token.key == "queue") {
      observation.subject.queue_key = token.value;
      observation.subject.queue = queue_id_from_key(token.value);
    } else if (token.key == "link") {
      observation.subject.link_key = token.value;
      observation.subject.link = link_id_from_key(token.value);
    } else if (token.key == "pathgen") {
      std::uint64_t value = 0;
      if (!parse_u64(token.value, value)) return fail("OBS pathgen is not a u64");
      observation.subject.path_generation = GenerationId::from_value(value);
    } else if (token.key == "hopcap") {
      observation.subject.hop_capacity_units.clear();
      std::string_view rest(token.value);
      while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        const std::string_view part =
            comma == std::string_view::npos ? rest : rest.substr(0, comma);
        std::uint64_t capacity = 0;
        if (!parse_u64(std::string(part), capacity)) {
          return fail("OBS hopcap contains a non-numeric entry");
        }
        observation.subject.hop_capacity_units.push_back(capacity);
        if (comma == std::string_view::npos) break;
        rest.remove_prefix(comma + 1);
      }
    } else if (token.key == "obs") {
      std::int64_t nanos = 0;
      if (!parse_i64(token.value, nanos)) return fail("OBS obs is not a valid instant");
      observation.observed_at = Timestamp::from_nanos(nanos);
      have_obs_at = true;
    } else if (token.key == "rx") {
      std::int64_t nanos = 0;
      if (!parse_i64(token.value, nanos)) return fail("OBS rx is not a valid instant");
      observation.received_at = Timestamp::from_nanos(nanos);
    } else if (token.key == "kind") {
      if (!parse_observation_kind(token.value, observation.kind)) {
        return fail("OBS kind '" + token.value + "' is not a known observation kind");
      }
      have_kind = true;
    } else if (token.key == "dir") {
      if (!parse_direction(token.value, observation.direction)) {
        return fail("OBS dir '" + token.value + "' is not a known direction");
      }
    } else if (token.key == "ev") {
      if (!parse_evidence(token.value, observation.evidence)) {
        return fail("OBS ev '" + token.value + "' is not a known evidence class");
      }
    } else if (token.key == "bytes") {
      if (!parse_optional_counter(token.value, observation.counters.bytes)) {
        return fail("OBS bytes is neither a u64 nor '-'");
      }
    } else if (token.key == "pkts") {
      if (!parse_optional_counter(token.value, observation.counters.packets)) {
        return fail("OBS pkts is neither a u64 nor '-'");
      }
    } else if (token.key == "retx") {
      if (!parse_optional_counter(token.value, observation.counters.retransmissions)) {
        return fail("OBS retx is neither a u64 nor '-'");
      }
    } else if (token.key == "dur") {
      std::int64_t nanos = 0;
      if (!parse_i64(token.value, nanos)) return fail("OBS dur is not a valid duration");
      observation.covered_duration = Duration::from_nanos(nanos);
    } else if (token.key == "rtt") {
      std::int64_t nanos = 0;
      if (!parse_i64(token.value, nanos)) return fail("OBS rtt is not a valid duration");
      observation.reported_rtt = Duration::from_nanos(nanos);
    } else if (token.key == "caps") {
      if (!parse_capabilities(token.value, observation.capabilities)) {
        return fail("OBS caps has an unknown capability name");
      }
    } else if (token.key == "schema") {
      std::uint32_t schema = 0;
      if (!parse_u32(token.value, schema)) return fail("OBS schema is not a u32");
      observation.schema_version = schema;
    } else {
      return fail("OBS has an unknown token '" + token.key + "'");
    }
  }
  if (!have_src) return fail("OBS requires src=\"...\"");
  if (!have_flow) return fail("OBS requires flow=\"...\"");
  if (!have_obs_at) return fail("OBS requires obs=<nanoseconds>");
  if (!have_kind) return fail("OBS requires kind=<name>");
  if (!observation.key.incarnation.valid()) {
    return fail("OBS requires a non-zero inc=<u64> incarnation fence");
  }
  if (!observation.subject.generation.valid()) {
    return fail("OBS requires a non-zero gen=<u64>");
  }
  out.observation = std::move(observation);
  return make_ok();
}

Result<Script> parse_script(std::string_view text, std::size_t max_records) {
  Script script;
  std::size_t line_number = 0;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    const std::string_view line =
        end == std::string_view::npos ? text.substr(start) : text.substr(start, end - start);
    ++line_number;
    Record record;
    const Status status = parse_line(line, line_number, record);
    if (status.failed()) {
      return status;
    }
    if (record.kind != Record::Kind::Comment) {
      if (script.records.size() >= max_records) {
        return limit_exceeded("FO1 script exceeds the configured record bound");
      }
      switch (record.kind) {
        case Record::Kind::Source: ++script.source_records; break;
        case Record::Kind::Observation: ++script.observation_records; break;
        case Record::Kind::At: ++script.at_records; break;
        case Record::Kind::Tick: ++script.tick_records; break;
        case Record::Kind::Freeze: ++script.freeze_records; break;
        case Record::Kind::Comment: break;
      }
      script.records.push_back(std::move(record));
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return script;
}

std::string format_capabilities(const SourceCapabilities& capabilities) {
  return capabilities.describe();
}

bool parse_capabilities(std::string_view text, SourceCapabilities& out) noexcept {
  out = SourceCapabilities{};
  out.asserts_counters = false;
  if (text.empty() || text == "none") {
    return true;
  }
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::string_view part =
        comma == std::string_view::npos ? text.substr(start) : text.substr(start, comma - start);
    if (part == "completion") {
      out.asserts_completion = true;
    } else if (part == "reset") {
      out.asserts_reset = true;
    } else if (part == "path") {
      out.asserts_path = true;
    } else if (part == "queue") {
      out.asserts_queue = true;
    } else if (part == "counters") {
      out.asserts_counters = true;
    } else if (part == "identity") {
      out.authoritative_identity = true;
    } else if (!part.empty()) {
      return false;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return true;
}

std::string format_source(const SourceDescriptor& descriptor) {
  std::string out;
  out.reserve(96);
  out.append("SRC v=");
  out.append(std::to_string(kTextObservationVersion));
  out.append(" key=\"");
  out.append(descriptor.canonical_key);
  out.append("\" auth=");
  out.append(std::to_string(descriptor.authority));
  out.append(" caps=");
  out.append(format_capabilities(descriptor.capabilities));
  out.append(" advisory=");
  out.push_back(descriptor.advisory_only ? '1' : '0');
  return out;
}

std::string format_observation(const Observation& observation) {
  std::string out;
  out.reserve(320);
  out.append("OBS v=");
  out.append(std::to_string(kTextObservationVersion));
  out.append(" src=");
  if (!observation.source_key.empty()) {
    append_quoted(out, observation.source_key);
  } else {
    out.push_back('@');
    append_hex16(out, observation.key.source.value());
  }
  out.append(" flow=");
  if (!observation.subject.flow_key.empty()) {
    append_quoted(out, observation.subject.flow_key);
  } else {
    out.push_back('@');
    append_hex16(out, observation.subject.flow.value());
  }
  out.append(" gen=");
  out.append(std::to_string(observation.subject.generation.value()));
  out.append(" inc=");
  out.append(std::to_string(observation.key.incarnation.value()));
  out.append(" epoch=");
  out.append(std::to_string(observation.key.epoch.value()));
  out.append(" rev=");
  out.append(std::to_string(observation.key.revision.value()));
  out.append(" seq=");
  out.append(std::to_string(observation.key.sequence.value()));
  if (observation.subject.endpoint.valid()) {
    out.append(" ep=");
    append_id_or_key(out, observation.subject.endpoint_key, observation.subject.endpoint.value());
  }
  if (observation.subject.peer_endpoint.valid()) {
    out.append(" peer=");
    append_id_or_key(out, observation.subject.peer_endpoint_key,
                     observation.subject.peer_endpoint.value());
  }
  if (observation.subject.path.has_value()) {
    out.append(" path=");
    append_id_or_key(out, observation.subject.path_key, observation.subject.path->value());
  }
  if (observation.subject.queue.has_value()) {
    out.append(" queue=");
    append_id_or_key(out, observation.subject.queue_key, observation.subject.queue->value());
  }
  if (observation.subject.link.has_value()) {
    out.append(" link=");
    append_id_or_key(out, observation.subject.link_key, observation.subject.link->value());
  }
  if (observation.subject.path_generation.has_value()) {
    out.append(" pathgen=");
    out.append(std::to_string(observation.subject.path_generation->value()));
  }
  if (!observation.subject.hop_capacity_units.empty()) {
    out.append(" hopcap=");
    for (std::size_t i = 0; i < observation.subject.hop_capacity_units.size(); ++i) {
      if (i != 0) {
        out.push_back(',');
      }
      out.append(std::to_string(observation.subject.hop_capacity_units[i]));
    }
  }
  out.append(" obs=");
  out.append(std::to_string(observation.observed_at.nanos()));
  if (observation.received_at.known()) {
    out.append(" rx=");
    out.append(std::to_string(observation.received_at.nanos()));
  }
  out.append(" kind=");
  out.append(flowobs::to_string(observation.kind));
  out.append(" dir=");
  out.append(flowobs::to_string(observation.direction));
  out.append(" ev=");
  out.append(flowobs::to_string(observation.evidence));
  append_optional(out, "bytes", observation.counters.bytes);
  append_optional(out, "pkts", observation.counters.packets);
  append_optional(out, "retx", observation.counters.retransmissions);
  if (observation.covered_duration.has_value()) {
    out.append(" dur=");
    out.append(std::to_string(observation.covered_duration->nanos()));
  }
  if (observation.reported_rtt.has_value()) {
    out.append(" rtt=");
    out.append(std::to_string(observation.reported_rtt->nanos()));
  }
  if (!observation.capabilities.empty()) {
    out.append(" caps=");
    out.append(format_capabilities(observation.capabilities));
  }
  if (observation.schema_version != kObservationSchemaVersion) {
    out.append(" schema=");
    out.append(std::to_string(observation.schema_version));
  }
  return out;
}

}  // namespace fo1
}  // namespace flowobs
