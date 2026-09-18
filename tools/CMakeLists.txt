# Bandwidth Broker coordinator, requester, inspection tooling, examples and benchmarks.

add_executable(bb_coordinator bb_coordinator.cpp)
target_link_libraries(bb_coordinator PRIVATE BandwidthBroker::bandwidth_broker)
bb_configure_target(bb_coordinator)

add_executable(bb_requester bb_requester.cpp)
target_link_libraries(bb_requester PRIVATE BandwidthBroker::bandwidth_broker)
bb_configure_target(bb_requester)

add_executable(bb_inspect bb_inspect.cpp)
target_link_libraries(bb_inspect PRIVATE BandwidthBroker::bandwidth_broker)
bb_configure_target(bb_inspect)

install(TARGETS bb_coordinator bb_requester bb_inspect RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
