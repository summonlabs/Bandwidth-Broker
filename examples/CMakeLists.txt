add_executable(bb_example_arbitrate arbitrate_example.cpp)
target_link_libraries(bb_example_arbitrate PRIVATE BandwidthBroker::bandwidth_broker)
bb_configure_target(bb_example_arbitrate)

if(BB_BUILD_TESTS)
  add_test(NAME bb_example_runs COMMAND bb_example_arbitrate)
endif()
