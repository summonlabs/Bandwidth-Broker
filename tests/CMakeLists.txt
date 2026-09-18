add_library(bb_test_support STATIC support/test_main.cpp)
target_include_directories(bb_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bb_test_support PUBLIC BandwidthBroker::bandwidth_broker)
bb_configure_target(bb_test_support)

function(bb_add_test target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE bb_test_support)
  bb_configure_target(${target})
  add_test(NAME ${target} COMMAND ${target})
endfunction()

bb_add_test(bb_test_smoke test_smoke.cpp)
bb_add_test(bb_test_fairness test_fairness.cpp)
bb_add_test(bb_test_wire test_wire.cpp)
bb_add_test(bb_test_arbitration test_arbitration.cpp)
bb_add_test(bb_test_broker test_broker.cpp)
bb_add_test(bb_test_hardening test_hardening.cpp)

# The multiprocess suite drives the real coordinator and requester executables.
add_executable(bb_test_multiprocess test_multiprocess.cpp)
target_link_libraries(bb_test_multiprocess PRIVATE bb_test_support)
bb_configure_target(bb_test_multiprocess)
target_compile_definitions(bb_test_multiprocess PRIVATE
  BB_COORDINATOR_PATH="$<TARGET_FILE:bb_coordinator>"
  BB_REQUESTER_PATH="$<TARGET_FILE:bb_requester>")
add_dependencies(bb_test_multiprocess bb_coordinator bb_requester)
add_test(NAME bb_test_multiprocess COMMAND bb_test_multiprocess)
