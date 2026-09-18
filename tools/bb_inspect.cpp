// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Inspection tool: reads a coordinator store without joining a running
// coordinator, reports what recovery would do and which durable facts exist.

#include <cstdio>
#include <string>

#include "bandwidth_broker/persistence.hpp"
#include "bandwidth_broker/version.hpp"

int main(int argc, char** argv) {
  using namespace bandwidth_broker;
  if (argc != 2) {
    std::fprintf(stderr, "usage: bb_inspect <store-directory>\n");
    return 2;
  }
  const std::string directory = argv[1];

  StoreOptions options;
  auto store = Store::open(directory, options);
  if (!store.ok()) {
    std::fprintf(stderr, "cannot open the store: %s (%s)\n", to_string(store.code()), store.error().message.c_str());
    return 1;
  }
  const auto report = store.value()->inspect();
  if (!report.ok()) {
    std::fprintf(stderr, "inspection failed: %s (%s)\n", to_string(report.code()), report.error().message.c_str());
    return 1;
  }
  const RecoveryReport& value = report.value();
  std::printf("store=%s\n", directory.c_str());
  std::printf("format_version=%u\n", store_format_version());
  std::printf("manifest_present=%s\n", value.manifest_present ? "yes" : "no");
  std::printf("snapshot_present=%s\n", value.snapshot_loaded ? "yes" : "no");
  std::printf("snapshot_corrupt=%s\n", value.snapshot_corrupt ? "yes" : "no");
  std::printf("snapshot_sequence=%llu\n", static_cast<unsigned long long>(value.snapshot_sequence));
  std::printf("journal_records=%llu\n", static_cast<unsigned long long>(value.journal_records));
  std::printf("journal_truncated=%s\n", value.journal_truncated ? "yes" : "no");
  std::printf("truncations=%llu\n", static_cast<unsigned long long>(value.truncations));
  std::printf("recoverable=%s\n", value.version_supported ? "yes" : "no");
  return 0;
}
