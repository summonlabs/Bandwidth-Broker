// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Test runner. Tests run to natural completion: there is no watchdog, no
// timeout and no forced termination. A test that does not terminate is a defect
// to diagnose, never a test to kill.

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "support/test_harness.hpp"

namespace {

struct Options final {
  std::string filter;
  bool list_only{false};
};

Options parse_options(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    constexpr std::string_view prefix = "--filter=";
    if (arg.rfind(prefix, 0) == 0) {
      options.filter = std::string(arg.substr(prefix.size()));
    } else if (arg == "--list") {
      options.list_only = true;
    } else if (arg == "--help") {
      std::printf("usage: %s [--filter=SUBSTRING] [--list]\n", argv[0]);
      std::exit(0);
    } else {
      std::printf("unrecognised argument: %s\n", argv[i]);
      std::exit(2);
    }
  }
  return options;
}

[[nodiscard]] bool matches(const bb_test::TestCase& test, const std::string& filter) {
  if (filter.empty()) {
    return true;
  }
  const std::string full = std::string(test.suite) + "." + test.name;
  return full.find(filter) != std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = parse_options(argc, argv);
  const auto& cases = bb_test::Registry::instance().cases();

  std::size_t selected = 0;
  std::size_t passed = 0;
  std::size_t failed = 0;
  std::vector<std::string> failed_names;

  for (const auto& test : cases) {
    if (!matches(test, options.filter)) {
      continue;
    }
    ++selected;
    if (options.list_only) {
      std::printf("%s.%s\n", test.suite, test.name);
      continue;
    }
    std::printf("[ RUN  ] %s.%s\n", test.suite, test.name);
    std::fflush(stdout);
    bb_test::Context::instance().reset();
    bool threw = false;
    try {
      test.body();
    } catch (const std::exception& ex) {
      threw = true;
      BB_FAIL(std::string("test threw an exception: ") + ex.what());
    } catch (...) {
      threw = true;
      BB_FAIL("test threw a non-standard exception");
    }
    const std::size_t failures = bb_test::Context::instance().failure_count();
    if (failures == 0 && !threw) {
      ++passed;
      std::printf("[  OK  ] %s.%s\n", test.suite, test.name);
    } else {
      ++failed;
      failed_names.push_back(std::string(test.suite) + "." + test.name);
      std::printf("[ FAIL ] %s.%s (%zu failures)\n", test.suite, test.name, failures);
    }
    std::fflush(stdout);
  }

  if (options.list_only) {
    std::printf("%zu test(s) listed\n", selected);
    return 0;
  }

  std::printf("\n%zu test(s) run: %zu passed, %zu failed\n", selected, passed, failed);
  for (const auto& name : failed_names) {
    std::printf("  failing: %s\n", name.c_str());
  }
  return failed == 0 ? 0 : 1;
}
