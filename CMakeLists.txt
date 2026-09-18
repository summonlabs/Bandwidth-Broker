cmake_minimum_required(VERSION 3.20)

project(BandwidthBroker
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral C++20 runtime for deterministic arbitration of scarce fabric bandwidth"
  HOMEPAGE_URL "https://github.com/summonlabs/Bandwidth-Broker"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(BB_BUILD_TESTS "Build the Bandwidth Broker test suite" ON)
option(BB_BUILD_TOOLS "Build the Bandwidth Broker coordinator and inspection tools" ON)
option(BB_BUILD_BENCHMARKS "Build the Bandwidth Broker benchmarks" ON)
option(BB_BUILD_EXAMPLES "Build the Bandwidth Broker examples" ON)
option(BB_ENABLE_ASAN "Build with AddressSanitizer where the toolchain supports it" OFF)
option(BB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

find_package(Threads REQUIRED)

# Every Bandwidth Broker target is compiled with the same strict warning set.
function(bb_configure_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus /Zc:preprocessor /Zc:inline /MP
      /wd4514 # unreferenced inline function has been removed by the compiler
      /wd4820 # padding added after data member (ABI-report noise, not a defect signal)
      /wd5045 # Spectre mitigation informational note
      /wd4711 # function selected for automatic inline expansion
      /wd4710 # function not inlined
      /wd4625 /wd4626 /wd5026 /wd5027 # implicitly deleted special members (informational)
    )
    target_compile_definitions(${target} PRIVATE
      _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
    )
    if(BB_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    if(BB_ENABLE_ASAN)
      target_compile_options(${target} PRIVATE /fsanitize=address /Zi /Od)
      target_link_options(${target} PRIVATE /INCREMENTAL:NO)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(BB_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(BB_ENABLE_ASAN)
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

set(BB_LIB_SOURCES
  src/error.cpp
  src/quantity.cpp
  src/checksum.cpp
  src/identity.cpp
  src/text.cpp
  src/capacity.cpp
  src/obligation.cpp
  src/request.cpp
  src/policy.cpp
  src/grant.cpp
  src/accounting.cpp
  src/fairness.cpp
  src/arbitrator.cpp
  src/wire.cpp
  src/explanation.cpp
  src/persistence.cpp
  src/broker.cpp
  src/net.cpp
  src/server.cpp
  src/client.cpp
)

add_library(bandwidth_broker STATIC ${BB_LIB_SOURCES})
add_library(BandwidthBroker::bandwidth_broker ALIAS bandwidth_broker)

target_include_directories(bandwidth_broker
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(bandwidth_broker PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(bandwidth_broker PUBLIC ws2_32)
endif()

set_target_properties(bandwidth_broker PROPERTIES
  OUTPUT_NAME bandwidth_broker
  DEBUG_POSTFIX "d"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

bb_configure_target(bandwidth_broker)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS bandwidth_broker
  EXPORT BandwidthBrokerTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT BandwidthBrokerTargets
  NAMESPACE BandwidthBroker::
  FILE BandwidthBrokerTargets.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BandwidthBroker
)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BandwidthBrokerConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/BandwidthBrokerConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BandwidthBroker
)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/BandwidthBrokerConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/BandwidthBrokerConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/BandwidthBrokerConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BandwidthBroker
)

if(BB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(BB_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
if(BB_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
if(BB_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
