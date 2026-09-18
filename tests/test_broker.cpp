// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Proof obligations for the coordinator runtime: idempotency, generation-bound
// authority, release, fencing, durable restart and conservative recovery.

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "bandwidth_broker/broker.hpp"
#include "support/fixtures.hpp"
#include "support/test_harness.hpp"

using namespace bandwidth_broker;
using bb_fixture::basic_policy;
using bb_fixture::Fixture;
using bb_fixture::RequestSpec;

namespace {

namespace fs = std::filesystem;

// Every test gets its own store directory so that durable state never leaks
// between tests or between runs.
class TempStore final {
 public:
  explicit TempStore(const char* name) {
    static std::uint64_t counter = 0;
    ++counter;
    path_ = fs::temp_directory_path() / ("bb-test-" + std::string(name) + "-" + std::to_string(counter));
    std::error_code error;
    fs::remove_all(path_, error);
  }
  ~TempStore() {
    std::error_code error;
    fs::remove_all(path_, error);
  }
  TempStore(const TempStore&) = delete;
  TempStore& operator=(const TempStore&) = delete;

  [[nodiscard]] std::string path() const { return path_.string(); }

 private:
  fs::path path_;
};

[[nodiscard]] BrokerConfig memory_config(const Fixture& fixture) {
  BrokerConfig config;
  config.epoch = fixture.epoch;
  config.incarnation = fixture.coordinator;
  return config;
}

[[nodiscard]] BrokerConfig durable_config(const Fixture& fixture, const std::string& directory) {
  BrokerConfig config = memory_config(fixture);
  config.store_directory = directory;
  config.fsync_on_commit = true;
  return config;
}

[[nodiscard]] RequestSpec spec(std::uint64_t id, std::int64_t minimum, std::int64_t desired, std::int64_t maximum,
                               std::uint64_t priority = 1) {
  RequestSpec request;
  request.id = id;
  request.minimum = minimum;
  request.desired = desired;
  request.maximum = maximum;
  request.priority = priority;
  return request;
}

}  // namespace

BB_TEST(Broker, PolicyGenerationMustAdvance) {
  Fixture fixture;
  auto broker = Broker::open(memory_config(fixture));
  BB_REQUIRE(broker.ok());
  Policy policy = basic_policy(fixture);
  BB_CHECK_OK(broker.value()->set_policy(policy));
  BB_CHECK_OK(broker.value()->set_policy(policy));  // idempotent re-install of the same generation
  policy.generation = PolicyGeneration::from_value(0);
  BB_CHECK_ERR(ErrorCode::InvalidIdentity, broker.value()->set_policy(policy));
}

BB_TEST(Broker, ArbitrationRequiresCapacityEvidence) {
  Fixture fixture;
  auto broker = Broker::open(memory_config(fixture));
  BB_REQUIRE(broker.ok());
  BB_CHECK_OK(broker.value()->set_policy(basic_policy(fixture)));

  RequestSpec request = spec(1, 0, 500, 500, 3);
  BB_CHECK_OK(broker.value()->submit(fixture.request(request)));

  // No capacity has been published: the round must refuse to invent any.
  const auto summary = broker.value()->arbitrate(fixture.target());
  BB_REQUIRE(summary.ok());
  BB_CHECK_EQ(summary.value().granted, std::size_t{0});
  BB_CHECK_EQ(summary.value().waiting, std::size_t{1});
  BB_CHECK_EQ(summary.value().accounting.evidence, CapacityEvidenceState::Unknown);
  BB_CHECK_EQ(summary.value().accounting.authorized_consumption.bits_per_second(), 0);

  const auto explanation = broker.value()->explain(BandwidthRequestId::from_value(1),
                                                   BandwidthRequestGeneration::initial());
  BB_REQUIRE(explanation.ok());
  BB_CHECK_EQ(explanation.value().reason, OutcomeReason::CapacityUnknown);
}

BB_TEST(Broker, PublishesCapacityArbitratesAndIssuesAGrant) {
  Fixture fixture;
  auto broker = Broker::open(memory_config(fixture));
  BB_REQUIRE(broker.ok());
  BB_CHECK_OK(broker.value()->set_policy(basic_policy(fixture)));
  BB_CHECK_OK(broker.value()->publish_capacity(fixture.snapshot(1000)));

  BB_CHECK_OK(broker.value()->submit(fixture.request(spec(1, 200, 600, 800, 3))));
  const auto summary = broker.value()->arbitrate(fixture.target());
  BB_REQUIRE(summary.ok());
  BB_CHECK_EQ(summary.value().granted, std::size_t{1});
  BB_CHECK_EQ(summary.value().accounting.authorized_consumption.bits_per_second(), 600);

  const auto grants = broker.value()->grants(fixture.target(), true);
  BB_REQUIRE(grants.ok());
  BB_REQUIRE(grants.value().size() == 1);
  const Grant& grant = grants.value()[0];
  // The grant holds 200 bps of guaranteed capacity plus 400 bps of
  // discretionary capacity, so it is revocable and carries the borrowed
  // lifecycle state while keeping the breakdown explicit.
  BB_CHECK_EQ(grant.state, GrantState::GrantedBorrowed);
  BB_CHECK_EQ(grant.allocation.discretionary.bits_per_second(), 400);
  BB_CHECK_EQ(grant.allocation.guaranteed.bits_per_second(), 200);
  BB_CHECK_EQ(grant.allocation.discretionary.bits_per_second(), 400);

  const auto queried = broker.value()->query_grant(grant.id, grant.generation);
  BB_REQUIRE(queried.ok());
  BB_CHECK(queried.value().found);
  BB_CHECK(queried.value().authoritative);

  const auto superseded = broker.value()->query_grant(grant.id, BandwidthGrantGeneration::from_value(99));
  BB_REQUIRE(superseded.ok());
  BB_CHECK(!superseded.value().authoritative);

  const auto explanation = broker.value()->explain(BandwidthRequestId::from_value(1),
                                                   BandwidthRequestGeneration::initial());
  BB_REQUIRE(explanation.ok());
  BB_CHECK_EQ(explanation.value().guaranteed.bits_per_second(), 200);
  BB_CHECK_EQ(explanation.value().effective_physical.bits_per_second(), 1000);
  BB_CHECK(!explanation.value().binding_reason.empty());
}

BB_TEST(Broker, DuplicateSubmissionIsIdempotentAndConflictsAreRefused) {
  Fixture fixture;
  auto broker = Broker::open(memory_config(fixture));
  BB_REQUIRE(broker.ok());
  BB_CHECK_OK(broker.value()->set_policy(basic_policy(fixture)));
  BB_CHECK_OK(broker.value()->publish_capacity(fixture.snapshot(1000)));

  const BandwidthRequest original = fixture.request(spec(1, 100, 400, 800, 3));
  const auto first = broker.value()->submit(original);
  BB_REQUIRE(first.ok());
  BB_CHECK_EQ(first.value().disposition, IdempotencyDisposition::New);

  const auto second = broker.value()->submit(original);
  BB_REQUIRE(second.ok());
  BB_CHECK_EQ(second.value().disposition, IdempotencyDisposition::Replay);

  BandwidthRequest conflicting = original;
  conflicting.desired = bb_fixture::bw(500);
  BB_CHECK_ERR(ErrorCode::IdentityConflict, broker.value()->submit(conflicting));

  // Only one request exists, so a round grants once.
  const auto summary = broker.value()->arbitrate(fixture.target());
  BB_REQUIRE(summary.ok());
  BB_CHECK_EQ(summary.value().granted, std::size_t{1});
  BB_CHECK_EQ(summary.value().accounting.authorized_consumption.bits_per_second(), 400);
}

BB_TEST(Broker, ReleaseReturnsCapacityToBaselineAndIsIdempotent) {
  Fixture fixture;
  auto broker = Broker::open(memory_config(fixture));
  BB_REQUIRE(broker.ok());
  BB_CHECK_OK(broker.value()->set_policy(basic_policy(fixture)));
  BB_CHECK_OK(broker.value()->publish_capacity(fixture.snapshot(1000)));
  BB_CHECK_OK(broker.value()->submit(fixture.request(spec(1, 300, 700, 900, 3))));
  BB_REQUIRE(broker.value()->arbitrate(fixture.target()).ok());

  const auto before = broker.value()->accounting(fixture.target());
  BB_REQUIRE(before.ok());
  BB_CHECK_EQ(before.value().authorized_consumption.bits_per_second(), 700);
  BB_CHECK_EQ(before.value().unallocated.bits_per_second(), 300);

  const auto grants = broker.value()->grants(fixture.target(), true);
  BB_REQUIRE(grants.ok());
  BB_REQUIRE(grants.value().size() == 1);
  const Grant grant = grants.value()[0];

  const auto released = broker.value()->release(grant.id, grant.generation);
  BB_REQUIRE(released.ok());
  BB_CHECK_EQ(released.value().released.bits_per_second(), 700);
  BB_CHECK(!released.value().already_released);

  const auto after = broker.value()->accounting(fixture.target());
  BB_REQUIRE(after.ok());
  BB_CHECK_EQ(after.value().authorized_consumption.bits_per_second(), 0);
  BB_CHECK_EQ(after.value().unallocated.bits_per_second(), 1000);
  BB_CHECK_EQ(after.value().effective_physical.bits_per_second(), 1000);

  // Releasing the superseded generation is refused: the old generation never
  // authorises anything again.
  BB_CHECK_ERR(ErrorCode::StaleGrant, broker.value()->release(grant.id, grant.generation));

  const auto current = broker.value()->query_grant(grant.id, BandwidthGrantGeneration::from_value(2));
  BB_REQUIRE(current.ok());
  BB_REQUIRE(current.value().found);
  const auto again = broker.value()->release(grant.id, current.value().grant.generation);
  BB_REQUIRE(again.ok());
  BB_CHECK(again.value().already_released);
  BB_CHECK_EQ(again.value().released.bits_per_second(), 0);

  const auto final_accounting = broker.value()->accounting(fixture.target());
  BB_REQUIRE(final_accounting.ok());
  BB_CHECK_EQ(final_accounting.value().unallocated.bits_per_second(), 1000);
}

BB_TEST(Broker, FencingWithdrawsGrantsImmediately) {
  Fixture fixture;
  auto broker = Broker::open(memory_config(fixture));
  BB_REQUIRE(broker.ok());
  BB_CHECK_OK(broker.value()->set_policy(basic_policy(fixture)));
  BB_CHECK_OK(broker.value()->publish_capacity(fixture.snapshot(1000)));
  BB_CHECK_OK(broker.value()->submit(fixture.request(spec(1, 0, 800, 800, 3))));
  BB_REQUIRE(broker.value()->arbitrate(fixture.target()).ok());

  const auto boot = boot_id_from_u64(1, 0xAA);
  const auto fenced = broker.value()->fence_publisher(PublisherId::from_value(1), boot, "process died");
  BB_CHECK_OK(fenced);
  const auto is_fenced = broker.value()->is_fenced(PublisherId::from_value(1), boot);
  BB_REQUIRE(is_fenced.ok());
  BB_CHECK(is_fenced.value());

  const auto accounting = broker.value()->accounting(fixture.target());
  BB_REQUIRE(accounting.ok());
  BB_CHECK_EQ(accounting.value().authorized_consumption.bits_per_second(), 0);
  BB_CHECK_EQ(accounting.value().unallocated.bits_per_second(), 1000);

  const auto grants = broker.value()->grants(fixture.target(), true);
  BB_REQUIRE(grants.ok());
  BB_CHECK(grants.value().empty());

  // A later submission from the fenced incarnation is refused, not granted.
  const auto refused = broker.value()->submit(fixture.request(spec(2, 0, 100, 100, 3)));
  BB_REQUIRE(refused.ok());
  BB_CHECK(refused.value().refused);
  BB_CHECK_EQ(refused.value().reason, OutcomeReason::RefusedBootFenced);
}

BB_TEST(Broker, RestartAdvancesEpochAndRequiresRevalidation) {
  Fixture fixture;
  TempStore store("restart");
  Grant before;
  {
    auto broker = Broker::open(durable_config(fixture, store.path()));
    BB_REQUIRE(broker.ok());
    BB_CHECK_OK(broker.value()->set_policy(basic_policy(fixture)));
    BB_CHECK_OK(broker.value()->publish_capacity(fixture.snapshot(1000)));
    BB_CHECK_OK(broker.value()->submit(fixture.request(spec(1, 100, 500, 900, 3))));
    const auto summary = broker.value()->arbitrate(fixture.target());
    BB_REQUIRE(summary.ok());
    BB_CHECK_EQ(summary.value().granted, std::size_t{1});
    const auto grants = broker.value()->grants(fixture.target(), true);
    BB_REQUIRE(grants.ok());
    BB_REQUIRE(grants.value().size() == 1);
    before = grants.value()[0];
    BB_CHECK_OK(broker.value()->flush());
  }

  auto restarted = Broker::open(durable_config(fixture, store.path()));
  if (!restarted.ok()) {
    BB_FAIL(std::string("restart failed: ") + to_string(restarted.code()) + " " + restarted.error().message);
    return;
  }
  const CoordinatorStatus status = restarted.value()->status();
  BB_CHECK(status.durable);
  BB_CHECK(status.epoch.value() > fixture.epoch.value());
  BB_CHECK_EQ(status.live_grants, std::size_t{0});
  BB_CHECK_EQ(status.revalidation_required, std::size_t{1});
  BB_CHECK_EQ(status.policies, std::size_t{1});
  BB_CHECK_EQ(status.requests, std::size_t{1});

  // Durable facts survive; dynamic authority does not.
  const auto policy = restarted.value()->policy();
  BB_REQUIRE(policy.ok());
  BB_CHECK_EQ(policy.value().id, fixture.policy_id);

  const auto capacity = restarted.value()->capacity(fixture.target());
  BB_REQUIRE(capacity.ok());
  BB_CHECK_EQ(capacity.value().evidence, CapacityEvidenceState::Stale);

  const auto grants = restarted.value()->grants(fixture.target(), true);
  BB_REQUIRE(grants.ok());
  BB_REQUIRE(grants.value().size() == 1);
  BB_CHECK_EQ(grants.value()[0].state, GrantState::RevalidationRequired);
  BB_CHECK(grants.value()[0].id == before.id);

  const auto queried = restarted.value()->query_grant(before.id, before.generation);
  BB_REQUIRE(queried.ok());
  BB_CHECK(!queried.value().authoritative);

  // Revalidation is refused while capacity evidence is stale.
  BB_CHECK_ERR(ErrorCode::CapacityUnknown,
               restarted.value()->revalidate(before.id, grants.value()[0].generation));
}

BB_TEST(Broker, RevalidationSucceedsAfterFreshCapacityPublication) {
  Fixture fixture;
  TempStore store("revalidate");
  Grant before;
  {
    auto broker = Broker::open(durable_config(fixture, store.path()));
    BB_REQUIRE(broker.ok());
    BB_CHECK_OK(broker.value()->set_policy(basic_policy(fixture)));
    BB_CHECK_OK(broker.value()->publish_capacity(fixture.snapshot(1000)));
    BB_CHECK_OK(broker.value()->submit(fixture.request(spec(1, 0, 600, 600, 3))));
    BB_REQUIRE(broker.value()->arbitrate(fixture.target()).ok());
    const auto grants = broker.value()->grants(fixture.target(), true);
    BB_REQUIRE(grants.ok());
    BB_REQUIRE(grants.value().size() == 1);
    before = grants.value()[0];
  }

  auto restarted = Broker::open(durable_config(fixture, store.path()));
  if (!restarted.ok()) {
    BB_FAIL(std::string("restart failed: ") + to_string(restarted.code()) + " " + restarted.error().message);
    return;
  }
  const auto epoch = restarted.value()->status().epoch;

  // The publisher must republish under the new epoch with a fresh generation.
  Fixture republication = fixture;
  republication.epoch = epoch;
  republication.coordinator = fixture.coordinator;
  republication.capacity_generation = CapacitySnapshotGeneration::from_value(2);
  republication.tick = 50;
  CapacitySnapshot snapshot = republication.snapshot(1000);
  snapshot.authority.publisher_boot = boot_id_from_u64(9, 9);
  BB_CHECK_OK(restarted.value()->publish_capacity(snapshot));

  const auto grants = restarted.value()->grants(fixture.target(), true);
  BB_REQUIRE(grants.ok());
  BB_REQUIRE(grants.value().size() == 1);
  const auto revalidated = restarted.value()->revalidate(grants.value()[0].id, grants.value()[0].generation);
  BB_REQUIRE(revalidated.ok());
  BB_CHECK(revalidated.value().authoritative);
  BB_CHECK(revalidated.value().grant.generation.value() > before.generation.value());
  BB_CHECK_EQ(revalidated.value().grant.authority.fabric_epoch, epoch);
  BB_CHECK_EQ(revalidated.value().grant.authority.capacity_generation.value(), std::uint64_t{2});
}

BB_TEST(Broker, FencesAndPolicySurviveRestart) {
  Fixture fixture;
  TempStore store("fences");
  const auto boot = boot_id_from_u64(5, 5);
  {
    auto broker = Broker::open(durable_config(fixture, store.path()));
    BB_REQUIRE(broker.ok());
    BB_CHECK_OK(broker.value()->set_policy(basic_policy(fixture)));
    BB_CHECK_OK(broker.value()->fence_publisher(PublisherId::from_value(5), boot, "killed"));
  }
  auto restarted = Broker::open(durable_config(fixture, store.path()));
  if (!restarted.ok()) {
    BB_FAIL(std::string("restart failed: ") + to_string(restarted.code()) + " " + restarted.error().message);
    return;
  }
  const auto is_fenced = restarted.value()->is_fenced(PublisherId::from_value(5), boot);
  BB_REQUIRE(is_fenced.ok());
  BB_CHECK(is_fenced.value());
  const auto fences = restarted.value()->fences();
  BB_REQUIRE(fences.ok());
  BB_CHECK_EQ(fences.value().size(), std::size_t{1});
  BB_CHECK_EQ(restarted.value()->status().policies, std::size_t{1});
}

BB_TEST(Broker, ObligationsSurviveRestartAndRemainReserved) {
  Fixture fixture;
  TempStore store("obligations");
  Obligation obligation;
  obligation.reservation = ReservationReferenceId::from_value(77);
  obligation.generation = ReservationGeneration::initial();
  obligation.target = fixture.target();
  obligation.amount = bb_fixture::bw(400);
  obligation.lendable = true;
  obligation.max_lend_permille = 500;
  obligation.provenance = fixture.provenance(NodeKind::Operator);
  {
    auto broker = Broker::open(durable_config(fixture, store.path()));
    BB_REQUIRE(broker.ok());
    BB_CHECK_OK(broker.value()->set_policy(basic_policy(fixture)));
    BB_CHECK_OK(broker.value()->upsert_obligation(obligation));
  }
  auto restarted = Broker::open(durable_config(fixture, store.path()));
  if (!restarted.ok()) {
    BB_FAIL(std::string("restart failed: ") + to_string(restarted.code()) + " " + restarted.error().message);
    return;
  }
  BB_CHECK_EQ(restarted.value()->status().obligations, std::size_t{1});
  const auto restored = restarted.value()->obligations(fixture.target());
  BB_REQUIRE(restored.ok());
  BB_REQUIRE(restored.value().size() == 1);
  BB_CHECK(restored.value()[0] == obligation);
}

BB_TEST(Broker, CorruptedJournalTailIsTruncatedNotGuessed) {
  Fixture fixture;
  TempStore store("corrupt");
  {
    auto broker = Broker::open(durable_config(fixture, store.path()));
    BB_REQUIRE(broker.ok());
    BB_CHECK_OK(broker.value()->set_policy(basic_policy(fixture)));
    BB_CHECK_OK(broker.value()->publish_capacity(fixture.snapshot(1000)));
    BB_CHECK_OK(broker.value()->flush());
  }

  // Damage the newest journal by appending a partial record: recovery must stop
  // there, keep everything before it and report the truncation.
  {
    std::FILE* file = nullptr;
    for (const auto& entry : fs::directory_iterator(store.path())) {
      const std::string name = entry.path().filename().string();
      if (name.rfind("journal-", 0) == 0) {
        file = std::fopen(entry.path().string().c_str(), "ab");
        break;
      }
    }
    BB_REQUIRE(file != nullptr);
    const unsigned char garbage[9] = {0x42, 0x42, 0x52, 0x52, 0x00, 0x01, 0x00, 0x00, 0x2A};
    BB_REQUIRE(std::fwrite(garbage, 1, sizeof(garbage), file) == sizeof(garbage));
    std::fclose(file);
  }

  auto restarted = Broker::open(durable_config(fixture, store.path()));
  if (!restarted.ok()) {
    BB_FAIL(std::string("restart failed: ") + to_string(restarted.code()) + " " + restarted.error().message);
    return;
  }
  const CoordinatorStatus status = restarted.value()->status();
  BB_CHECK(status.durable);
  BB_CHECK_EQ(status.policies, std::size_t{1});
  BB_CHECK_EQ(status.resources, std::size_t{1});
  BB_CHECK_EQ(status.journal_truncations, std::uint64_t{1});
}

BB_TEST(Broker, AdvanceEpochStalesEverything) {
  Fixture fixture;
  auto broker = Broker::open(memory_config(fixture));
  BB_REQUIRE(broker.ok());
  BB_CHECK_OK(broker.value()->set_policy(basic_policy(fixture)));
  BB_CHECK_OK(broker.value()->publish_capacity(fixture.snapshot(1000)));
  BB_CHECK_OK(broker.value()->submit(fixture.request(spec(1, 0, 700, 700, 3))));
  BB_REQUIRE(broker.value()->arbitrate(fixture.target()).ok());
  BB_CHECK_EQ(broker.value()->status().live_grants, std::size_t{1});

  const auto advanced = broker.value()->advance_epoch();
  BB_REQUIRE(advanced.ok());
  BB_CHECK(advanced.value().value() > fixture.epoch.value());
  const CoordinatorStatus status = broker.value()->status();
  BB_CHECK_EQ(status.live_grants, std::size_t{0});
  BB_CHECK_EQ(status.revalidation_required, std::size_t{1});

  // Submitting against the superseded epoch is refused.
  BB_CHECK_ERR(ErrorCode::EpochMismatch, broker.value()->submit(fixture.request(spec(2, 0, 10, 10, 3))));
}

BB_TEST(Broker, RevokeReleasesCapacity) {
  Fixture fixture;
  auto broker = Broker::open(memory_config(fixture));
  BB_REQUIRE(broker.ok());
  BB_CHECK_OK(broker.value()->set_policy(basic_policy(fixture)));
  BB_CHECK_OK(broker.value()->publish_capacity(fixture.snapshot(1000)));
  BB_CHECK_OK(broker.value()->submit(fixture.request(spec(1, 0, 900, 900, 3))));
  BB_REQUIRE(broker.value()->arbitrate(fixture.target()).ok());
  const auto grants = broker.value()->grants(fixture.target(), true);
  BB_REQUIRE(grants.ok());
  BB_REQUIRE(grants.value().size() == 1);

  const auto revoked = broker.value()->revoke(grants.value()[0].id, grants.value()[0].generation,
                                              OutcomeReason::Revoked, "operator revoke");
  BB_REQUIRE(revoked.ok());
  BB_CHECK_EQ(revoked.value().grant.state, GrantState::Revoked);
  const auto accounting = broker.value()->accounting(fixture.target());
  BB_REQUIRE(accounting.ok());
  BB_CHECK_EQ(accounting.value().authorized_consumption.bits_per_second(), 0);
  BB_CHECK_EQ(accounting.value().unallocated.bits_per_second(), 1000);
}

BB_TEST(Broker, RetiringARequestRevokesItsGrant) {
  Fixture fixture;
  auto broker = Broker::open(memory_config(fixture));
  BB_REQUIRE(broker.ok());
  BB_CHECK_OK(broker.value()->set_policy(basic_policy(fixture)));
  BB_CHECK_OK(broker.value()->publish_capacity(fixture.snapshot(1000)));
  BB_CHECK_OK(broker.value()->submit(fixture.request(spec(1, 0, 400, 400, 3))));
  BB_REQUIRE(broker.value()->arbitrate(fixture.target()).ok());
  BB_CHECK_OK(broker.value()->retire_request(BandwidthRequestId::from_value(1),
                                             BandwidthRequestGeneration::initial()));
  const auto summary = broker.value()->arbitrate(fixture.target());
  BB_REQUIRE(summary.ok());
  BB_CHECK_EQ(summary.value().recalls, std::size_t{1});
  BB_CHECK_EQ(summary.value().accounting.authorized_consumption.bits_per_second(), 0);
  const auto live = broker.value()->grants(fixture.target(), true);
  BB_REQUIRE(live.ok());
  BB_CHECK(live.value().empty());
}

BB_TEST(Broker, ManyRequestsCloseAccountingAcrossRounds) {
  Fixture fixture;
  auto broker = Broker::open(memory_config(fixture));
  BB_REQUIRE(broker.ok());
  Policy policy = basic_policy(fixture, 4);
  BB_CHECK_OK(broker.value()->set_policy(policy));
  BB_CHECK_OK(broker.value()->publish_capacity(fixture.snapshot(10'000)));

  for (std::uint64_t id = 1; id <= 40; ++id) {
    RequestSpec request = spec(id, 100, 500, 900, 1 + (id % 3));
    request.group = 1 + (id % 4);
    request.tenant = 1 + (id % 4);
    BB_REQUIRE(broker.value()->submit(fixture.request(request)).ok());
  }
  for (int round = 0; round < 5; ++round) {
    const auto summary = broker.value()->arbitrate(fixture.target());
    BB_REQUIRE(summary.ok());
    BB_CHECK(summary.value().accounting.validate().empty());
    BB_CHECK(summary.value().accounting.authorized_consumption <=
             summary.value().accounting.effective_physical);
  }
  // Releasing every live grant must return the accounting to its baseline.
  const auto live = broker.value()->grants(fixture.target(), true);
  BB_REQUIRE(live.ok());
  for (const Grant& grant : live.value()) {
    BB_REQUIRE(broker.value()->release(grant.id, grant.generation).ok());
  }
  const auto accounting = broker.value()->accounting(fixture.target());
  BB_REQUIRE(accounting.ok());
  BB_CHECK_EQ(accounting.value().authorized_consumption.bits_per_second(), 0);
  BB_CHECK_EQ(accounting.value().unallocated.bits_per_second(), 10'000);
  BB_CHECK(accounting.value().validate().empty());
}
