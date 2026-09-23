// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowobs/transport.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "flowobs/checksum.hpp"
#include "flowobs/export_json.hpp"
#include "flowobs/textproto.hpp"
#include "flowobs/version.hpp"
#include "render.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace flowobs {
namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

// Initialises Winsock exactly once per process. On POSIX this is a no-op.
void ensure_socket_layer() {
#if defined(_WIN32)
  static std::once_flag flag;
  std::call_once(flag, []() {
    WSADATA data{};
    (void)WSAStartup(MAKEWORD(2, 2), &data);
  });
#endif
}

void close_socket(NativeSocket socket) {
  if (socket == kInvalidSocket) {
    return;
  }
#if defined(_WIN32)
  ::closesocket(socket);
#else
  ::close(socket);
#endif
}

void shutdown_socket(NativeSocket socket) {
  if (socket == kInvalidSocket) {
    return;
  }
#if defined(_WIN32)
  ::shutdown(socket, SD_BOTH);
#else
  ::shutdown(socket, SHUT_RDWR);
#endif
}

bool send_all(NativeSocket socket, const void* data, std::size_t size) {
  const char* cursor = static_cast<const char*>(data);
  std::size_t remaining = size;
  while (remaining > 0) {
#if defined(_WIN32)
    const int chunk = static_cast<int>(remaining > 0x40000000u ? 0x40000000u : remaining);
    const int sent = ::send(socket, cursor, chunk, 0);
    if (sent <= 0) {
      return false;
    }
    cursor += sent;
    remaining -= static_cast<std::size_t>(sent);
#else
    const ssize_t sent = ::send(socket, cursor, remaining, 0);
    if (sent <= 0) {
      return false;
    }
    cursor += sent;
    remaining -= static_cast<std::size_t>(sent);
#endif
  }
  return true;
}

// Blocking read of exactly \`size\` bytes. Returns false on error or on a
// truncated stream; \`eof\` reports whether the peer closed cleanly at a frame
// boundary.
bool recv_exact(NativeSocket socket, void* data, std::size_t size, bool& eof) {
  char* cursor = static_cast<char*>(data);
  std::size_t remaining = size;
  eof = false;
  while (remaining > 0) {
    const int chunk = static_cast<int>(remaining > 0x40000000u ? 0x40000000u : remaining);
    const int got = ::recv(socket, cursor, chunk, 0);
    if (got == 0) {
      eof = true;
      return false;
    }
    if (got < 0) {
      return false;
    }
    cursor += got;
    remaining -= static_cast<std::size_t>(got);
  }
  return true;
}

Status io_error(const std::string& message) {
  return Status(ErrorCode::IoError, message);
}

std::string describe_socket_error() {
#if defined(_WIN32)
  return "winsock error " + std::to_string(WSAGetLastError());
#else
  return std::string(std::strerror(errno));
#endif
}

// ---------------------------------------------------------------------------
// Tiny key=value request parser used by the transport control messages.
// ---------------------------------------------------------------------------

struct FieldMap final {
  std::map<std::string, std::string> fields;

  [[nodiscard]] bool has(const std::string& key) const {
    return fields.find(key) != fields.end();
  }
  [[nodiscard]] std::string get(const std::string& key, const std::string& fallback) const {
    const auto found = fields.find(key);
    return found == fields.end() ? fallback : found->second;
  }
  [[nodiscard]] bool flag(const std::string& key, bool fallback) const {
    const auto found = fields.find(key);
    if (found == fields.end()) {
      return fallback;
    }
    return found->second == "1" || found->second == "true" || found->second == "yes";
  }
  [[nodiscard]] std::uint64_t number(const std::string& key, std::uint64_t fallback) const {
    const auto found = fields.find(key);
    if (found == fields.end()) {
      return fallback;
    }
    std::uint64_t value = 0;
    for (const char c : found->second) {
      if (c < '0' || c > '9') {
        return fallback;
      }
      value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return value;
  }
};

Status parse_fields(std::string_view text, FieldMap& out, std::size_t max_fields) {
  out.fields.clear();
  std::size_t start = 0;
  while (start < text.size()) {
    while (start < text.size() && (text[start] == ' ' || text[start] == '\n' ||
                                   text[start] == '\r' || text[start] == '\t')) {
      ++start;
    }
    if (start >= text.size()) {
      break;
    }
    const std::size_t end = text.find_first_of(" \n\r\t", start);
    const std::string_view token =
        end == std::string_view::npos ? text.substr(start) : text.substr(start, end - start);
    const std::size_t equals = token.find('=');
    if (equals == std::string_view::npos || equals == 0) {
      return invalid_argument("expected key=value");
    }
    if (out.fields.size() >= max_fields) {
      return limit_exceeded("too many request fields");
    }
    std::string key(token.substr(0, equals));
    std::string value(token.substr(equals + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }
    out.fields[key] = std::move(value);
    if (end == std::string_view::npos) {
      break;
    }
    start = end;
  }
  return make_ok();
}

Status parse_identity(const std::string& text, FlowId& out) {
  if (text.empty()) {
    return invalid_argument("identity text is empty");
  }
  if (text.front() == '@') {
    std::uint64_t raw = 0;
    if (!parse_hex16(std::string_view(text).substr(1), raw)) {
      return invalid_argument("identity is not 16 hex digits");
    }
    out = FlowId::from_value(raw);
    return make_ok();
  }
  out = flow_id_from_key(text);
  return make_ok();
}

std::string json_text(const json::Writer& writer) { return writer.str(); }

}  // namespace

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

std::string_view to_string(MessageKind kind) noexcept {
  switch (kind) {
    case MessageKind::Hello: return "hello";
    case MessageKind::Welcome: return "welcome";
    case MessageKind::Ingest: return "ingest";
    case MessageKind::IngestAck: return "ingest-ack";
    case MessageKind::Query: return "query";
    case MessageKind::QueryResult: return "query-result";
    case MessageKind::Explain: return "explain";
    case MessageKind::ExplainResult: return "explain-result";
    case MessageKind::History: return "history";
    case MessageKind::HistoryResult: return "history-result";
    case MessageKind::Attribution: return "attribution";
    case MessageKind::AttributionResult: return "attribution-result";
    case MessageKind::StatsRequest: return "stats-request";
    case MessageKind::StatsResult: return "stats-result";
    case MessageKind::Tick: return "tick";
    case MessageKind::TickResult: return "tick-result";
    case MessageKind::Bye: return "bye";
    case MessageKind::Error: return "error";
  }
  return "invalid";
}

std::uint32_t frame_crc(MessageKind kind, std::uint32_t length, const std::byte* payload,
                        std::size_t payload_size) noexcept {
  std::byte header[6];
  const std::uint16_t raw_kind = static_cast<std::uint16_t>(kind);
  header[0] = static_cast<std::byte>(raw_kind & 0xFFu);
  header[1] = static_cast<std::byte>((raw_kind >> 8u) & 0xFFu);
  for (unsigned shift = 0; shift < 32; shift += 8) {
    header[2 + shift / 8u] = static_cast<std::byte>((length >> shift) & 0xFFu);
  }
  Crc32c crc;
  crc.update(header, sizeof(header));
  if (payload != nullptr && payload_size > 0) {
    crc.update(payload, payload_size);
  }
  return crc.value();
}

Status encode_frame(MessageKind kind, std::string_view payload, std::size_t max_frame_bytes,
                    std::vector<std::byte>& out) {
  if (payload.size() > max_frame_bytes) {
    return limit_exceeded("frame payload exceeds limits.max_frame_bytes");
  }
  const std::uint32_t length = static_cast<std::uint32_t>(payload.size());
  const auto* bytes = reinterpret_cast<const std::byte*>(payload.data());
  const std::uint32_t crc = frame_crc(kind, length, bytes, payload.size());
  const std::uint16_t raw_kind = static_cast<std::uint16_t>(kind);
  out.push_back(std::byte{'F'});
  out.push_back(std::byte{'O'});
  out.push_back(std::byte{'I'});
  out.push_back(std::byte{'1'});
  out.push_back(static_cast<std::byte>(kWireProtocolVersion & 0xFFu));
  out.push_back(static_cast<std::byte>((kWireProtocolVersion >> 8u) & 0xFFu));
  out.push_back(static_cast<std::byte>(raw_kind & 0xFFu));
  out.push_back(static_cast<std::byte>((raw_kind >> 8u) & 0xFFu));
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::byte>((length >> shift) & 0xFFu));
  }
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::byte>((crc >> shift) & 0xFFu));
  }
  out.insert(out.end(), bytes, bytes + payload.size());
  return make_ok();
}

Status decode_frame_header(const std::byte* header, std::size_t max_frame_bytes,
                           FrameHeader& out) {
  if (header[0] != std::byte{'F'} || header[1] != std::byte{'O'} ||
      header[2] != std::byte{'I'} || header[3] != std::byte{'1'}) {
    return Status(ErrorCode::ProtocolError, "frame magic does not match");
  }
  const std::uint16_t version = static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(header[4]) |
      (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(header[5])) << 8u));
  if (version != kWireProtocolVersion) {
    return Status(ErrorCode::VersionMismatch, "wire protocol version does not match");
  }
  out.kind = static_cast<MessageKind>(static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(header[6]) |
      (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(header[7])) << 8u)));
  std::uint32_t length = 0;
  for (unsigned shift = 0; shift < 32; shift += 8) {
    length |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(header[8 + shift / 8u]))
              << shift;
  }
  std::uint32_t crc = 0;
  for (unsigned shift = 0; shift < 32; shift += 8) {
    crc |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(header[12 + shift / 8u]))
           << shift;
  }
  if (length > max_frame_bytes) {
    return limit_exceeded("frame length exceeds limits.max_frame_bytes");
  }
  out.length = length;
  out.crc = crc;
  return make_ok();
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

Client::~Client() { (void)close(); }

Status Client::connect(const ClientOptions& options) {
  ensure_socket_layer();
  options_ = options;
  if (options_.port == 0) {
    return invalid_argument("client requires a non-zero port");
  }
  NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) {
    return Status(ErrorCode::ConnectionRefused, describe_socket_error());
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(options_.port);
  if (::inet_pton(AF_INET, options_.host.c_str(), &address.sin_addr) != 1) {
    close_socket(socket);
    return invalid_argument("client host is not a valid IPv4 literal");
  }
  if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    const std::string message = describe_socket_error();
    close_socket(socket);
    return Status(ErrorCode::ConnectionRefused, message);
  }
  socket_ = static_cast<std::uintptr_t>(socket);
  return make_ok();
}

Status Client::send(MessageKind kind, std::string_view payload) {
  if (!connected()) {
    return Status(ErrorCode::ConnectionClosed, "client is not connected");
  }
  std::vector<std::byte> buffer;
  const Status encoded = encode_frame(kind, payload, options_.max_frame_bytes, buffer);
  if (encoded.failed()) {
    return encoded;
  }
  if (!send_all(static_cast<NativeSocket>(socket_), buffer.data(), buffer.size())) {
    return Status(ErrorCode::ConnectionClosed, describe_socket_error());
  }
  return make_ok();
}

Result<Frame> Client::receive() {
  if (!connected()) {
    return Status(ErrorCode::ConnectionClosed, "client is not connected");
  }
  std::byte header[kFrameHeaderBytes];
  bool eof = false;
  if (!recv_exact(static_cast<NativeSocket>(socket_), header, sizeof(header), eof)) {
    (void)close();
    return Status(ErrorCode::ConnectionClosed,
                  eof ? "peer closed the connection" : describe_socket_error());
  }
  FrameHeader decoded;
  const Status status = decode_frame_header(header, options_.max_frame_bytes, decoded);
  if (status.failed()) {
    (void)close();
    return status;
  }
  Frame frame;
  frame.kind = decoded.kind;
  frame.payload.resize(decoded.length);
  if (decoded.length > 0 &&
      !recv_exact(static_cast<NativeSocket>(socket_), frame.payload.data(), decoded.length, eof)) {
    (void)close();
    return Status(ErrorCode::ConnectionClosed, "frame payload was truncated");
  }
  const std::uint32_t crc =
      frame_crc(decoded.kind, decoded.length, frame.payload.data(), frame.payload.size());
  if (crc != decoded.crc) {
    (void)close();
    return Status(ErrorCode::ProtocolError, "frame checksum mismatch");
  }
  return frame;
}

Result<std::string> Client::request(MessageKind request_kind, MessageKind expected_reply,
                                    std::string_view payload) {
  const Status status = send(request_kind, payload);
  if (status.failed()) {
    return status;
  }
  Result<Frame> frame = receive();
  if (!frame.ok()) {
    return frame.status();
  }
  if (frame.value().kind == MessageKind::Error) {
    return Status(ErrorCode::ProtocolError,
                  std::string(reinterpret_cast<const char*>(frame.value().payload.data()),
                              frame.value().payload.size()));
  }
  if (frame.value().kind != expected_reply) {
    return Status(ErrorCode::ProtocolError,
                  "unexpected reply kind " + std::string(to_string(frame.value().kind)));
  }
  return std::string(reinterpret_cast<const char*>(frame.value().payload.data()),
                     frame.value().payload.size());
}

Status Client::close() {
  if (!connected()) {
    return make_ok();
  }
  const NativeSocket socket = static_cast<NativeSocket>(socket_);
  socket_ = kInvalidSocket;
  close_socket(socket);
  return make_ok();
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

struct Server::Impl final {
  Engine* engine = nullptr;
  ServerOptions options{};
  NativeSocket listener = kInvalidSocket;
  std::uint16_t bound_port = 0;
  std::atomic<bool> stop{false};
  std::atomic<bool> started{false};

  std::mutex mutex;
  std::condition_variable cv;
  std::deque<NativeSocket> pending;
  std::vector<NativeSocket> active;
  std::vector<std::thread> handlers;
  std::thread acceptor;

  [[nodiscard]] bool send_reply(NativeSocket socket, MessageKind kind, const std::string& payload) {
    std::vector<std::byte> buffer;
    if (encode_frame(kind, payload, options.max_frame_bytes, buffer).failed()) {
      return false;
    }
    return send_all(socket, buffer.data(), buffer.size());
  }

  // Handles one request. Returns false when the connection should be closed.
  bool dispatch(NativeSocket socket, MessageKind kind, const std::string& payload) {
    switch (kind) {
      case MessageKind::Hello:
        return send_reply(socket, MessageKind::Welcome,
                          std::string("flow-observatory/") + std::string(version_string()));
      case MessageKind::Bye:
        // "shutdown" is an explicit request to stop serving. It is only
        // reachable over a loopback connection the operator established.
        if (payload.rfind("shutdown", 0) == 0) {
          stop.store(true, std::memory_order_relaxed);
          const NativeSocket listening = listener;
          listener = kInvalidSocket;
          close_socket(listening);
        }
        return false;
      case MessageKind::Ingest: {
        if (!options.allow_writes) {
          return send_reply(socket, MessageKind::Error, "endpoint is read-only");
        }
        Result<ScriptResult> applied =
            apply_script_text(*engine, payload, engine->limits().max_ingest_records);
        if (!applied.ok()) {
          return send_reply(socket, MessageKind::Error, applied.status().describe());
        }
        std::string out;
        out.append("applied=");
        out.append(std::to_string(applied.value().applied));
        out.append(" duplicates=");
        out.append(std::to_string(applied.value().duplicates));
        out.append(" refused=");
        out.append(std::to_string(applied.value().refused));
        out.append(" ticks=");
        out.append(std::to_string(applied.value().ticks));
        out.append(" sources=");
        out.append(std::to_string(applied.value().sources));
        for (const std::string& diagnostic : applied.value().diagnostics) {
          out.append(" | ");
          out.append(diagnostic);
        }
        return send_reply(socket, MessageKind::IngestAck, out);
      }
      case MessageKind::Query: {
        FieldMap fields;
        if (parse_fields(payload, fields, 32).failed()) {
          return send_reply(socket, MessageKind::Error, "malformed query payload");
        }
        FlowQuery request;
        request.limit = static_cast<std::size_t>(fields.number("limit", 100));
        request.offset = static_cast<std::size_t>(fields.number("offset", 0));
        request.only_live = fields.flag("only_live", false);
        request.only_restored = fields.flag("only_restored", false);
        request.include_terminal = fields.flag("include_terminal", true);
        if (fields.has("state")) {
          FlowState state = FlowState::Unknown;
          if (!parse_flow_state(fields.get("state", ""), state)) {
            return send_reply(socket, MessageKind::Error, "unknown state filter");
          }
          request.state = state;
        }
        if (fields.has("flow")) {
          FlowId flow{};
          if (parse_identity(fields.get("flow", ""), flow).failed()) {
            return send_reply(socket, MessageKind::Error, "malformed flow identity");
          }
          request.flow = flow;
        }
        ExportOptions export_options;
        export_options.query = request;
        export_options.format = ExportOptions::Format::Json;
        export_options.pretty = fields.flag("pretty", false);
        export_options.include_sources = fields.flag("include_sources", true);
        export_options.include_generations = fields.flag("include_generations", false);
        export_options.include_anomalies = fields.flag("include_anomalies", true);
        Result<ExportResult> exported = engine->export_data(export_options);
        if (!exported.ok()) {
          return send_reply(socket, MessageKind::Error, exported.status().describe());
        }
        return send_reply(socket, MessageKind::QueryResult, exported.value().text);
      }
      case MessageKind::Explain: {
        FieldMap fields;
        if (parse_fields(payload, fields, 8).failed() || !fields.has("flow")) {
          return send_reply(socket, MessageKind::Error, "explain requires flow=<identity>");
        }
        FlowId flow{};
        if (parse_identity(fields.get("flow", ""), flow).failed()) {
          return send_reply(socket, MessageKind::Error, "malformed flow identity");
        }
        Result<Explanation> explanation = engine->explain(flow);
        if (!explanation.ok()) {
          return send_reply(socket, MessageKind::Error, explanation.status().describe());
        }
        return send_reply(socket, MessageKind::ExplainResult, explanation.value().render());
      }
      case MessageKind::History: {
        FieldMap fields;
        if (parse_fields(payload, fields, 8).failed() || !fields.has("flow")) {
          return send_reply(socket, MessageKind::Error, "history requires flow=<identity>");
        }
        FlowId flow{};
        if (parse_identity(fields.get("flow", ""), flow).failed()) {
          return send_reply(socket, MessageKind::Error, "malformed flow identity");
        }
        Result<HistoryReport> history =
            engine->history(flow, static_cast<std::size_t>(fields.number("limit", 64)));
        if (!history.ok()) {
          return send_reply(socket, MessageKind::Error, history.status().describe());
        }
        json::Writer writer(false);
        writer.begin_object();
        writer.field("flow", flowobs::to_string(flow));
        writer.field("returned", static_cast<std::uint64_t>(history.value().returned));
        writer.field("dropped", history.value().events_dropped);
        writer.field("truncated", history.value().truncated);
        writer.key("events");
        writer.begin_array();
        for (const LifecycleEvent& event : history.value().events) {
          writer.begin_object();
          writer.field("at", event.at.nanos());
          writer.field("observed_at", event.observed_at.nanos());
          writer.field("generation", event.generation.generation().value());
          writer.field("from", flowobs::to_string(event.from));
          writer.field("to", flowobs::to_string(event.to));
          writer.field("cause", flowobs::to_string(event.cause));
          writer.field("source", flowobs::to_string(event.source));
          writer.field("detail", event.detail);
          writer.end_object();
        }
        writer.end_array();
        writer.end_object();
        return send_reply(socket, MessageKind::HistoryResult, json_text(writer));
      }
      case MessageKind::Attribution: {
        FieldMap fields;
        if (parse_fields(payload, fields, 8).failed() || !fields.has("flow")) {
          return send_reply(socket, MessageKind::Error, "attribution requires flow=<identity>");
        }
        FlowId flow{};
        if (parse_identity(fields.get("flow", ""), flow).failed()) {
          return send_reply(socket, MessageKind::Error, "malformed flow identity");
        }
        const GenerationId generation =
            GenerationId::from_value(fields.number("generation", 0));
        const Result<AttributionReport> report =
            generation.valid()
                ? engine->attribute(flow, generation, engine->config().attribution)
                : engine->attribute_current(flow);
        if (!report.ok()) {
          return send_reply(socket, MessageKind::Error, report.status().describe());
        }
        json::Writer writer(false);
        writer.begin_object();
        writer.field("flow", flowobs::to_string(flow));
        writer.field("generation", report.value().generation.value());
        writer.field("status", flowobs::to_string(report.value().status));
        writer.field("status_detail", report.value().status_detail);
        writer.key("total");
        writer.begin_object();
        writer.field("bytes", report.value().total.bytes);
        writer.field("packets", report.value().total.packets);
        writer.field("retransmissions", report.value().total.retransmissions);
        writer.field("unknown_intervals", report.value().total.unknown_intervals);
        writer.field("exact", report.value().total.is_exact());
        writer.end_object();
        writer.key("targets");
        writer.begin_array();
        for (const AttributedTarget& target : report.value().targets) {
          writer.begin_object();
          writer.field("index", static_cast<std::uint64_t>(target.index));
          writer.field("bytes", target.consumption.bytes);
          writer.field("packets", target.consumption.packets);
          writer.field("retransmissions", target.consumption.retransmissions);
          writer.field("link", flowobs::to_string(target.link));
          writer.field("queue", flowobs::to_string(target.queue));
          writer.field("endpoint", flowobs::to_string(target.endpoint));
          if (target.capacity_units.has_value()) {
            writer.field("capacity_units", *target.capacity_units);
          } else {
            writer.field_null("capacity_units");
          }
          writer.end_object();
        }
        writer.end_array();
        writer.key("notes");
        writer.begin_array();
        for (const std::string& note : report.value().notes) {
          writer.value_string(note);
        }
        writer.end_array();
        writer.end_object();
        return send_reply(socket, MessageKind::AttributionResult, json_text(writer));
      }
      case MessageKind::StatsRequest:
        return send_reply(socket, MessageKind::StatsResult, engine->stats().describe());
      case MessageKind::Tick: {
        FieldMap fields;
        if (parse_fields(payload, fields, 4).failed()) {
          return send_reply(socket, MessageKind::Error, "malformed tick payload");
        }
        Timestamp at = engine->now();
        if (fields.has("now")) {
          at = Timestamp::from_nanos(static_cast<std::int64_t>(fields.number("now", 0)));
        }
        Result<TickReport> ticked = engine->tick(at);
        if (!ticked.ok()) {
          return send_reply(socket, MessageKind::Error, ticked.status().describe());
        }
        std::string out;
        out.append("evaluated_at=");
        out.append(std::to_string(ticked.value().evaluated_at.nanos()));
        out.append(" examined=");
        out.append(std::to_string(ticked.value().flows_examined));
        out.append(" transitions=");
        out.append(std::to_string(ticked.value().transitions));
        out.append(" idle=");
        out.append(std::to_string(ticked.value().became_idle));
        out.append(" expired=");
        out.append(std::to_string(ticked.value().expired));
        return send_reply(socket, MessageKind::TickResult, out);
      }
      default:
        return send_reply(socket, MessageKind::Error, "unsupported request kind");
    }
  }

  void handle_connection(NativeSocket socket) {
    for (;;) {
      std::byte header[kFrameHeaderBytes];
      bool eof = false;
      if (!recv_exact(socket, header, sizeof(header), eof)) {
        break;
      }
      FrameHeader decoded;
      const Status status = decode_frame_header(header, options.max_frame_bytes, decoded);
      if (status.failed()) {
        (void)send_reply(socket, MessageKind::Error, status.describe());
        break;
      }
      std::string payload(decoded.length, '\0');
      if (decoded.length > 0 &&
          !recv_exact(socket, payload.data(), decoded.length, eof)) {
        break;
      }
      const std::uint32_t crc =
          frame_crc(decoded.kind, decoded.length,
                    reinterpret_cast<const std::byte*>(payload.data()), payload.size());
      if (crc != decoded.crc) {
        (void)send_reply(socket, MessageKind::Error, "frame checksum mismatch");
        break;
      }
      if (!dispatch(socket, decoded.kind, payload)) {
        break;
      }
      if (stop.load(std::memory_order_relaxed)) {
        break;
      }
    }
    close_socket(socket);
    {
      std::unique_lock<std::mutex> lock(mutex);
      active.erase(std::remove(active.begin(), active.end(), socket), active.end());
    }
  }

  void handler_loop() {
    for (;;) {
      NativeSocket socket = kInvalidSocket;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this]() { return stop.load(std::memory_order_relaxed) || !pending.empty(); });
        if (pending.empty()) {
          if (stop.load(std::memory_order_relaxed)) {
            return;
          }
          continue;
        }
        socket = pending.front();
        pending.pop_front();
        active.push_back(socket);
      }
      handle_connection(socket);
      if (stop.load(std::memory_order_relaxed) && pending.empty()) {
        std::unique_lock<std::mutex> lock(mutex);
        if (pending.empty()) {
          return;
        }
      }
    }
  }
};

Server::Server(Engine& engine, ServerOptions options) : impl_(std::make_unique<Impl>()) {
  impl_->engine = &engine;
  impl_->options = std::move(options);
}

Server::~Server() {
  stop();
  (void)join();
}

Status Server::start() {
  ensure_socket_layer();
  if (impl_->started.load(std::memory_order_relaxed)) {
    return Status(ErrorCode::AlreadyRunning, "server is already started");
  }
  NativeSocket listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == kInvalidSocket) {
    return Status(ErrorCode::IoError, describe_socket_error());
  }
  int reuse = 1;
  (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                     static_cast<socklen_t>(sizeof(reuse)));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(impl_->options.port);
  if (::inet_pton(AF_INET, impl_->options.host.c_str(), &address.sin_addr) != 1) {
    close_socket(listener);
    return invalid_argument("server host is not a valid IPv4 literal");
  }
  if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    const std::string message = describe_socket_error();
    close_socket(listener);
    return Status(ErrorCode::AddressInUse, message);
  }
  if (::listen(listener, static_cast<int>(impl_->options.backlog)) != 0) {
    const std::string message = describe_socket_error();
    close_socket(listener);
    return Status(ErrorCode::IoError, message);
  }
  sockaddr_in bound{};
  socklen_t size = static_cast<socklen_t>(sizeof(bound));
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &size) != 0) {
    const std::string message = describe_socket_error();
    close_socket(listener);
    return Status(ErrorCode::IoError, message);
  }
  impl_->listener = listener;
  impl_->bound_port = ntohs(bound.sin_port);
  impl_->stop.store(false, std::memory_order_relaxed);
  impl_->started.store(true, std::memory_order_relaxed);

  for (std::uint32_t i = 0; i < impl_->options.max_connections; ++i) {
    impl_->handlers.emplace_back([this]() { impl_->handler_loop(); });
  }

  if (!impl_->options.ready_file.empty()) {
    std::filesystem::path temp = impl_->options.ready_file;
    temp += ".tmp";
    std::string text = endpoint();
    if (!impl_->options.ready_nonce.empty()) {
      text.push_back(' ');
      text.append(impl_->options.ready_nonce);
    }
    text.push_back('\n');
    {
      std::FILE* file = std::fopen(temp.string().c_str(), "wb");
      if (file == nullptr) {
        return io_error("cannot write ready file");
      }
      const std::size_t written = std::fwrite(text.data(), 1, text.size(), file);
      std::fflush(file);
      std::fclose(file);
      if (written != text.size()) {
        return io_error("cannot write ready file");
      }
    }
    std::error_code ec;
    std::filesystem::remove(impl_->options.ready_file, ec);
    ec.clear();
    std::filesystem::rename(temp, impl_->options.ready_file, ec);
    if (ec) {
      return io_error("cannot publish ready file: " + ec.message());
    }
  }
  return make_ok();
}

std::string Server::endpoint() const {
  std::string out = impl_->options.host;
  out.push_back(':');
  out.append(std::to_string(impl_->bound_port));
  return out;
}

Status Server::start_background() { return start(); }

Status Server::serve() {
  if (!impl_->started.load(std::memory_order_relaxed)) {
    const Status started_status = start();
    if (started_status.failed()) {
      return started_status;
    }
  }
  for (;;) {
    sockaddr_in peer{};
    socklen_t size = static_cast<socklen_t>(sizeof(peer));
    const NativeSocket connection =
        ::accept(impl_->listener, reinterpret_cast<sockaddr*>(&peer), &size);
    if (connection == kInvalidSocket) {
      if (impl_->stop.load(std::memory_order_relaxed)) {
        return make_ok();
      }
      continue;
    }
    if (impl_->stop.load(std::memory_order_relaxed)) {
      close_socket(connection);
      return make_ok();
    }
    bool refused = false;
    {
      std::unique_lock<std::mutex> lock(impl_->mutex);
      if (impl_->pending.size() + impl_->active.size() >= impl_->options.max_connections) {
        refused = true;
      } else {
        impl_->pending.push_back(connection);
      }
    }
    if (refused) {
      // The connection limit is enforced by refusing the socket outright,
      // never by queueing without bound.
      close_socket(connection);
      continue;
    }
    impl_->cv.notify_one();
  }
}

void Server::stop() noexcept {
  if (!impl_) {
    return;
  }
  impl_->stop.store(true, std::memory_order_relaxed);
  const NativeSocket listener = impl_->listener;
  impl_->listener = kInvalidSocket;
  close_socket(listener);
  {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    for (const NativeSocket socket : impl_->active) {
      shutdown_socket(socket);
    }
    for (const NativeSocket socket : impl_->pending) {
      close_socket(socket);
    }
    impl_->pending.clear();
  }
  impl_->cv.notify_all();
}

Status Server::join() {
  if (impl_->acceptor.joinable()) {
    impl_->acceptor.join();
  }
  for (std::thread& handler : impl_->handlers) {
    if (handler.joinable()) {
      handler.join();
    }
  }
  impl_->handlers.clear();
  impl_->started.store(false, std::memory_order_relaxed);
  return make_ok();
}

// ---------------------------------------------------------------------------
// Script application
// ---------------------------------------------------------------------------

ScriptResult apply_script(Engine& engine, const fo1::Script& script, ManualClock* clock) {
  ScriptResult result;
  for (const fo1::Record& record : script.records) {
    switch (record.kind) {
      case fo1::Record::Kind::Comment:
        break;
      case fo1::Record::Kind::Source: {
        const Status status = engine.register_source(record.source);
        if (status.failed()) {
          result.diagnostics.push_back("SRC " + record.source.canonical_key + ": " +
                                       status.describe());
          if (result.code == ErrorCode::Ok) {
            result.code = status.code();
          }
        } else {
          ++result.sources;
        }
        break;
      }
      case fo1::Record::Kind::At: {
        if (clock != nullptr) {
          clock->set(record.now);
        }
        break;
      }
      case fo1::Record::Kind::Observation: {
        const Result<IngestOutcome> applied = engine.submit(record.observation);
        if (!applied.ok()) {
          ++result.refused;
          result.diagnostics.push_back("OBS line " + std::to_string(record.line) + ": " +
                                       applied.status().describe());
          if (result.code == ErrorCode::Ok) {
            result.code = applied.status().code();
          }
          break;
        }
        switch (applied.value().disposition) {
          case IngestDisposition::Applied: ++result.applied; break;
          case IngestDisposition::Duplicate: ++result.duplicates; break;
          case IngestDisposition::Queued: ++result.applied; break;
          case IngestDisposition::Refused:
          case IngestDisposition::DroppedQueueFull:
            ++result.refused;
            result.diagnostics.push_back("OBS line " + std::to_string(record.line) + ": " +
                                         applied.value().describe());
            break;
        }
        break;
      }
      case fo1::Record::Kind::Tick: {
        const Timestamp at = record.now.known() ? record.now
                                                : (clock != nullptr ? clock->now() : engine.now());
        const Result<TickReport> ticked = engine.tick(at);
        if (!ticked.ok()) {
          result.diagnostics.push_back("TICK: " + ticked.status().describe());
          if (result.code == ErrorCode::Ok) {
            result.code = ticked.status().code();
          }
        } else {
          ++result.ticks;
        }
        break;
      }
      case fo1::Record::Kind::Freeze:
        engine.freeze();
        break;
    }
  }
  return result;
}

Result<ScriptResult> apply_script_text(Engine& engine, std::string_view text,
                                       std::size_t max_records, ManualClock* clock) {
  Result<fo1::Script> script = fo1::parse_script(text, max_records);
  if (!script.ok()) {
    return script.status();
  }
  return apply_script(engine, script.value(), clock);
}

}  // namespace flowobs
