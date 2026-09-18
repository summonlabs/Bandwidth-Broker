# Release validation record — Bandwidth Broker 1.0.0

Everything in this file was executed on the platform and toolchain stated below. Nothing here is projected,
assumed or carried over from a different environment.

## Toolchain

| Item | Value |
| --- | --- |
| Operating system | Windows 10.0.26100 (x64) |
| Compiler | Microsoft C/C++ 19.44.35222.0 (MSVC 14.44.35207, x64) |
| CMake | 4.3.2 |
| Generator | Ninja |
| C++ standard | C++20 |
| Warning level | `/W4 /permissive- /WX` (warnings are errors) |

## Configuration matrix

| Configuration | Command | Result |
| --- | --- | --- |
| Release | `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release` | Build clean, zero warnings |
| Debug | `cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug` | Build clean, zero warnings |
| AddressSanitizer | `cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBB_ENABLE_ASAN=ON` | Build clean; `/fsanitize=address` present on 31 compile commands and 10 link commands |
| Static analysis | `cmake -S . -B build-analyze -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS=/analyze"` | Build clean |

### Static analysis detail

`/analyze` reported **zero first-party findings**. Two warnings were emitted from Windows SDK headers that
this project includes (`ws2tcpip.h`, C6101 "returning uninitialized memory" on `*Mtu` and `*Enabled`); they
are inside Microsoft's SDK, are not reachable from first-party code paths, and were therefore not masked with
suppressions.

### Sanitizer detail

AddressSanitizer was genuinely enabled for the whole library, all test executables and every tool. All seven
sanitized test suites passed with no ASan report of any kind:

```text
1/7 bb_test_smoke .......... Passed
2/7 bb_test_fairness ....... Passed
3/7 bb_test_wire ........... Passed
4/7 bb_test_arbitration .... Passed
5/7 bb_test_broker ......... Passed
6/7 bb_test_hardening ...... Passed
7/7 bb_test_multiprocess ... Passed
100% tests passed, 0 tests failed out of 7
```

The benchmark and example targets are excluded from the sanitized configuration because they add no coverage of
the runtime boundary.

## Test results

Release and Debug each ran the complete suite to natural completion. **No test anywhere in this repository is
wrapped in a timeout**, and no timeout parameter was passed to any validation command.

| Suite | Release | Debug | ASan |
| --- | --- | --- | --- |
| `bb_test_smoke` | Passed | Passed | Passed |
| `bb_test_fairness` | Passed | Passed | Passed |
| `bb_test_wire` | Passed | Passed | Passed |
| `bb_test_arbitration` | Passed | Passed | Passed |
| `bb_test_broker` | Passed | Passed | Passed |
| `bb_test_hardening` | Passed | Passed | Passed |
| `bb_test_multiprocess` | Passed | Passed | Passed |
| `bb_bench_smoke` | Passed | Passed | (not built) |
| `bb_example_runs` | Passed | Passed | (not built) |

## Multiprocess evidence

`bb_test_multiprocess` drives the real `bb_coordinator` and `bb_requester` executables through
`CreateProcess` with redirected pipes over a real TCP listener on the loopback interface. It proves, in order:

1. a requester process completes a handshake and receives a real grant;
2. the requester is terminated with `TerminateProcess` (hard kill, no cleanup) and reports a non-zero exit;
3. its boot identity is permanently fenced through the protocol;
4. a new connection presenting the fenced boot is refused with `boot_fenced` and exits non-zero;
5. a fresh incarnation from the same logical publisher is accepted and granted capacity;
6. the fenced incarnation's grant contributes nothing to authorised consumption;
7. the coordinator process is hard-killed and restarted against the same store;
8. the restarted coordinator reports an advanced fabric epoch;
9. durable policy, requests and fences survive, while capacity evidence is demoted to `stale` and no grant
   remains live;
10. a fresh capacity publication under the new epoch restores arbitration and grants are issued again.

## Install and downstream consumer

```text
cmake --install build --prefix dist
cmake -S tests/consumer -B build-consumer -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=<abs>/dist"
cmake --build build-consumer
./build-consumer/consumer.exe
```

`tests/consumer` is a separate CMake project that is **not** part of this build. It uses
`find_package(BandwidthBroker 1.0.0 REQUIRED)` and links `BandwidthBroker::bandwidth_broker` from the installed
prefix only. Observed output:

```text
bandwidth-broker version: 1.0.0 (protocol 1, store format 1)
granted=2 waiting=0
authorized=600000 headroom=100000
explanation: request=1/1 target=resource=1/gen=1 state=granted_borrowed reason=satisfied_fully ...
after release authorized=0 unallocated=900000
consumer OK
```

## Benchmark

`bb_bench_arbitration` measures completed rounds (full arbitration plus the resulting decision set) and the
full coordinator commit path. All populations are **synthetic**; the numbers are not physical-network
performance. Sample Release output (`--quick`):

```text
dimension=request_count value=64   requests=64   groups=8  obligations=4 rounds=3 per_round_ms=0.154
dimension=request_count value=256  requests=256  groups=8  obligations=4 rounds=3 per_round_ms=0.245
dimension=group_count   value=16   requests=512  groups=16 obligations=4 rounds=3 per_round_ms=0.448
dimension=obligation_density value=8 requests=512 groups=8 obligations=8 rounds=3 per_round_ms=0.542
dimension=borrow_recall_pressure value=on requests=1024 groups=16 obligations=32 rounds=3 per_round_ms=1.220
dimension=commit_path_requests value=64 requests=64 groups=8 obligations=4 rounds=3 per_round_ms=0.195
```

## Platform coverage statement

Windows x64 with MSVC is the validated platform. The POSIX socket, process and file paths exist in the source
and mirror the Windows paths, but they were **not** built, tested or validated for this release, and no POSIX
result is claimed.
