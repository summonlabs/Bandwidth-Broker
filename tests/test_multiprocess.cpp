// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Multiprocess proof.
//
// Every claim in this file is made against real OS processes talking over real
// framed TCP: a coordinator process and requester processes. The requester is
// hard-killed, its boot identity is permanently fenced, a fresh incarnation is
// registered, stale identity is refused, the coordinator is hard-killed and
// restarted against the same store with an advanced epoch.

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "bandwidth_broker/client.hpp"
#include "bandwidth_broker/version.hpp"
#include "support/child_process.hpp"
#include "support/fixtures.hpp"
#include "support/test_harness.hpp"

using namespace bandwidth_broker;
using bb_fixture::basic_policy;
using bb_fixture::Fixture;

namespace {

namespace fs = std::filesystem;

#ifndef BB_COORDINATOR_PATH
#error "BB_COORDINATOR_PATH must be defined by the build"
#endif
#ifndef BB_REQUESTER_PATH
#error "BB_REQUESTER_PATH must be defined by the build"
#endif

constexpr const char* kCoordinator = BB_COORDINATOR_PATH;
constexpr const char* kRequester = BB_REQUESTER_PATH;
constexpr const char* kToken = "test-session-token";

class StoreDirectory final {
 public:
  explicit StoreDirectory(const char* name) {
    static std::uint64_t counter = 0;
    ++counter;
    path_ = fs::temp_directory_path() / ("bb-mp-" + std::string(name) + "-" + std::to_string(counter));
    std::error_code error;
    fs::remove_all(path_, error);
  }
  ~StoreDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }
  StoreDirectory(const StoreDirectory&) = delete;
  StoreDirectory& operator=(const StoreDirectory&) = delete;
  [[nodiscard]] std::string path() const { return path_.string(); }

 private:
  fs::path path_;
};

struct CoordinatorHandle final {
  bb_child::ChildProcess process;
  std::uint16_t port{0};
  std::uint64_t epoch{0};
  std::uint64_t incarnation{0};
};

// Starts a coordinator process and waits for its READY line. The wait is a real
// blocking read: there is no timeout anywhere in this file.
[[nodiscard]] bool start_coordinator(CoordinatorHandle& handle,
                                     const std::string& store,
                                     std::uint64_t incarnation,
                                     std::uint64_t epoch) {
  std::vector<std::string> arguments = {"--port", "0", "--store", store, "--token", kToken, "--incarnation",
                                        std::to_string(incarnation), "--epoch", std::to_string(epoch)};
  if (!handle.process.spawn(kCoordinator, arguments)) {
    BB_FAIL("cannot spawn the coordinator process");
    return false;
  }
  const std::string ready = handle.process.read_line();
  unsigned port = 0;
  unsigned long long parsed_epoch = 0;
  unsigned long long parsed_incarnation = 0;
  if (std::sscanf(ready.c_str(), "READY %u %llu %llu", &port, &parsed_epoch, &parsed_incarnation) != 3) {
    BB_FAIL("coordinator did not report readiness: " + ready);
    return false;
  }
  handle.port = static_cast<std::uint16_t>(port);
  handle.epoch = parsed_epoch;
  handle.incarnation = parsed_incarnation;
  return true;
}

[[nodiscard]] std::unique_ptr<CoordinatorClient> connect_client(std::uint16_t port, std::uint64_t publisher,
                                                               std::uint64_t boot_hi, std::uint64_t boot_lo) {
  ClientOptions options;
  options.host = "127.0.0.1";
  options.port = port;
  options.session_token = kToken;
  options.publisher = PublisherId::from_value(publisher);
  options.boot = boot_id_from_u64(boot_hi, boot_lo);
  auto client = CoordinatorClient::connect(options);
  if (!client.ok()) {
    BB_FAIL("cannot connect a client: " + client.error().message);
    return nullptr;
  }
  return client.take();
}

// Spawns a requester process and collects its report lines.
struct RequesterReport final {
  std::string connected;
  std::string epoch;
  std::string submitted;
  std::string grant;
  std::string holding;
  bool refused{false};
  std::string refusal;
};

[[nodiscard]] bool run_requester(bb_child::ChildProcess& process, const std::vector<std::string>& arguments,
                                 RequesterReport& report, bool hold, std::uint16_t port, const std::string& boot_hex) {
  std::vector<std::string> full = {"--host", "127.0.0.1", "--port", std::to_string(port), "--token", kToken,
                                   "--boot", boot_hex};
  for (const std::string& argument : arguments) {
    full.push_back(argument);
  }
  if (hold) {
    full.push_back("--hold");
  }
  if (!process.spawn(kRequester, full)) {
    BB_FAIL("cannot spawn the requester process");
    return false;
  }
  for (;;) {
    const std::string line = process.read_line();
    if (line.empty()) {
      return true;  // end of output
    }
    if (line.rfind("CONNECTED", 0) == 0) {
      report.connected = line;
    } else if (line.rfind("EPOCH", 0) == 0) {
      report.epoch = line;
    } else if (line.rfind("SUBMITTED", 0) == 0) {
      report.submitted = line;
    } else if (line.rfind("GRANT", 0) == 0) {
      report.grant = line;
    } else if (line.rfind("REFUSED", 0) == 0 || line.rfind("ERROR", 0) == 0) {
      report.refused = true;
      report.refusal = line;
    } else if (line.rfind("HOLDING", 0) == 0) {
      report.holding = line;
      if (hold) {
        return true;
      }
    } else if (line.rfind("NOGRANT", 0) == 0) {
      report.grant = line;
    }
  }
}

[[nodiscard]] std::string boot_hex(std::uint64_t hi, std::uint64_t lo) {
  return boot_id_from_u64(hi, lo).to_string();
}

}  // namespace

BB_TEST(Multiprocess, KillFenceAndFreshIncarnation) {
  StoreDirectory store("kill");
  CoordinatorHandle coordinator;
  BB_REQUIRE(start_coordinator(coordinator, store.path(), 1, 1));

  Fixture fixture;
  auto operator_client = connect_client(coordinator.port, 1, 100, 1);
  BB_REQUIRE(operator_client != nullptr);
  const auto hello = operator_client->handshake();
  BB_REQUIRE(hello.ok());
  BB_CHECK_EQ(hello.value().epoch.value(), coordinator.epoch);
  BB_CHECK_OK(operator_client->set_policy(basic_policy(fixture)));

  Fixture publisher = fixture;
  publisher.tick = 1;
  BB_CHECK_OK(operator_client->publish_capacity(publisher.snapshot(1'000'000)));

  // A real requester process holds a real grant.
  const std::string dead_boot = boot_hex(2, 2);
  bb_child::ChildProcess requester;
  RequesterReport first;
  BB_REQUIRE(run_requester(requester, {"--publisher", "2", "--request-id", "100", "--min", "0", "--desired",
                                       "400000", "--max", "400000", "--priority", "3"},
                           first, true, coordinator.port, dead_boot));
  BB_CHECK(!first.connected.empty());
  BB_CHECK(first.submitted.rfind("SUBMITTED 100 1", 0) == 0);
  BB_CHECK(first.grant.rfind("GRANT ", 0) == 0);
  BB_CHECK(!first.holding.empty());

  // Hard-kill the requester: no graceful shutdown, no cleanup.
  requester.kill_hard();
  const int exit_code = requester.wait();
  BB_CHECK(exit_code != 0);

  // Fence the dead boot permanently, then verify that stale identity is refused.
  const auto fenced = operator_client->fence_publisher(PublisherId::from_value(2), boot_id_from_u64(2, 2),
                                                       "hard-killed requester");
  BB_REQUIRE(fenced.ok());
  BB_CHECK_EQ(fenced.value().status.code, ErrorCode::Ok);
  BB_CHECK(!fenced.value().already_fenced);

  bb_child::ChildProcess stale;
  RequesterReport stale_report;
  BB_REQUIRE(run_requester(stale, {"--publisher", "2", "--request-id", "101", "--min", "0", "--desired", "1000",
                                   "--max", "1000", "--priority", "3"},
                           stale_report, false, coordinator.port, dead_boot));
  BB_CHECK(stale_report.refused);
  BB_CHECK(stale_report.refusal.find("boot_fenced") != std::string::npos);
  BB_CHECK_EQ(stale.wait(), 3);

  // A fresh incarnation from the same logical publisher is accepted.
  const std::string fresh_boot = boot_hex(2, 3);
  bb_child::ChildProcess fresh;
  RequesterReport fresh_report;
  BB_REQUIRE(run_requester(fresh, {"--publisher", "2", "--request-id", "102", "--min", "0", "--desired", "100000",
                                   "--max", "100000", "--priority", "3"},
                           fresh_report, true, coordinator.port, fresh_boot));
  BB_CHECK(fresh_report.grant.rfind("GRANT ", 0) == 0);
  BB_CHECK(!fresh_report.holding.empty());

  // The fenced incarnation's grant no longer authorises consumption.
  const auto accounting = operator_client->query_accounting(fixture.target());
  BB_REQUIRE(accounting.ok());
  BB_CHECK_EQ(accounting.value().status.code, ErrorCode::Ok);
  BB_CHECK_EQ(accounting.value().accounting.authorized_consumption.bits_per_second(), 100'000);

  fresh.kill_hard();
  (void)fresh.wait();
  operator_client->close();
  coordinator.process.write_line("stop");
  coordinator.process.close_stdin();
  (void)coordinator.process.wait();
}

BB_TEST(Multiprocess, CoordinatorRestartAdvancesEpochAndPreservesDurableFacts) {
  StoreDirectory store("restart");
  std::vector<std::string> observed_grants;
  {
    CoordinatorHandle coordinator;
    BB_REQUIRE(start_coordinator(coordinator, store.path(), 1, 1));
    const std::uint64_t first_epoch = coordinator.epoch;

    Fixture fixture;
    auto client = connect_client(coordinator.port, 1, 100, 1);
    BB_REQUIRE(client != nullptr);
    BB_REQUIRE(client->handshake().ok());
    BB_CHECK_OK(client->set_policy(basic_policy(fixture)));
    Fixture publisher = fixture;
    publisher.tick = 1;
    BB_CHECK_OK(client->publish_capacity(publisher.snapshot(1'000'000)));

    const std::string boot = boot_hex(3, 3);
    bb_child::ChildProcess requester;
    RequesterReport report;
    BB_REQUIRE(run_requester(requester, {"--publisher", "3", "--request-id", "200", "--min", "0", "--desired",
                                         "250000", "--max", "250000", "--priority", "3"},
                             report, true, coordinator.port, boot));
    BB_CHECK(report.grant.rfind("GRANT ", 0) == 0);
    observed_grants.push_back(report.grant);
    requester.kill_hard();
    (void)requester.wait();
    client->close();

    // Hard-kill the coordinator itself, without a graceful shutdown.
    coordinator.process.kill_hard();
    (void)coordinator.process.wait();
    BB_CHECK(first_epoch >= 1);
  }

  // Restart against the same store: the epoch must advance and no liveness may
  // be restored.
  CoordinatorHandle restarted;
  BB_REQUIRE(start_coordinator(restarted, store.path(), 1, 1));
  BB_CHECK(restarted.epoch > 1);

  auto client = connect_client(restarted.port, 1, 100, 9);
  BB_REQUIRE(client != nullptr);
  const auto hello = client->handshake();
  BB_REQUIRE(hello.ok());
  BB_CHECK_EQ(hello.value().epoch.value(), restarted.epoch);

  Fixture fixture;
  // The policy survived the restart: a submission is accepted without
  // reinstalling it. The request must be built against the epoch the restarted
  // coordinator announced, which is exactly what a real requester does.
  BandwidthRequest current = fixture.request(bb_fixture::RequestSpec{201, 1, 0, 1000, 1000, 3});
  current.authority.fabric_epoch = hello.value().epoch;
  const auto submitted = client->submit({current});
  BB_REQUIRE(submitted.ok());
  BB_CHECK_EQ(submitted.value().status.code, ErrorCode::Ok);
  BB_CHECK_EQ(submitted.value().accepted, std::uint32_t{1});
  BB_CHECK(submitted.value().refused.empty());

  // Capacity evidence did not survive as current: a fresh publication under the
  // new epoch is required before anything is arbitrable again.
  const auto before = client->query_accounting(fixture.target());
  BB_REQUIRE(before.ok());
  BB_CHECK_EQ(before.value().accounting.evidence, CapacityEvidenceState::Stale);
  BB_CHECK_EQ(before.value().accounting.authorized_consumption.bits_per_second(), 0);

  const auto stale_round = client->arbitrate(fixture.target());
  BB_REQUIRE(stale_round.ok());
  BB_CHECK_EQ(stale_round.value().accounting.authorized_consumption.bits_per_second(), 0);
  BB_CHECK_EQ(stale_round.value().accounting.evidence, CapacityEvidenceState::Stale);

  Fixture republication = fixture;
  republication.epoch = FabricEpoch::from_value(restarted.epoch);
  republication.capacity_generation = CapacitySnapshotGeneration::from_value(2);
  republication.tick = 100;
  const auto published = client->publish_capacity(republication.snapshot(1'000'000));
  BB_REQUIRE(published.ok());
  BB_CHECK_EQ(published.value().status.code, ErrorCode::Ok);

  const auto round = client->arbitrate(fixture.target());
  BB_REQUIRE(round.ok());
  BB_CHECK_EQ(round.value().accounting.evidence, CapacityEvidenceState::Known);
  BB_CHECK(round.value().accounting.authorized_consumption.bits_per_second() > 0);

  client->close();
  restarted.process.write_line("stop");
  restarted.process.close_stdin();
  (void)restarted.process.wait();
}
