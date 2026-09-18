// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Text validation. All externally supplied text passes through these checks
// before it is stored, hashed, journaled or transmitted.

#ifndef BANDWIDTH_BROKER_TEXT_HPP
#define BANDWIDTH_BROKER_TEXT_HPP

#include <cstddef>
#include <string>
#include <string_view>

#include "bandwidth_broker/error.hpp"
#include "bandwidth_broker/export.hpp"

namespace bandwidth_broker {

// Strict UTF-8 validation: rejects overlong encodings, surrogate code points,
// code points above U+10FFFF, truncated sequences and stray continuation bytes.
[[nodiscard]] BB_API bool is_valid_utf8(std::string_view text) noexcept;

// Valid UTF-8 containing no C0/C1 control characters (tab is rejected too:
// nothing in this runtime needs embedded control characters).
[[nodiscard]] BB_API bool is_valid_text(std::string_view text) noexcept;

// Valid text of at most p max_bytes bytes.
[[nodiscard]] BB_API bool is_valid_text(std::string_view text, std::size_t max_bytes) noexcept;

[[nodiscard]] BB_API Status validate_text(std::string_view text, std::size_t max_bytes, const char* field);

[[nodiscard]] BB_API std::string to_hex(const void* data, std::size_t length);

}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_TEXT_HPP
