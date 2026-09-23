// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// foctl - the inspection and operation tool.
//
// Every subcommand opens the journal, performs exactly one operation and closes
// it, so a shell pipeline can drive the runtime and a second process can observe
// what the first one wrote. `serve` is the only long-running command.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <flowobs/flowobs.hpp>

namespace {

using namespace flowobs;

struct Arguments final {
  std::vector<std::string> positional;
  std::map<std::string, std::string> options;
  std::vector<std::string> flags;

  [[nodiscard]] bool has(const std::string& name) const {
    return options.find(name) != options.end() ||
           std::find(flags.begin(), flags.end(), name) != flags.end();
  }
  [[nodiscard]] std::string get(const std::string& name,
                                const std::string& fallback = std::string()) const {
    const auto found = options.find(name);
    return found == options.end() ? fallback : found->second;
  }
  [[nodiscard]] std::uint64_t number(const std::string& name, std::uint64_t fallback) const {
    const std::string text = get(name);
    if (text.empty()) {
      return fallback;
    }
    return std::strtoull(text.c_str(), nullptr, 10);
  }
};

void print_usage() {
  std::cout <<
      "foctl " << version_string() << " - Flow Observatory inspection tool\n"
      "\n"
      "usage: foctl <command> [options]\n"
      "\n"
      "commands:\n"
      "  version                                     print build information\n"
      "  limits                                      print the compiled-in bounds\n"
      "  apply   --state F --script S                apply an FO1 script to the journal\n"
      "  ingest  --state F [--script S | --stdin]    alias of apply\n"
      "  query   --state F [--flow K] [--filter S]   list flows\n"
      "          [--format json|csv|text] [--pretty] [--limit N] [--offset N]\n"
      "  explain --state F --flow K                  explain one flow deterministically\n"
      "  history --state F --flow K [--limit N]      print the lifecycle history\n"
      "  attribution --state F --flow K              attribute consumption to resources\n"
      "          [--generation N] [--method endpoint|uniform|capacity]\n"
      "  export  --state F [--format json|csv|text]  export the current view\n"
      "  tick    --state F [--now NS]                apply the expiry policy\n"
      "  compact --state F                           rewrite the journal as a snapshot\n"
      "  verify  --state F                           verify journal integrity\n"
      "  stats   --state F                           print runtime counters\n"
      "  serve   --state F [--port N] [--ready-file P] [--nonce X]\n"
      "          [--read-only] [--max-connections N]\n"
      "\n"
      "exit codes: 0 success, 1 runtime failure, 2 usage error\n";
}

void print_limits(const Limits& limits) { std::cout << limits.describe(); }

std::string read_file(const std::string& path, bool& ok) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    ok = false;
    return {};
  }
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  ok = true;
  return text;
}

std::string read_stdin() {
  std::string text((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  return text;
}

bool parse_identity_text(const std::string& text, FlowId& out) {
  if (!text.empty() && text.front() == '@') {
    std::uint64_t raw = 0;
    if (!parse_hex16(std::string_view(text).substr(1), raw)) {
      return false;
    }
    out = FlowId::from_value(raw);
    return true;
  }
  out = flow_id_from_key(text);
  return true;
}

struct ToolContext final {
  EngineConfig config;
  ManualClock clock;
  std::unique_ptr<Engine> engine;
  std::filesystem::path state;

  // The evaluation instant may be pinned so that tooling output is
  // reproducible: --now=<nanoseconds> wins, then FOCTL_NOW, then the wall clock.
  bool pinned = false;
};

int open_engine(ToolContext& context) {
  const Status valid = context.config.validate();
  if (valid.failed()) {
    std::cerr << "foctl: " << valid.describe() << "\n";
    return 1;
  }
  context.engine = std::make_unique<Engine>(context.config);
  const Status opened = context.engine->open();
  if (opened.failed()) {
    std::cerr << "foctl: cannot open the journal: " << opened.describe() << "\n";
    return 1;
  }
  return 0;
}

int report(const Status& status, const char* what) {
  if (status.ok()) {
    return 0;
  }
  std::cerr << "foctl: " << what << ": " << status.describe() << "\n";
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  Arguments args;
  for (int i = 1; i < argc; ++i) {
    const std::string token = argv[i];
    if (token.rfind("--", 0) == 0) {
      const std::size_t equals = token.find('=');
      if (equals != std::string::npos) {
        args.options[token.substr(2, equals - 2)] = token.substr(equals + 1);
      } else {
        const std::string name = token.substr(2);
        // A following token that is not an option is the value.
        if (i + 1 < argc && argv[i + 1][0] != '-') {
          args.options[name] = argv[++i];
        } else {
          args.flags.push_back(name);
        }
      }
    } else {
      args.positional.push_back(token);
    }
  }

  if (args.positional.empty()) {
    print_usage();
    return 2;
  }
  const std::string command = args.positional[0];

  if (command == "version") {
    std::cout << describe_build();
    return 0;
  }
  if (command == "limits") {
    Limits limits;
    print_limits(limits);
    return 0;
  }
  if (command == "help" || command == "--help") {
    print_usage();
    return 0;
  }

  const bool known =
      command == "apply" || command == "ingest" || command == "query" || command == "explain" ||
      command == "history" || command == "lifecycle" || command == "attribution" ||
      command == "export" || command == "tick" || command == "compact" || command == "verify" ||
      command == "stats" || command == "serve";
  if (!known) {
    std::cerr << "foctl: unknown command '" << command << "'\n";
    print_usage();
    return 2;
  }

  const std::string state_path = args.get("state");
  if (state_path.empty() && command != "version" && command != "limits") {
    std::cerr << "foctl: --state is required\n";
    return 2;
  }

  ToolContext context;
  context.state = state_path;
  if (!state_path.empty()) {
    context.config.journal_path = state_path;
  }
  context.config.clock = &context.clock;
  context.clock.set(SystemClock::instance().now());
  context.config.limits.worker_threads = 0;
  context.config.limits.max_result_set = 100000;
  context.config.query.max_rows = 100000;
  if (args.has("limit")) {
    const std::uint64_t limit = args.number("limit", 1000);
    if (limit == 0) {
      std::cerr << "foctl: --limit must be greater than zero\n";
      return 2;
    }
    context.config.limits.max_result_set = static_cast<std::size_t>(limit);
    context.config.query.max_rows = static_cast<std::size_t>(limit);
  }
  if (args.has("now")) {
    context.clock.set(Timestamp::from_nanos(
        static_cast<std::int64_t>(args.number("now", 0))));
    context.pinned = true;
  } else if (const char* env = std::getenv("FOCTL_NOW"); env != nullptr && env[0] != '\0') {
    context.clock.set(Timestamp::from_nanos(
        static_cast<std::int64_t>(std::strtoll(env, nullptr, 10))));
    context.pinned = true;
  }
  if (args.has("max-flows")) {
    context.config.limits.max_flows =
        static_cast<std::uint32_t>(args.number("max-flows", 200000));
  }

  if (command == "serve") {
    // A pinned evaluation instant makes the served view reproducible for a
    // caller that supplies its own timestamps.
    if (args.has("now") || context.pinned) {
      context.config.clock = &context.clock;
    }
    context.config.restore_on_open = true;
    const int opened = open_engine(context);
    if (opened != 0) {
      return opened;
    }
    ServerOptions options;
    options.port = static_cast<std::uint16_t>(args.number("port", 0));
    options.ready_file = args.get("ready-file");
    options.ready_nonce = args.get("nonce");
    options.allow_writes = !args.has("read-only");
    options.max_connections =
        static_cast<std::uint32_t>(args.number("max-connections", 8));
    options.max_frame_bytes = context.config.limits.max_frame_bytes;
    Server server(*context.engine, options);
    const Status started = server.start_background();
    if (started.failed()) {
      std::cerr << "foctl: cannot start the ingest server: " << started.describe() << "\n";
      return 1;
    }
    std::cout << "listening " << server.endpoint() << "\n" << std::flush;
    const Status served = server.serve();
    server.stop();
    (void)server.join();
    const Status closed = context.engine->close();
    if (served.failed()) {
      return report(served, "serve");
    }
    return report(closed, "close");
  }

  const int opened = open_engine(context);
  if (opened != 0) {
    return opened;
  }

  int result = 0;
  do {
    if (command == "apply" || command == "ingest") {
      std::string text;
      bool ok = false;
      if (args.has("script")) {
        text = read_file(args.get("script"), ok);
        if (!ok) {
          std::cerr << "foctl: cannot read " << args.get("script") << "\n";
          result = 1;
          break;
        }
      } else if (args.has("stdin")) {
        text = read_stdin();
      } else {
        std::cerr << "foctl: apply requires --script or --stdin\n";
        result = 2;
        break;
      }
      Result<ScriptResult> applied =
          apply_script_text(*context.engine, text, context.config.limits.max_ingest_records,
                            &context.clock);
      if (!applied.ok()) {
        std::cerr << "foctl: " << applied.status().describe() << "\n";
        result = 1;
        break;
      }
      std::cout << "applied=" << applied.value().applied
                << " duplicates=" << applied.value().duplicates
                << " refused=" << applied.value().refused
                << " sources=" << applied.value().sources
                << " ticks=" << applied.value().ticks << "\n";
      for (const std::string& diagnostic : applied.value().diagnostics) {
        std::cerr << "foctl: " << diagnostic << "\n";
      }
      break;
    }

    if (command == "tick") {
      const Timestamp at = context.pinned ? context.clock.now() : context.engine->now();
      Result<TickReport> ticked = context.engine->tick(at);
      if (!ticked.ok()) {
        std::cerr << "foctl: " << ticked.status().describe() << "\n";
        result = 1;
        break;
      }
      std::cout << "evaluated_at=" << ticked.value().evaluated_at.nanos()
                << " examined=" << ticked.value().flows_examined
                << " transitions=" << ticked.value().transitions
                << " idle=" << ticked.value().became_idle
                << " expired=" << ticked.value().expired << "\n";
      break;
    }

    if (command == "compact") {
      const Status compacted = context.engine->compact();
      if (compacted.failed()) {
        std::cerr << "foctl: " << compacted.describe() << "\n";
        result = 1;
        break;
      }
      std::cout << "compacted\n";
      break;
    }

    if (command == "verify") {
      const Status verified = context.engine->verify_journal();
      if (verified.failed()) {
        std::cerr << "foctl: " << verified.describe() << "\n";
        result = 1;
        break;
      }
      std::cout << "verified " << state_path << "\n";
      break;
    }

    if (command == "stats") {
      std::cout << context.engine->stats().describe();
      break;
    }

    if (command == "explain") {
      FlowId flow{};
      if (!parse_identity_text(args.get("flow"), flow)) {
        std::cerr << "foctl: --flow is not a valid identity\n";
        result = 2;
        break;
      }
      Result<Explanation> explanation = context.engine->explain(flow);
      if (!explanation.ok()) {
        std::cerr << "foctl: " << explanation.status().describe() << "\n";
        result = 1;
        break;
      }
      std::cout << explanation.value().render();
      break;
    }

    if (command == "history" || command == "lifecycle") {
      FlowId flow{};
      if (!parse_identity_text(args.get("flow"), flow)) {
        std::cerr << "foctl: --flow is not a valid identity\n";
        result = 2;
        break;
      }
      Result<HistoryReport> history =
          context.engine->history(flow, static_cast<std::size_t>(args.number("limit", 0)));
      if (!history.ok()) {
        std::cerr << "foctl: " << history.status().describe() << "\n";
        result = 1;
        break;
      }
      for (const LifecycleEvent& event : history.value().events) {
        std::cout << format_timestamp(event.at) << " " << event.describe() << "\n";
      }
      std::cout << history.value().describe() << "\n";
      break;
    }

    if (command == "attribution") {
      FlowId flow{};
      if (!parse_identity_text(args.get("flow"), flow)) {
        std::cerr << "foctl: --flow is not a valid identity\n";
        result = 2;
        break;
      }
      AttributionPolicy policy = context.config.attribution;
      const std::string method = args.get("method");
      if (method == "endpoint" || method.empty()) {
        policy.method = AttributionPolicy::Method::EndpointOnly;
      } else if (method == "uniform") {
        policy.method = AttributionPolicy::Method::UniformAcrossHops;
      } else if (method == "capacity") {
        policy.method = AttributionPolicy::Method::ProportionalToCapacity;
      } else {
        std::cerr << "foctl: --method must be endpoint, uniform or capacity\n";
        result = 2;
        break;
      }
      if (args.has("allow-stale")) {
        policy.require_fresh_generation = false;
      }
      if (args.has("allow-superseded")) {
        policy.require_current_generation = false;
      }
      const GenerationId generation = GenerationId::from_value(args.number("generation", 0));
      Result<AttributionReport> attributed =
          generation.valid() ? context.engine->attribute(flow, generation, policy)
                             : context.engine->attribute_current(flow);
      if (!attributed.ok()) {
        std::cerr << "foctl: " << attributed.status().describe() << "\n";
        result = 1;
        break;
      }
      std::cout << attributed.value().describe() << "\n";
      break;
    }

    if (command == "query" || command == "export") {
      ExportOptions options;
      const std::string format = args.get("format", "json");
      if (format == "json") {
        options.format = ExportOptions::Format::Json;
      } else if (format == "csv") {
        options.format = ExportOptions::Format::Csv;
      } else if (format == "text") {
        options.format = ExportOptions::Format::Text;
      } else {
        std::cerr << "foctl: --format must be json, csv or text\n";
        result = 2;
        break;
      }
      options.pretty = args.has("pretty");
      options.query.limit = static_cast<std::size_t>(args.number("limit", 1000));
      options.query.offset = static_cast<std::size_t>(args.number("offset", 0));
      options.include_sources = !args.has("no-sources");
      options.include_generations = args.has("generations");
      options.include_anomalies = !args.has("no-anomalies");
      if (args.has("flow")) {
        FlowId flow{};
        if (!parse_identity_text(args.get("flow"), flow)) {
          std::cerr << "foctl: --flow is not a valid identity\n";
          result = 2;
          break;
        }
        options.query.flow = flow;
      }
      if (args.has("filter")) {
        FlowState state = FlowState::Unknown;
        if (!parse_flow_state(args.get("filter"), state)) {
          std::cerr << "foctl: --filter is not a known lifecycle state\n";
          result = 2;
          break;
        }
        options.query.state = state;
      }
      if (args.has("live")) {
        options.query.only_live = true;
      }
      if (args.has("restored")) {
        options.query.only_restored = true;
      }
      if (context.pinned) {
        options.evaluated_at = context.clock.now();
      }
      Result<ExportResult> exported = context.engine->export_data(options);
      if (!exported.ok()) {
        std::cerr << "foctl: " << exported.status().describe() << "\n";
        result = 1;
        break;
      }
      std::cout << exported.value().text;
      break;
    }
  } while (false);

  const Status closed = context.engine->close();
  if (closed.failed() && result == 0) {
    std::cerr << "foctl: close: " << closed.describe() << "\n";
    result = 1;
  }
  return result;
}
