// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef BANDWIDTH_BROKER_VERSION_HPP
#define BANDWIDTH_BROKER_VERSION_HPP

#define BB_VERSION_MAJOR 1
#define BB_VERSION_MINOR 0
#define BB_VERSION_PATCH 0
#define BB_VERSION_STRING "1.0.0"

// Wire protocol revision implemented by this build.
#define BB_PROTOCOL_VERSION 1

// On-disk store format revision implemented by this build.
#define BB_STORE_FORMAT_VERSION 1

#define BB_COPYRIGHT_NOTICE "Copyright 2026 Summon Software Labs"

namespace bandwidth_broker {

struct Version final {
  int major;
  int minor;
  int patch;
};

[[nodiscard]] constexpr Version version() noexcept { return Version{BB_VERSION_MAJOR, BB_VERSION_MINOR, BB_VERSION_PATCH}; }
[[nodiscard]] constexpr const char* version_string() noexcept { return BB_VERSION_STRING; }
[[nodiscard]] constexpr unsigned protocol_version() noexcept { return BB_PROTOCOL_VERSION; }
[[nodiscard]] constexpr unsigned store_format_version() noexcept { return BB_STORE_FORMAT_VERSION; }

}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_VERSION_HPP
