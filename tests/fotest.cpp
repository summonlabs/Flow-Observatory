// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fotest.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>

#if defined(_MSC_VER)
#include <crtdbg.h>
namespace {
// Prints the C runtime's assertion text to stderr but returns FALSE so the
// default (fatal) handling still runs. An assertion inside the runtime is a
// defect, so the suite must both show it and fail because of it.
int report_hook(int report_type, char* message, int* /*return_value*/) {
  const char* kind = report_type == _CRT_ASSERT ? "assert" : "error";
  std::fprintf(stderr, "[crt %s] %s\n", kind, message == nullptr ? "" : message);
  std::fflush(stderr);
  return 0;
}
}  // namespace
#endif

namespace fotest {

std::vector<Entry>& registry() {
  static std::vector<Entry> entries;
  return entries;
}

bool register_test(const char* suite, const char* name, TestFn fn) {
  Entry entry;
  entry.suite = suite;
  entry.name = name;
  entry.fn = fn;
  registry().push_back(std::move(entry));
  return true;
}

int run_all(int argc, char** argv) {
  std::string filter;
  std::uint64_t seed = 0x5DEECE66Dull;
  bool list_only = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--list") {
      list_only = true;
    } else if (argument.rfind("--filter=", 0) == 0) {
      filter = argument.substr(9);
    } else if (argument.rfind("--seed=", 0) == 0) {
      seed = std::strtoull(argument.c_str() + 7, nullptr, 10);
    } else {
      std::cerr << "unknown argument: " << argument << "\n";
      return 2;
    }
  }

  std::vector<Entry>& entries = registry();
  if (list_only) {
    for (const Entry& entry : entries) {
      std::cout << entry.suite << "." << entry.name << "\n";
    }
    return 0;
  }

  std::size_t passed = 0;
  std::size_t failed = 0;
  std::size_t checks = 0;
  for (const Entry& entry : entries) {
    const std::string full = entry.suite + "." + entry.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    Context ctx;
    ctx.suite = entry.suite;
    ctx.name = entry.name;
    ctx.seed = seed;
    std::string abort_message;
    try {
      entry.fn(ctx);
    } catch (const TestAbort& abort) {
      abort_message = abort.message;
    } catch (const std::exception& error) {
      ctx.fail(std::string("unhandled exception: ") + error.what());
    } catch (...) {
      ctx.fail("unhandled non-standard exception");
    }
    checks += ctx.checks;
    if (ctx.failures == 0) {
      ++passed;
      std::cout << "[ ok ] " << full << " (" << ctx.checks << " checks)\n";
    } else {
      ++failed;
      std::cout << "[FAIL] " << full << " (" << ctx.failures << " of " << ctx.checks
                << " checks failed)\n";
      for (const std::string& message : ctx.messages) {
        std::cout << "        " << message << "\n";
      }
      if (!abort_message.empty()) {
        std::cout << "        aborted: " << abort_message << "\n";
      }
    }
    std::cout.flush();
  }
  std::cout << "----\n"
            << "tests: " << (passed + failed) << "  passed: " << passed
            << "  failed: " << failed << "  checks: " << checks << "  seed: " << seed
            << "\n";
  if (failed == 0) {
    return 0;
  }
  return static_cast<int>(failed > 124 ? 124 : failed);
}

}  // namespace fotest

int main(int argc, char** argv) {
#if defined(_MSC_VER)
  _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, report_hook);
#endif
  return fotest::run_all(argc, argv);
}
