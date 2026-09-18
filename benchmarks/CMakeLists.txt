add_executable(bb_bench_arbitration bb_bench_arbitration.cpp)
target_link_libraries(bb_bench_arbitration PRIVATE BandwidthBroker::bandwidth_broker)
bb_configure_target(bb_bench_arbitration)

if(BB_BUILD_TESTS)
  add_test(NAME bb_bench_smoke COMMAND bb_bench_arbitration --quick)
endif()
