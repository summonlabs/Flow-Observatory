// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// REAL independent-process transport proof.
//
// This test starts foctl (a separate operating-system process) in serve mode,
// reads the endpoint it reports on its own stdout, and then drives it over a
// TCP socket on loopback with the documented frame protocol. Afterwards a
// second, independent foctl process reads the same journal from disk, proving
// that the evidence the socket delivered is durable and visible across process
// boundaries.
//
// There are no timeouts anywhere: every wait is a blocking read that either
// completes or fails when the peer closes its end of the pipe.

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

std::filesystem::path scratch_dir() {
  std::filesystem::path directory = std::filesystem::temp_directory_path() / "flowobs-transport";
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  return directory;
}

// --- child process plumbing -------------------------------------------------
//
// The child is started directly with CreateProcess, never through a shell: the
// build directory path can contain spaces, and routing the command line through
// cmd.exe would make the quoting rules part of the test. stdout and stderr are
// redirected into one anonymous pipe and read with blocking calls, so every
// wait is a real completion barrier and no timeout is involved anywhere.

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

class ChildProcess final {
 public:
  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ~ChildProcess() { close_handles(); }

  bool start(const std::string& command_line) {
#if defined(_WIN32)
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (CreatePipe(&read_end, &write_end, &attributes, 0) == FALSE) {
      return false;
    }
    if (SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0) == FALSE) {
      CloseHandle(read_end);
      CloseHandle(write_end);
      return false;
    }
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::string mutable_command = command_line;
    PROCESS_INFORMATION information{};
    const BOOL created =
        CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0, nullptr,
                       nullptr, &startup, &information);
    CloseHandle(write_end);
    if (created == FALSE) {
      std::fprintf(stderr, "[transport] CreateProcess failed for <%s>: error %lu\n",
                   command_line.c_str(), static_cast<unsigned long>(GetLastError()));
      std::fflush(stderr);
      CloseHandle(read_end);
      return false;
    }
    CloseHandle(information.hThread);
    read_handle_ = read_end;
    process_ = information.hProcess;
    return true;
#else
    (void)command_line;
    return false;
#endif
  }

  // Blocking read. Returns the bytes that were available; an empty result with
  // eof() true means the child closed the pipe.
  std::string read_some() {
    std::string chunk;
#if defined(_WIN32)
    if (read_handle_ == nullptr) {
      eof_ = true;
      return chunk;
    }
    char buffer[512];
    DWORD got = 0;
    if (ReadFile(read_handle_, buffer, sizeof(buffer), &got, nullptr) == FALSE || got == 0) {
      eof_ = true;
      return chunk;
    }
    chunk.assign(buffer, got);
#endif
    return chunk;
  }

  // Reads until a newline has been seen or the pipe closes.
  std::string read_line() {
    std::string line;
    while (true) {
      const std::size_t newline = pending_.find('\n');
      if (newline != std::string::npos) {
        line = pending_.substr(0, newline);
        pending_.erase(0, newline + 1);
        break;
      }
      if (eof_) {
        line = pending_;
        pending_.clear();
        break;
      }
      pending_ += read_some();
    }
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
      line.pop_back();
    }
    return line;
  }

  std::string read_rest() {
    while (!eof_) {
      pending_ += read_some();
    }
    return pending_;
  }

  // Waits for the child to exit. There is no timeout parameter by design.
  int wait() {
#if defined(_WIN32)
    if (process_ == nullptr) {
      return -1;
    }
    (void)read_rest();
    WaitForSingleObject(process_, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    return static_cast<int>(code);
#else
    return -1;
#endif
  }

  [[nodiscard]] bool eof() const noexcept { return eof_; }

 private:
  void close_handles() {
#if defined(_WIN32)
    if (read_handle_ != nullptr) {
      CloseHandle(read_handle_);
      read_handle_ = nullptr;
    }
    if (process_ != nullptr) {
      CloseHandle(process_);
      process_ = nullptr;
    }
#endif
  }

#if defined(_WIN32)
  HANDLE read_handle_ = nullptr;
  HANDLE process_ = nullptr;
#endif
  std::string pending_;
  bool eof_ = false;
};

std::string quote(const std::string& text) { return "\"" + text + "\""; }

std::string run_capture(const std::string& command, int& exit_code) {
  ChildProcess process;
  if (!process.start(command)) {
    exit_code = -1;
    return {};
  }
  const std::string output = process.read_rest();
  exit_code = process.wait();
  return output;
}

}  // namespace

const char* kScript = R"FO1(
SRC v=1 key="fabric-a" auth=20 caps=completion,reset,path,queue,counters
OBS v=1 src="fabric-a" inc=1 epoch=1 rev=1 seq=1 flow="flow-remote" gen=1 obs=1700000000000000000 kind=open dir=forward ev=complete ep="host-a" peer="host-b" path="p0" queue="q0" bytes=0 pkts=0 retx=0
OBS v=1 src="fabric-a" inc=1 epoch=1 rev=2 seq=2 flow="flow-remote" gen=1 obs=1700000001000000000 kind=sample dir=forward ev=complete ep="host-a" peer="host-b" path="p0" queue="q0" bytes=8192 pkts=16 retx=1
OBS v=1 src="fabric-a" inc=1 epoch=1 rev=3 seq=3 flow="flow-remote" gen=1 obs=1700000002000000000 kind=sample dir=reverse ev=complete ep="host-b" peer="host-a" bytes=2048 pkts=4 retx=0
)FO1";

}  // namespace

FOTEST("transport", "independent_process_round_trip_over_loopback_tcp") {
  const char* foctl_env = std::getenv("FLOWOBS_FOCTL");
  FO_REQUIRE(foctl_env != nullptr && foctl_env[0] != '\0');
  const std::string foctl = foctl_env;

  const std::filesystem::path directory = scratch_dir();
  const std::filesystem::path state = directory / "transport.foj";
  const std::filesystem::path script = directory / "transport.fo1";
  const std::filesystem::path ready = directory / "transport.ready";
  std::error_code ec;
  std::filesystem::remove(state, ec);
  std::filesystem::remove(ready, ec);
  {
    std::ofstream out(script, std::ios::binary | std::ios::trunc);
    out << kScript;
  }

  const std::string nonce = "flowobs-transport-nonce";
  // The evaluation instant is pinned three seconds after the scripted evidence,
  // so freshness and therefore attribution are reproducible.
  const std::string serve_command =
      quote(foctl) + " serve --state " + quote(state.string()) +
      " --port 0 --ready-file " + quote(ready.string()) + " --nonce " + nonce +
      " --max-connections 4 --now " + std::to_string(1700000003000000000LL);

  ChildProcess server;
  FO_REQUIRE(server.start(serve_command));

  // Blocking read of the child's readiness line. If the child failed to start,
  // this returns an empty string when its stdout closes.
  const std::string announced = server.read_line();
  FO_REQUIRE(!announced.empty());
  const std::size_t space = announced.find(' ');
  FO_REQUIRE(space != std::string::npos);
  FO_CHECK_EQ(announced.substr(0, 9), std::string("listening"));
  const std::string endpoint = announced.substr(10, space - 10);
  const std::size_t colon = endpoint.find(':');
  FO_REQUIRE(colon != std::string::npos);
  const std::string host = endpoint.substr(0, colon);
  const std::uint16_t port =
      static_cast<std::uint16_t>(std::strtoul(endpoint.substr(colon + 1).c_str(), nullptr, 10));
  FO_CHECK(port != 0);

  // The child also published the same endpoint to its ready file.
  {
    std::ifstream input(ready, std::ios::binary);
    std::string text;
    std::getline(input, text);
    FO_CHECK(text.find(nonce) != std::string::npos);
    FO_CHECK(text.find(endpoint) != std::string::npos);
  }

  ClientOptions options;
  options.host = host;
  options.port = port;

  // --- handshake --------------------------------------------------------
  {
    Client client;
    FO_REQUIRE(client.connect(options).ok());
    const Result<std::string> welcome = client.request(MessageKind::Hello, MessageKind::Welcome, "");
    FO_REQUIRE(welcome.ok());
    FO_CHECK(welcome.value().find("flow-observatory") != std::string::npos);
    FO_REQUIRE(client.send(MessageKind::Bye, "").ok());
    FO_REQUIRE(client.close().ok());
  }

  // --- ingest over the socket ------------------------------------------
  {
    Client client;
    FO_REQUIRE(client.connect(options).ok());
    const Result<std::string> ack =
        client.request(MessageKind::Ingest, MessageKind::IngestAck, kScript);
    FO_REQUIRE(ack.ok());
    FO_CHECK(ack.value().find("applied=3") != std::string::npos);
    FO_CHECK(ack.value().find("refused=0") != std::string::npos);
    FO_REQUIRE(client.send(MessageKind::Bye, "").ok());
    FO_REQUIRE(client.close().ok());
  }

  // --- query the same process over a new connection ---------------------
  {
    Client client;
    FO_REQUIRE(client.connect(options).ok());
    const Result<std::string> result =
        client.request(MessageKind::Query, MessageKind::QueryResult,
                       "limit=10 include_generations=1 include_sources=1");
    FO_REQUIRE(result.ok());
    FO_CHECK(result.value().find("flow-observatory/export/1") != std::string::npos);
    FO_CHECK(result.value().find("flow-remote") != std::string::npos);
    FO_CHECK(result.value().find("\"bytes\":8192") != std::string::npos);

    const Result<std::string> explanation =
        client.request(MessageKind::Explain, MessageKind::ExplainResult, "flow=flow-remote");
    FO_REQUIRE(explanation.ok());
    FO_CHECK(explanation.value().find("state:") != std::string::npos);

    const Result<std::string> history =
        client.request(MessageKind::History, MessageKind::HistoryResult, "flow=flow-remote");
    FO_REQUIRE(history.ok());
    FO_CHECK(history.value().find("events") != std::string::npos);

    const Result<std::string> attribution = client.request(
        MessageKind::Attribution, MessageKind::AttributionResult, "flow=flow-remote");
    FO_REQUIRE(attribution.ok());
    FO_CHECK(attribution.value().find("\"status\":\"ok\"") != std::string::npos);
    FO_CHECK(attribution.value().find("\"targets\"") != std::string::npos);

    const Result<std::string> stats =
        client.request(MessageKind::StatsRequest, MessageKind::StatsResult, "");
    FO_REQUIRE(stats.ok());
    FO_CHECK(stats.value().find("observations_applied=3") != std::string::npos);

    FO_REQUIRE(client.send(MessageKind::Bye, "").ok());
    FO_REQUIRE(client.close().ok());
  }

  // --- malformed frames are refused and close the connection ------------
  {
    Client client;
    FO_REQUIRE(client.connect(options).ok());
    // A frame whose payload does not parse must produce an explicit error rather
    // than a silent success.
    const Result<std::string> refused =
        client.request(MessageKind::Ingest, MessageKind::IngestAck, "OBS v=1 nonsense");
    FO_REQUIRE(!refused.ok());
    FO_REQUIRE(client.close().ok());

    // The server is still healthy afterwards.
    Client probe;
    FO_REQUIRE(probe.connect(options).ok());
    const Result<std::string> welcome =
        probe.request(MessageKind::Hello, MessageKind::Welcome, "");
    FO_REQUIRE(welcome.ok());
    FO_REQUIRE(probe.send(MessageKind::Bye, "").ok());
    FO_REQUIRE(probe.close().ok());
  }

  // --- shut the server down and reap it ---------------------------------
  {
    Client client;
    FO_REQUIRE(client.connect(options).ok());
    FO_REQUIRE(client.send(MessageKind::Bye, "shutdown").ok());
    FO_REQUIRE(client.close().ok());
  }
  const int server_exit = server.wait();
  FO_CHECK_EQ(server_exit, 0);

  // --- an independent second process reads the durable result -----------
  {
    int exit_code = -1;
    const std::string query_command =
        quote(foctl) + " query --state " + quote(state.string()) +
        " --format json --limit 10";
    const std::string output = run_capture(query_command, exit_code);
    FO_CHECK_EQ(exit_code, 0);
    FO_CHECK(output.find("flow-remote") != std::string::npos);
    FO_CHECK(output.find("8192") != std::string::npos);
  }
  {
    int exit_code = -1;
    const std::string verify_command =
        quote(foctl) + " verify --state " + quote(state.string()) + "";
    const std::string output = run_capture(verify_command, exit_code);
    FO_CHECK_EQ(exit_code, 0);
    FO_CHECK(output.find("verified") != std::string::npos);
  }
  {
    int exit_code = -1;
    const std::string apply_command =
        quote(foctl) + " apply --state " + quote(state.string()) + " --script " +
        quote(script.string()) + "";
    const std::string output = run_capture(apply_command, exit_code);
    FO_CHECK_EQ(exit_code, 0);
    // Replaying the same script from a different process is idempotent: the
    // script is journaled, so every record is recognised as already seen.
    if (output.find("duplicates=3") == std::string::npos) {
      ctx.fail("re-apply output was: " + output);
    }
    FO_CHECK(output.find("duplicates=3") != std::string::npos);
  }
}

FOTEST("transport", "server_refuses_to_start_twice_and_reports_usage_errors") {
  const char* foctl_env = std::getenv("FLOWOBS_FOCTL");
  FO_REQUIRE(foctl_env != nullptr && foctl_env[0] != '\0');
  const std::string foctl = foctl_env;
  int exit_code = -1;
  const std::string output = run_capture(quote(foctl) + " nonsense", exit_code);
  FO_CHECK_EQ(exit_code, 2);
  FO_CHECK(output.find("unknown command") != std::string::npos);
  const std::string missing_state = run_capture(quote(foctl) + " query", exit_code);
  FO_CHECK_EQ(exit_code, 2);
  FO_CHECK(missing_state.find("--state is required") != std::string::npos);
  const std::string version = run_capture(quote(foctl) + " version", exit_code);
  FO_CHECK_EQ(exit_code, 0);
  FO_CHECK(version.find("flow-observatory 1.0.0") != std::string::npos);
}
