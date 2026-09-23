// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Loopback ingest transport. This is a REAL proof surface: the tests start the
// server and the clients as independent operating-system processes and exchange
// frames over a TCP socket on 127.0.0.1.
//
// Framing (little endian):
//   magic  'F','O','I','1'      (4 bytes)
//   version u16                 (kWireProtocolVersion)
//   kind    u16
//   length  u32                 (payload bytes)
//   crc32c  u32                 (over kind, length and payload)
//   payload length bytes
//
// A frame whose magic, version, length or CRC is wrong is a protocol error and
// closes the connection. The transport never resynchronises by scanning.

#ifndef FLOWOBS_TRANSPORT_HPP
#define FLOWOBS_TRANSPORT_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "flowobs/engine.hpp"
#include "flowobs/error.hpp"
#include "flowobs/export.hpp"
#include "flowobs/limits.hpp"
#include "flowobs/textproto.hpp"

namespace flowobs {

enum class MessageKind : std::uint16_t {
  Hello = 1,
  Welcome = 2,
  Ingest = 3,
  IngestAck = 4,
  Query = 5,
  QueryResult = 6,
  Explain = 7,
  ExplainResult = 8,
  History = 9,
  HistoryResult = 10,
  Attribution = 11,
  AttributionResult = 12,
  StatsRequest = 13,
  StatsResult = 14,
  Tick = 15,
  TickResult = 16,
  Bye = 17,
  Error = 18,
};

FLOWOBS_PUBLIC std::string_view to_string(MessageKind kind) noexcept;

inline constexpr std::size_t kFrameHeaderBytes = 16;

struct FLOWOBS_PUBLIC Frame final {
  MessageKind kind = MessageKind::Hello;
  std::vector<std::byte> payload;
};

// Encodes a frame into `out` (appended). Returns LimitExceeded when the
// payload exceeds `max_frame_bytes`.
FLOWOBS_PUBLIC Status encode_frame(MessageKind kind, std::string_view payload,
                                   std::size_t max_frame_bytes, std::vector<std::byte>& out);

// Decodes exactly one frame header. `header` must hold kFrameHeaderBytes bytes.
struct FLOWOBS_PUBLIC FrameHeader final {
  MessageKind kind = MessageKind::Hello;
  std::uint32_t length = 0;
  std::uint32_t crc = 0;
};
FLOWOBS_PUBLIC Status decode_frame_header(const std::byte* header,
                                          std::size_t max_frame_bytes, FrameHeader& out);

FLOWOBS_PUBLIC std::uint32_t frame_crc(MessageKind kind, std::uint32_t length,
                                       const std::byte* payload, std::size_t payload_size) noexcept;

// ---------------------------------------------------------------------------
// Request / response payloads (canonical FO1 or JSON text).
// ---------------------------------------------------------------------------

struct FLOWOBS_PUBLIC ServerOptions final {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;  // 0 selects an ephemeral port
  std::uint32_t max_connections = 16;
  std::size_t max_frame_bytes = 1024u * 1024u;
  std::uint32_t backlog = 16;
  // When set, the server writes "<host>:<port> <nonce>" to this path once the
  // listening socket is bound, using an atomic rename.
  std::filesystem::path ready_file;
  std::string ready_nonce;
  // When false the server refuses every message except Hello and Bye.
  bool allow_writes = true;
};

struct FLOWOBS_PUBLIC ClientOptions final {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  std::size_t max_frame_bytes = 1024u * 1024u;
};

// Blocking, single-connection client. Owns a socket; not thread safe.
class FLOWOBS_PUBLIC Client final {
 public:
  Client() = default;
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  ~Client();

  [[nodiscard]] Status connect(const ClientOptions& options);
  [[nodiscard]] Status send(MessageKind kind, std::string_view payload);
  [[nodiscard]] Result<Frame> receive();
  [[nodiscard]] Status close();
  [[nodiscard]] bool connected() const noexcept { return socket_ != kInvalidSocket; }
  // One round trip: send a request, read the response, verify the kind.
  [[nodiscard]] Result<std::string> request(MessageKind request_kind,
                                            MessageKind expected_reply,
                                            std::string_view payload);

 private:
  static constexpr std::uintptr_t kInvalidSocket = ~std::uintptr_t{0};
  std::uintptr_t socket_ = kInvalidSocket;
  ClientOptions options_{};
};

// Multi-connection server bound to loopback. Each connection is served by one
// worker thread drawn from a bounded pool.
class FLOWOBS_PUBLIC Server final {
 public:
  Server(Engine& engine, ServerOptions options);
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  ~Server();

  // Binds and listens. After this returns the endpoint is known.
  [[nodiscard]] Status start();
  // Serves until stop() is called. Blocks the calling thread.
  [[nodiscard]] Status serve();
  // Requests shutdown and closes the listening socket. Safe from any thread and
  // from a signal handler context in the sense that it only sets a flag and
  // closes a socket.
  void stop() noexcept;
  // Waits for the serve loop and every connection thread to finish.
  [[nodiscard]] Status join();
  // start() + background serve thread; used by the CLI.
  [[nodiscard]] Status start_background();
  [[nodiscard]] std::string endpoint() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Applies one parsed FO1 script to an engine. Shared by foctl and the tests so
// that the two can never diverge.
struct FLOWOBS_PUBLIC ScriptResult final {
  std::uint64_t applied = 0;
  std::uint64_t duplicates = 0;
  std::uint64_t refused = 0;
  std::uint64_t ticks = 0;
  std::uint64_t sources = 0;
  std::vector<std::string> diagnostics;
  ErrorCode code = ErrorCode::Ok;
};

// `clock` is optional. When supplied, AT records move it, which is how a
// script pins the evaluation instant and makes freshness decisions
// reproducible.
FLOWOBS_PUBLIC ScriptResult apply_script(Engine& engine, const fo1::Script& script,
                                         ManualClock* clock = nullptr);
FLOWOBS_PUBLIC Result<ScriptResult> apply_script_text(Engine& engine, std::string_view text,
                                                      std::size_t max_records,
                                                      ManualClock* clock = nullptr);

}  // namespace flowobs

#endif  // FLOWOBS_TRANSPORT_HPP
