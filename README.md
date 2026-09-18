# Bandwidth Broker

**Vendor-neutral C++20 runtime for deterministic arbitration of scarce fabric bandwidth.**

Bandwidth Broker answers one question, completely and auditably:

> Given authoritative available bandwidth, existing obligations, competing bandwidth requests, priorities,
> fairness groups, policy and exact resource generations — who may receive bandwidth now, how much may each
> claimant receive, what remains unallocated, which requests must wait or be refused, and when must a grant be
> recalled, fenced, revalidated, superseded or rejected as stale?

It produces **authoritative allocation state**. It does not move a single packet.

```text
        capacity evidence            obligations                 competing requests
     (external authority)      (Bandwidth Reservation Fabric)   (requesters, in policy order)
                |                          |                            |
                +--------------------------+----------------------------+
                                           v
                                 +---------------------+
                                 |   Bandwidth Broker  |
                                 |   (this runtime)    |
                                 +---------------------+
                                           |
                        authoritative grants, recalls, accounting, explanations
                                           |
              +----------------------------+----------------------------+
              v                            v                            v
      Traffic Engineering          Flow Scheduler                Rate Governor
      Fabric (capacity)            (temporal order)              (enforcement)
```

---

## 1. Systems boundary

Bandwidth Broker **owns** arbitration of scarce fabric bandwidth among competing requesters under explicit
policy, priority, fairness, capacity, obligation, generation and authority constraints. It owns authoritative
grant state, grant lifecycle, generation-bound authority, recall state, capacity accounting, resolution of
competing claims, explanations for every outcome, and the durable policy/history/fencing state that belongs to
it.

Bandwidth Broker **does not own** and never implements:

| Responsibility | Owned by |
| --- | --- |
| Physical link discovery | Fabric inventory / link discovery |
| Topology truth, link-state truth | Topology and link-state services |
| Path computation, path legality, route lifecycle | Routing / Traffic Engineering Fabric |
| Global traffic-engineering optimisation | Traffic Engineering Fabric |
| Future and durable reservation lifecycle | Bandwidth Reservation Fabric |
| Packet scheduling, queue scheduling | Queue/packet schedulers |
| Rate enforcement of an allocation | Rate Governor |
| Congestion control | Transport / congestion control |
| Telemetry collection | Telemetry pipeline |
| Forwarding and device programming | Device/SDN controllers |

Consequences that are enforced in code, not just in prose:

* **Capacity is not a grant.** Observed or configured bandwidth never authorises consumption. A resource with
  no current evidence has UNKNOWN capacity, and UNKNOWN is never treated as spare capacity.
* **A grant is not enforcement.** Bandwidth Broker produces allocation state. Adjacent runtimes apply it.
* **Obligations are not grants.** A committed reservation (owned by the Bandwidth Reservation Fabric) is
  imported as an obligation and withheld from arbitration; it is only ever consumed by grants that bind the
  same reservation reference *and* generation.
* **No worker, session or lease authority is invented.** Nothing that was not explicitly published is assumed.

### 1.1 Bandwidth Broker versus the Bandwidth Reservation Fabric

The Bandwidth Reservation Fabric owns **durable and future** commitments: it decides that a reservation
exists, for how long, and under what commercial or contractual terms. Bandwidth Broker only imports the
resulting capacity obligation so that present-tense arbitration cannot over-allocate over it. An obligation
here is a fact about *now*: an amount, a target, a reservation reference and a reservation generation. The
reservation's lifecycle (creation, extension, cancellation) is never driven from this runtime — the
`upsert_obligation` / `retire_obligation` API reflects decisions made elsewhere.

### 1.2 Bandwidth Broker versus the Traffic Engineering Fabric

The Traffic Engineering Fabric consumes brokered capacity and obligations. It is free to use them for path
placement, global optimisation and rerouting. It must not assume that a grant is stable: grants carry a
lifecycle state and a generation, and a grant may be recalled, revoked, fenced, expired or superseded.
Bandwidth Broker never computes a path, never validates path legality, and never programs a device.

### 1.3 Grant versus enforcement

```text
Bandwidth Broker says:  "request 42, under policy generation 3 and capacity generation 7,
                         may consume 4 000 000 bit/s: 2 000 000 guaranteed, 2 000 000 discretionary."
Rate Governor says:     "the shaper for request 42 is set to 4 000 000 bit/s."
```

Only the first statement is made here. A grant states what is authorised, never that traffic was shaped,
policed or admitted.

---

## 2. Core principles

1. **Capacity is not a grant.** Only a committed arbitration decision produces a grant.
2. **A grant is not enforcement.**
3. **Arbitration is policy.** Priority, fairness, borrowing, recall, starvation and headroom rules are explicit
   validated policy fields, never incidental code paths.
4. **Existing obligations matter.** Committed and higher-authority allocations are represented explicitly and
   withheld before anything is arbitrated.
5. **Borrowed bandwidth is revocable.** A borrower can never gain the authority semantics of an owner.
6. **Fairness must be deterministic.** Equivalent state produces equivalent grants and identical ordering.
7. **No silent oversubscription.** Authoritative grants never exceed authoritative allocatable capacity unless
   controlled oversubscription is explicitly enabled, and then guaranteed and contingent capacity stay
   distinct in every view.
8. **Grants are generation-bound.** A grant binds the exact capacity, policy, requester, reservation and control-plane
   generations that justified it.
9. **Recall is a state transition, not deletion.**
10. **UNKNOWN does not become spare capacity.**

---

## 3. What is implemented

### 3.1 Strongly typed identities, generations and provenance

`BandwidthResourceId`, `BandwidthPoolId`, `BandwidthRequestId`, `BandwidthGrantId`, `TenantId`,
`FairnessGroupId`, `PriorityClassId`, `PolicyId`, `ReservationReferenceId`, `FabricEpoch`,
`CoordinatorIncarnation`, `PublisherId`, `WorkerId`, `AttemptId`, `SessionId`, `DecisionId`,
`RecallId`, `AuditSequence`, `CapacitySnapshotId` — each a distinct type; value `0` is reserved for
"absent" and can never be produced by an allocation path. Matching generation types
(`BandwidthResourceGeneration`, `CapacitySnapshotGeneration`, `PolicyGeneration`, `ReservationGeneration`,
`FairnessConfigGeneration`, `TenantConfigGeneration`, `BandwidthRequestGeneration`,
`BandwidthGrantGeneration`) never wrap; advancing past the end of the space fails instead.

Every authority-bearing record also carries a `Provenance` (source kind, source identity, boot identity,
fabric epoch, coordinator incarnation, monotonic source sequence, audit timestamp).

### 3.2 Exact capacity units

`Bandwidth` is an exact fixed-point quantity whose quantum is one bit per second, range
`[0, 10^15]` bit/s. All arithmetic is checked: overflow, underflow and out-of-range values are rejected, never
clamped. There is no floating point anywhere in an authority-bearing path.

### 3.3 Request model

A request expresses target resource/pool (with generations), minimum, desired and maximum bandwidth, guarantee
class, priority class, tenant, fairness group, borrowing eligibility, preemptibility, recall tolerance, an
optional reservation binding, an optional tick-based effective window, a latency/SLO class reference, policy
labels, requester generation and provenance. Contradictory requests are rejected deterministically:

* minimum > maximum, desired > maximum, desired < minimum;
* an explicitly guaranteed request with a zero minimum, or a best-effort request with a non-zero minimum;
* a borrowing request that is not preemptible, or that declares `NoRecall` (borrowed bandwidth is always
  revocable);
* a non-preemptible request that declares a recall tolerance other than `NoRecall`;
* a reservation-bound request that is not a guaranteed request;
* a window whose end tick is not after its start tick;
* labels that are unsorted, duplicated, over-long, non-UTF-8 or contain control characters;
* a submission that pre-fills coordinator-owned authority fields.

### 3.4 Capacity model

```text
effective_physical = physical_configured - administratively_unavailable - degraded_loss
```

All three terms are explicit, and a publisher that knows only part of the picture must declare evidence
`Unknown` rather than supply a partial number. A resource whose evidence is not `Known` has **no arbitrable
capacity at all**: every request is explicitly waiting, and existing grants are staled.

### 3.5 Policy model

```text
SchedulingMode          StrictPriority | WeightedFairShare
PriorityClass           id, name, rank, weight, emergency, preemptible, borrowing_eligible, allow_discretionary
FairnessGroupConfig     id, tenant, name, weight, absolute cap, cap permille, borrowing, preemptible
BorrowPolicy            enabled, lend_obligations, max_lend_permille, max_borrow_permille, borrow_from_headroom
StarvationPolicy        enabled, aging_threshold_rounds, aging_weight_multiplier, maximum_promotions
PreemptionPolicy        enabled, recall_discretionary, recall_grace_rounds
OversubscriptionPolicy  numerator / denominator (1/1 disables it)
                        headroom_target, headroom_permille, emergency_reserve_permille,
                        minimum_allocation_quantum, hysteresis_rounds,
                        fairness_config_generation, tenant_config_generation
```

Policy is validated on installation: unique class ids and ranks, bounded weights (1 … 1 000 000), unique group
ids, caps within range, headroom and emergency reserve that cannot withhold more than all capacity, borrowing
parameters that are consistent with the borrowing switch, bounded aging and oversubscription ratios.

### 3.6 Arbitration precedence (exact)

1. **Authority validation.** Every request is checked against the current fabric epoch, resource generation,
   policy generation, fairness and tenant configuration generations, priority class, fairness group, tenant
   ownership, reservation generation and boot fence table. A mismatch is refused with a specific reason and
   consumes no capacity.
2. **Capacity evidence.** Only `Known` evidence is usable. Otherwise the round produces an explicitly empty
   accounting and every candidate waits.
3. **Hard obligations** are withheld from the allocatable pool before anything is arbitrated.
4. **Emergency reserve** is withheld for classes marked `emergency`.
5. **Headroom** (`max(headroom_target, allocatable × headroom_permille)`) is withheld and is never granted.
6. **Guaranteed minima** are satisfied in descending effective priority rank; ties break on fairness-group
   weight, fairness-group id, tenant id, request id, request generation.
7. **Discretionary capacity** is distributed toward each request's `desired` amount, then toward `maximum`.
   Under `StrictPriority` the strongest class present is served first; under `WeightedFairShare` one weighted
   max-min fair share is computed across all groups. Inside a class (or a group) capacity is shared by
   hierarchical weighted max-min fairness over indivisible quanta.
8. **Emergency classes are served before every other class** and may draw on the withheld reserve.
9. **Borrowing** may lend a bounded share of *unused, lendable obligation capacity* to revocable requests only.
10. **Contingent capacity** is created only when oversubscription is explicitly enabled, and is never
    guaranteed, never counted as backed capacity and never granted to an irrevocable request.
11. **Recalls** are computed against the previous committed state; the accounting identity is re-checked before
    the decision set is returned.

### 3.7 Weighted max-min fairness

The engine implements **exact hierarchical weighted max-min fairness by progressive filling** in integer
arithmetic over indivisible quanta. Properties proven by the test suite:

* **Conservation** — sum of allocations plus the unallocated remainder equals the offered capacity exactly.
* **Monotonicity** — increasing offered capacity never decreases any allocation.
* **Demand safety** — no allocation exceeds its demand.
* **Quantum integrity** — every allocation is a whole multiple of the quantum; leftover below one quantum is
  reported as unallocated rather than invented.
* **Determinism** — indivisible quanta are distributed by (largest remainder, lowest identifier), so identical
  input produces identical output.
* **Isolation** — a branch's allocation depends only on its own weight and demand plus its peers' aggregate.

Starvation prevention is policy: a request unsatisfied for `aging_threshold_rounds` consecutive rounds has its
effective rank promoted by up to `maximum_promotions` steps and its effective weight multiplied by
`aging_weight_multiplier` (bounded by the maximum weight). Under weighted fair sharing with all weights at
least 1 and at least one quantum of capacity per active demand, every active demand receives at least one
quantum. Under strict priority a lower class can be starved by a persistently unsatisfied higher class; aging
is what bounds that wait, and the exact assumptions are stated above rather than claimed away.

### 3.8 Borrow and recall

An obligation may be marked *lendable* up to a policy-bounded share. Unused lendable obligation capacity is
offered to requests that opt into borrowing, that are preemptible, and whose class or group also permits
borrowing. Borrowed capacity is reported separately from guaranteed and discretionary capacity in the grant,
the accounting and the explanation.

When the owner arrives, or capacity is withdrawn, or policy changes, the borrowing request's allocation shrinks.
The runtime never silently mutates a grant into a smaller amount:

* the grant **generation advances**;
* a `RecallRecord` captures the recalled amount, the retained amount, the reason, the requesting tick and the
  deadline tick, and whether acknowledgement is required;
* if the policy's recall grace is non-zero the grant passes through `RecallPending`, during which it authorises
  **nothing** — the grace period is an acknowledgement window, never extra permission;
* the old generation never authorises consumption again.

### 3.9 Grant lifecycle

```text
REQUESTED -> VALIDATED -> QUEUED -> GRANTED_GUARANTEED | GRANTED_BORROWED
GRANTED_*  -> RECALL_PENDING | RELEASED | EXPIRED | REVOKED | FENCED | STALE | REVALIDATION_REQUIRED
RECALL_PENDING -> RELEASED | EXPIRED | REVOKED | FENCED | STALE | REVALIDATION_REQUIRED
REVALIDATION_REQUIRED -> GRANTED_GUARANTEED | GRANTED_BORROWED | RELEASED | EXPIRED | REVOKED | FENCED | STALE
RELEASED | EXPIRED | REVOKED | FENCED | STALE | REJECTED   (terminal)
```

Every transition is checked against this table; illegal transitions are refused. A grant whose entire
allocation is guaranteed carries `GRANTED_GUARANTEED`; a grant holding any revocable amount (discretionary,
borrowed or contingent) carries `GRANTED_BORROWED` while its breakdown stays explicit in
`GrantAllocation{guaranteed, discretionary, borrowed, contingent}`. Terminal grants retain their final
allocation as history — accounting is driven by the lifecycle state, never by the presence of a stored amount.

### 3.10 Idempotency

Submitting the same `(request id, request generation)` with byte-identical canonical content returns the
recorded outcome and changes nothing. The same identity with different content is refused with
`IdentityConflict`. Releasing a grant twice is a no-op that reports `already_released` and never applies the
release to accounting twice. Idempotency entries are durable, so a duplicate submission after a coordinator
restart is still detected.

### 3.11 Authority and stale rejection

Every authority-bearing mutation revalidates first. A decision is refused as stale if any bound item changed:
fabric epoch, resource generation, capacity snapshot generation, policy generation, requester boot,
reservation generation, fairness or tenant configuration generation. A grant can only be used with its **current**
generation: querying, releasing or revoking with a superseded generation is refused and authorises nothing.

### 3.12 Persistence

A store directory contains a versioned manifest, at most two full snapshots and append-only journals. Every
record carries a magic value, a format version, its audit sequence, tick, fabric epoch and coordinator
incarnation, plus a CRC-32C over the record body; snapshots carry a header checksum and a payload checksum.
Writes go through `validate → bind authority → plan → reserve → journal → apply → commit`, and a mutation is
acknowledged only after its record has been written, flushed and (by default) synced.

Recovery replays the newest intact snapshot and then every intact journal record after it. A damaged newest
snapshot falls back to the previous one. A damaged journal tail is **truncated and reported**, never guessed.
The store is compacted automatically when the journal reaches its configured bound, and journals are retired by
the snapshot that covers them.

Recovery distinguishes, and never conflates:

| Category | Behaviour after restart |
| --- | --- |
| Durable configuration and history | Restored (policy, obligations, requests, fences, idempotency, audit sequence) |
| Committed authoritative state | Restored as data, never as live authority |
| Live grants | Moved to `REVALIDATION_REQUIRED` |
| Capacity evidence | Demoted to `STALE`; a fresh publication is required |
| Coordinator authority | Fabric epoch advanced by one, new incarnation |

Nothing that was live becomes live again by being reloaded. A revalidated grant gets a **new generation** bound
to the new epoch, the new coordinator incarnation and the current capacity generation, and revalidation is
refused entirely for a fenced boot.

### 3.13 Distributed protocol

Real framed TCP between real OS processes. A frame is a 32-byte header (magic, protocol version, message type,
flags, payload length, correlation id, header CRC-32C, payload CRC-32C) followed by a canonical big-endian
payload. Decoding rejects bad magic, unsupported versions, undefined message types, reserved flags, oversized
payloads, payload checksum mismatches, unknown enum values, over-long length prefixes, truncated fields and
trailing bytes. The stream decoder bounds buffered-but-incomplete input to one frame.

The coordinator server bounds connections, buffers and per-message vector sizes, handles each connection on its
own thread, polls its stop flag rather than relying on cross-thread socket teardown, and joins nothing while
holding state a worker needs. The client is one process incarnation: its boot identity is its fencing token.

The optional session token is a **bearer-token gate, not a credential**, and the transport is **not
encrypted**. See Limitations.

### 3.14 Public API

```cpp
// broker.hpp — the coordinator runtime
Broker::open(config)
set_policy / policy
publish_capacity / capacity
upsert_obligation / retire_obligation / obligations
submit / retire_request
arbitrate
query_grant / grants / release / revoke / revalidate
fence_publisher / is_fenced / fences
explain / accounting / accounting_all / status / targets
advance_epoch / flush

// arbitrator.hpp — the pure arbitration engine (no I/O, no locks, no callbacks)
arbitrate(input) -> ArbitrationOutcome
validate_arbitration_input(input)

// fairness.hpp — exact weighted max-min fairness
weighted_max_min_fair_share(capacity, quantum, leaves | branches)

// persistence.hpp — versioned, checksummed durable state
Store::open / recover / append / flush / compact / inspect

// server.hpp, client.hpp, net.hpp — real framed TCP client and coordinator server
```

`arbitrate()` is a pure function: identical input produces byte-identical output, which is what makes the whole
runtime property-testable.

### 3.15 Explanations

Every request outcome is explainable without re-running arbitration. `RequestExplanation` carries the exact
request and generations, the effective capacity, the obligations applied before arbitration, the fairness group
and priority, the guaranteed / discretionary / borrowed / contingent breakdown, the denied amount, whether the
request is waiting or refused, the binding reason, the recall status (amount, reason, deadline), provenance and
the full authority vector. Every string is bounded and every list is bounded, so an explanation can never
amplify memory or bandwidth.

---

## 4. Accounting invariants

For every resource, after every authoritative mutation, when capacity evidence is `Known`:

```text
(i)   effective_physical + capacity_deficit
          == obligations_reserved + allocatable
(ii)  allocatable
          == emergency_reserve + headroom + arbitrable
(iii) allocatable
          == emergency_reserve_unused + headroom
             + guaranteed_granted + discretionary_granted + unallocated
(iv)  obligations_reserved
          == obligations_consumed + obligations_lent + obligations_idle
(v)   contingent_pool == contingent_granted + contingent_unallocated
(vi)  borrowed_granted == obligations_lent
(vii) authorized_consumption
          == obligations_consumed + guaranteed_granted + discretionary_granted
             + borrowed_granted + contingent_granted
(viii) authorized_consumption <= effective_physical + contingent_pool
```

All quantities are non-negative by construction and no term can overflow. The invariant set is re-checked after
every arbitration round, after every out-of-round release, revocation and fencing, and inside the property,
concurrency and adversarial test suites. Out-of-round retirement is exactly reversible because each grant
records the funding provenance of its own allocation (`obligation_backed`, `reserve_backed`).

---

## 5. Building

Requirements: CMake ≥ 3.20, a C++20 compiler, and a build generator.

```text
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Options: `BB_BUILD_TESTS`, `BB_BUILD_TOOLS`, `BB_BUILD_BENCHMARKS`, `BB_BUILD_EXAMPLES`,
`BB_WARNINGS_AS_ERRORS` (default ON), `BB_ENABLE_ASAN`.

No test, benchmark or example in this repository is wrapped in a timeout. A hanging test is a defect to
diagnose, never something to kill and mark passing.

### 5.1 Installing and consuming

```text
cmake --install build --prefix /path/to/prefix
```

An independent downstream project then uses the exported package:

```cmake
find_package(BandwidthBroker 1.0.0 REQUIRED)
target_link_libraries(my_service PRIVATE BandwidthBroker::bandwidth_broker)
```

`tests/consumer/` contains exactly such a project; it is configured, built and executed against an installed
prefix as part of release validation, not against the build tree.

### 5.2 Command line tools

```text
bb_coordinator --port 0 --store <dir> --incarnation 1 --epoch 1 --token <token>
bb_requester   --host 127.0.0.1 --port <port> --token <token> --publisher 2 \
               --request-id 100 --min 0 --desired 400000 --max 400000 --priority 3 [--hold]
bb_inspect     <store-directory>
```

`bb_coordinator` prints one machine-readable `READY <port> <epoch> <incarnation>` line, then serves real TCP
until it is asked to stop. `bb_requester` performs a complete handshake/submit/arbitrate cycle and reports the
resulting grant; with `--hold` it stays alive so a test can hard-kill a live incarnation.

### 5.3 Example

```text
./build/examples/bb_example_arbitrate
```

Opens a coordinator, installs a two-class / two-tenant policy with preserved headroom, publishes 10 Mbit/s,
submits three competing requests, arbitrates, prints the full accounting and one explanation per request, then
releases everything and prints the returned-to-baseline accounting.

---

## 6. Validation

### 6.1 What is real, what is synthetic

* **REAL** — multiprocess behaviour: the coordinator and requesters are separate OS processes exchanging framed
  bytes over a real TCP socket on the loopback interface; requesters and the coordinator are terminated with
  the platform's hard-kill primitive and restarted against the same on-disk store.
* **REAL** — persistence: real files, real CRC-32C integrity checks, real torn-tail truncation, real restart.
* **REAL** — concurrency: real threads racing submission, arbitration, release and revocation against one
  coordinator.
* **SYNTHETIC** — every benchmark. Request populations, capacities, obligation densities and group counts are
  generated. Benchmark numbers are **not** physical-network performance and must never be presented as such.
* **UNSUPPORTED** — there is no physical-fabric, NIC, switch, DPU, RDMA, NVLink or optical validation of any
  kind, because this runtime does not touch a network data plane. Its only network use is its own control
  protocol.

### 6.2 Test suites

| Suite | Proves |
| --- | --- |
| `bb_test_smoke` | Exact integer arithmetic, CRC-32C vectors, identity and generation rules |
| `bb_test_fairness` | Conservation, monotonicity, demand safety, quantum determinism, hierarchical isolation, overflow safety |
| `bb_test_arbitration` | Precedence, guarantees, caps, headroom, emergency reserve, obligations, unknown capacity, staleness refusal, duplicates, contradictions, borrow, recall, oversubscription, determinism, order independence, starvation promotion |
| `bb_test_broker` | Policy generations, idempotency and identity conflict, release and double release, stale-generation refusal, fencing, durable restart with epoch advance, revalidation, journal corruption truncation, epoch advancement, revocation, retirement, accounting closure across many rounds |
| `bb_test_wire` | Canonical round trips for every model and message type, exact header and payload bounds, every-length truncation, oversized length fields before allocation, unknown enums, trailing bytes, arbitrary chunking, corrupted-frame detection, bounded buffering, 20 000-case fuzz, 4 000-case adversarial bit flips |
| `bb_test_hardening` | Seeded 400-step operation sequences with invariant re-checking, capacity monotonicity, eight-thread racing rounds, duplicate-identity race with exactly one winner, revoke-versus-release race, hostile requests, oversized quantities, huge weights, reservation overcommit, full capacity withdrawal, repeated borrow/recall cycles, fuzzed populations, fenced revalidation |
| `bb_test_multiprocess` | Real processes: handshake, policy and capacity publication, grant issuance, hard kill, permanent fencing, stale-identity refusal, fresh incarnation acceptance, coordinator hard kill and restart with advanced epoch, durable policy survival, capacity demotion to stale, republication and recovery |

### 6.3 Benchmark

`bb_bench_arbitration` measures the **completed** arbitration path (a full round and its decision set) and the
full coordinator commit path, over request count, fairness-group count, obligation density and borrow/recall
pressure. It prints raw totals and per-round times; it deliberately prints no derived "throughput" claim.

### 6.4 Toolchain and static analysis

See `VALIDATION.md` for the exact toolchain, warning levels, sanitizer and static-analysis results, and the
fresh-clone and downstream-consumer transcripts recorded for this release.

---

## 7. Limitations

Stated plainly, because a runtime that hides them cannot be trusted with authority.

1. **Windows is the validated platform.** The POSIX socket, process and file paths are implemented and mirror
   the Windows paths, but this release was built, tested and validated only on Windows x64 with MSVC 14.44
   under `/W4 /WX`. POSIX behaviour is **implemented but unvalidated** here; do not treat it as proven.
2. **The control transport is neither encrypted nor authenticated beyond a bearer token.** Anyone who can reach
   the loopback listener and knows the token can publish capacity, install policy and submit requests. Put a
   mutually authenticated, encrypted transport in front of it before exposing it beyond a trusted host. The
   boot identity is a fencing token, not a credential.
3. **No clock-based expiry is enforced.** Timestamps are recorded for audit only. Ordering, aging and windows
   are expressed in coordinator ticks. A wall-clock change can neither grant nor expire authority, and a
   `recall_deadline_tick` is an acknowledgement window that the coordinator records but does not enforce.
4. **Hysteresis is modelled but not applied.** `Policy::hysteresis_rounds` is validated, carried and
   persisted, but the arbitrator does not yet defer decisions across rounds with it. It is a declared field
   awaiting a defined semantic, and it is listed here rather than described as working.
5. **Oversubscription is bounded to a 16× ratio** and applies to the whole effective physical capacity, not per
   tenant.
6. **The idempotency table is bounded and never evicted.** When it is full, further submissions are refused
   with `ResourceExhausted` rather than silently dropping the ability to detect duplicates.
7. **Recovery requires an intact manifest.** If store files exist without their manifest, recovery refuses
   rather than guessing the store's provenance.
8. **A single coordinator is authoritative.** There is no leader election, no replication and no consensus;
   `CoordinatorIncarnation` and `FabricEpoch` exist precisely so that a restart is detectable and stale
   authority is refused.
9. **Explanations are retained for the requests the coordinator restored or accepted**, bounded by the store
   and memory configuration, not as an unbounded audit log.
10. **The benchmark is synthetic**, as stated in §6.1.

---

## 8. License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
