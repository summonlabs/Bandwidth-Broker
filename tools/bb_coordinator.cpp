// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Coordinator process.
//
// A real OS process holding a real TCP listener over a real framed protocol.
// It prints one machine-readable line when it is ready to accept work, then
// serves until it is asked to stop or its input stream closes.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "bandwidth_broker/broker.hpp"
#include "bandwidth_broker/server.hpp"
#include "bandwidth_broker/version.hpp"

namespace {

using namespace bandwidth_broker;

struct Options final {
  std::uint16_t port{0};
  std::string store;
  std::uint64_t incarnation{1};
  std::uint64_t epoch{1};
  std::string token;
  bool fsync{true};
};

[[nodiscard]] bool next_value(int argc, char** argv, int& index, std::string_view name, std::string& out) {
  if (std::string_view(argv[index]) != name) {
    return false;
  }
  if (index + 1 >= argc) {
    std::fprintf(stderr, "missing value for %.*s\n", static_cast<int>(name.size()), name.data());
    std::exit(2);
  }
  out = argv[++index];
  return true;
}

[[nodiscard]] std::uint64_t parse_u64(const std::string& text, const char* name) {
  const auto value = parse_u64_decimal(text);
  if (!value.ok()) {
    std::fprintf(stderr, "invalid value for %s\n", name);
    std::exit(2);
  }
  return value.value();
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  std::string value;
  for (int i = 1; i < argc; ++i) {
    if (next_value(argc, argv, i, "--port", value)) {
      options.port = static_cast<std::uint16_t>(parse_u64(value, "--port"));
    } else if (next_value(argc, argv, i, "--store", value)) {
      options.store = value;
    } else if (next_value(argc, argv, i, "--incarnation", value)) {
      options.incarnation = parse_u64(value, "--incarnation");
    } else if (next_value(argc, argv, i, "--epoch", value)) {
      options.epoch = parse_u64(value, "--epoch");
    } else if (next_value(argc, argv, i, "--token", value)) {
      options.token = value;
    } else if (std::string_view(argv[i]) == "--no-fsync") {
      options.fsync = false;
    } else {
      std::fprintf(stderr, "unrecognised argument: %s\n", argv[i]);
      return 2;
    }
  }

  BrokerConfig config;
  config.epoch = FabricEpoch::from_value(options.epoch);
  config.incarnation = CoordinatorIncarnation::from_value(options.incarnation);
  config.store_directory = options.store;
  config.fsync_on_commit = options.fsync;

  auto broker = Broker::open(config);
  if (!broker.ok()) {
    std::fprintf(stderr, "cannot open the coordinator: %s (%s)\n", to_string(broker.code()),
                 broker.error().message.c_str());
    return 1;
  }

  ServerOptions server_options;
  server_options.port = options.port;
  server_options.session_token = options.token;

  auto server = CoordinatorServer::start(*broker.value(), server_options);
  if (!server.ok()) {
    std::fprintf(stderr, "cannot start the listener: %s (%s)\n", to_string(server.code()),
                 server.error().message.c_str());
    return 1;
  }

  const CoordinatorStatus status = broker.value()->status();
  std::printf("READY %u %llu %llu\n", static_cast<unsigned>(server.value()->port()),
              static_cast<unsigned long long>(status.epoch.value()),
              static_cast<unsigned long long>(status.incarnation.value()));
  std::fflush(stdout);

  // Serve until the input stream closes or a shutdown request stops the server.
  char buffer[256];
  while (std::fgets(buffer, sizeof(buffer), stdin) != nullptr) {
    if (std::string_view(buffer).rfind("stop", 0) == 0) {
      break;
    }
  }
  server.value()->stop();
  const auto flushed = broker.value()->flush();
  if (!flushed.ok()) {
    std::fprintf(stderr, "flush failed: %s\n", flushed.message().c_str());
    return 1;
  }
  std::printf("STOPPED\n");
  std::fflush(stdout);
  return 0;
}
