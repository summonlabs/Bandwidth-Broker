// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Requester process.
//
// A real OS process that connects to a coordinator, submits a request,
// arbitrates and reports the resulting grant on stdout in a machine-readable
// form. With --hold it stays alive until its input stream closes, which lets a
// test hard-kill a live incarnation.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "bandwidth_broker/client.hpp"
#include "bandwidth_broker/version.hpp"

namespace {

using namespace bandwidth_broker;

struct Options final {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  std::uint64_t publisher{1};
  std::string boot;
  std::string token;
  std::uint64_t request_id{1};
  std::int64_t minimum{0};
  std::int64_t desired{0};
  std::int64_t maximum{0};
  std::uint64_t priority{1};
  std::uint64_t tenant{1};
  std::uint64_t group{1};
  std::uint64_t resource{1};
  std::uint64_t resource_generation{1};
  std::uint64_t policy{1};
  std::uint64_t policy_generation{1};
  bool hold{false};
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

[[nodiscard]] std::uint64_t parse_u64(const std::string& text) {
  const auto value = parse_u64_decimal(text);
  if (!value.ok()) {
    std::fprintf(stderr, "invalid numeric argument\n");
    std::exit(2);
  }
  return value.value();
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  std::string value;
  for (int i = 1; i < argc; ++i) {
    if (next_value(argc, argv, i, "--host", value)) {
      options.host = value;
    } else if (next_value(argc, argv, i, "--port", value)) {
      options.port = static_cast<std::uint16_t>(parse_u64(value));
    } else if (next_value(argc, argv, i, "--publisher", value)) {
      options.publisher = parse_u64(value);
    } else if (next_value(argc, argv, i, "--boot", value)) {
      options.boot = value;
    } else if (next_value(argc, argv, i, "--token", value)) {
      options.token = value;
    } else if (next_value(argc, argv, i, "--request-id", value)) {
      options.request_id = parse_u64(value);
    } else if (next_value(argc, argv, i, "--min", value)) {
      options.minimum = static_cast<std::int64_t>(parse_u64(value));
    } else if (next_value(argc, argv, i, "--desired", value)) {
      options.desired = static_cast<std::int64_t>(parse_u64(value));
    } else if (next_value(argc, argv, i, "--max", value)) {
      options.maximum = static_cast<std::int64_t>(parse_u64(value));
    } else if (next_value(argc, argv, i, "--priority", value)) {
      options.priority = parse_u64(value);
    } else if (next_value(argc, argv, i, "--tenant", value)) {
      options.tenant = parse_u64(value);
    } else if (next_value(argc, argv, i, "--group", value)) {
      options.group = parse_u64(value);
    } else if (next_value(argc, argv, i, "--resource", value)) {
      options.resource = parse_u64(value);
    } else if (next_value(argc, argv, i, "--resource-generation", value)) {
      options.resource_generation = parse_u64(value);
    } else if (next_value(argc, argv, i, "--policy", value)) {
      options.policy = parse_u64(value);
    } else if (next_value(argc, argv, i, "--policy-generation", value)) {
      options.policy_generation = parse_u64(value);
    } else if (std::string_view(argv[i]) == "--hold") {
      options.hold = true;
    } else {
      std::fprintf(stderr, "unrecognised argument: %s\n", argv[i]);
      return 2;
    }
  }

  BootId boot;
  if (options.boot.empty()) {
    SystemEntropySource entropy;
    boot = generate_boot_id(entropy);
  } else {
    const auto parsed = BootId::parse(options.boot);
    if (!parsed.ok()) {
      std::fprintf(stderr, "invalid boot identity\n");
      return 2;
    }
    boot = parsed.value();
  }

  ClientOptions client_options;
  client_options.host = options.host;
  client_options.port = options.port;
  client_options.session_token = options.token;
  client_options.publisher = PublisherId::from_value(options.publisher);
  client_options.boot = boot;

  auto client = CoordinatorClient::connect(client_options);
  if (!client.ok()) {
    std::printf("ERROR connect %s\n", to_string(client.code()));
    std::fflush(stdout);
    return 1;
  }
  const auto hello = client.value()->handshake();
  if (!hello.ok()) {
    std::printf("REFUSED handshake %s\n", to_string(hello.code()));
    std::fflush(stdout);
    client.value()->close();
    return 3;
  }
  std::printf("CONNECTED %s\n", boot.to_string().c_str());
  std::printf("EPOCH %llu\n", static_cast<unsigned long long>(hello.value().epoch.value()));
  std::fflush(stdout);

  CapacityTarget target;
  target.resource = BandwidthResourceId::from_value(options.resource);
  target.resource_generation = BandwidthResourceGeneration::from_value(options.resource_generation);

  BandwidthRequest request;
  request.id = BandwidthRequestId::from_value(options.request_id);
  request.generation = BandwidthRequestGeneration::initial();
  request.target = target;
  request.minimum = Bandwidth::from_bits_per_second(options.minimum).value();
  request.desired = Bandwidth::from_bits_per_second(options.desired).value();
  request.maximum = Bandwidth::from_bits_per_second(options.maximum).value();
  request.priority = PriorityClassId::from_value(options.priority);
  request.tenant = TenantId::from_value(options.tenant);
  request.fairness_group = FairnessGroupId::from_value(options.group);
  request.preemptible = true;
  request.authority.fabric_epoch = hello.value().epoch;
  request.authority.resource = target.resource;
  request.authority.resource_generation = target.resource_generation;
  request.authority.policy = PolicyId::from_value(options.policy);
  request.authority.policy_generation = PolicyGeneration::from_value(options.policy_generation);
  request.authority.request = request.id;
  request.authority.request_generation = request.generation;
  request.authority.fairness_group = request.fairness_group;
  request.authority.fairness_config_generation = FairnessConfigGeneration::initial();
  request.authority.tenant_config_generation = TenantConfigGeneration::initial();
  request.authority.publisher = client_options.publisher;
  request.authority.publisher_boot = boot;
  request.authority.publisher_sequence = 1;

  const auto submitted = client.value()->submit({request});
  if (!submitted.ok() || submitted.value().status.code != ErrorCode::Ok) {
    std::printf("REFUSED submit %s\n", to_string(submitted.ok() ? submitted.value().status.code : submitted.code()));
    std::fflush(stdout);
    client.value()->close();
    return 4;
  }
  std::printf("SUBMITTED %llu %u\n", static_cast<unsigned long long>(options.request_id), submitted.value().accepted);
  std::fflush(stdout);

  const auto round = client.value()->arbitrate(target);
  if (!round.ok() || round.value().status.code != ErrorCode::Ok) {
    std::printf("REFUSED arbitrate %s\n", to_string(round.ok() ? round.value().status.code : round.code()));
    std::fflush(stdout);
    client.value()->close();
    return 5;
  }
  for (const ArbitrationDecision& decision : round.value().decisions) {
    if (!decision.grant.has_value()) {
      std::printf("NOGRANT %llu %s\n", static_cast<unsigned long long>(decision.request.value()),
                  to_string(decision.reason));
      continue;
    }
    const auto total = decision.grant->allocation.total();
    std::printf("GRANT %llu %llu %lld %s\n", static_cast<unsigned long long>(decision.grant->id.value()),
                static_cast<unsigned long long>(decision.grant->generation.value()),
                static_cast<long long>(total.ok() ? total.value().bits_per_second() : -1),
                to_string(decision.grant->state));
  }
  std::fflush(stdout);

  if (options.hold) {
    std::printf("HOLDING\n");
    std::fflush(stdout);
    char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), stdin) != nullptr) {
      if (std::string_view(buffer).rfind("quit", 0) == 0) {
        break;
      }
    }
  }
  client.value()->close();
  std::printf("DONE\n");
  std::fflush(stdout);
  return 0;
}
