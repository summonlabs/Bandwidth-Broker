# Independent downstream consumer.
#
# This project is deliberately NOT part of the Bandwidth Broker build: release
# validation configures it on its own, against an installed prefix, so that it
# can only succeed if the exported package is complete and correct.

cmake_minimum_required(VERSION 3.20)
project(BandwidthBrokerConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(BandwidthBroker 1.0.0 REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE BandwidthBroker::bandwidth_broker)
