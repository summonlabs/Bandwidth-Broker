// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "bandwidth_broker/text.hpp"

namespace bandwidth_broker {
namespace {

constexpr unsigned char kContinuationMask = 0xC0;
constexpr unsigned char kContinuationValue = 0x80;

[[nodiscard]] bool is_continuation(unsigned char byte) noexcept {
  return (byte & kContinuationMask) == kContinuationValue;
}

}  // namespace

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t i = 0;
  const std::size_t n = text.size();
  while (i < n) {
    const auto byte = static_cast<unsigned char>(text[i]);
    if (byte < 0x80u) {
      ++i;
      continue;
    }
    std::size_t extra = 0;
    std::uint32_t code_point = 0;
    if ((byte & 0xE0u) == 0xC0u) {
      extra = 1;
      code_point = static_cast<std::uint32_t>(byte & 0x1Fu);
      if (code_point < 0x02u) {
        return false;  // overlong two-byte encoding
      }
    } else if ((byte & 0xF0u) == 0xE0u) {
      extra = 2;
      code_point = static_cast<std::uint32_t>(byte & 0x0Fu);
    } else if ((byte & 0xF8u) == 0xF0u) {
      extra = 3;
      code_point = static_cast<std::uint32_t>(byte & 0x07u);
      if (code_point > 0x04u) {
        return false;  // code point above U+10FFFF
      }
    } else {
      return false;  // stray continuation byte or invalid lead byte
    }
    if (i + extra >= n) {
      return false;  // truncated sequence
    }
    for (std::size_t k = 1; k <= extra; ++k) {
      const auto next = static_cast<unsigned char>(text[i + k]);
      if (!is_continuation(next)) {
        return false;
      }
      code_point = (code_point << 6) | static_cast<std::uint32_t>(next & 0x3Fu);
    }
    if (extra == 2 && code_point < 0x0800u) {
      return false;  // overlong three-byte encoding
    }
    if (extra == 3 && code_point < 0x10000u) {
      return false;  // overlong four-byte encoding
    }
    if (code_point >= 0xD800u && code_point <= 0xDFFFu) {
      return false;  // UTF-16 surrogate range is not a valid scalar value
    }
    if (code_point > 0x10FFFFu) {
      return false;
    }
    i += extra + 1;
  }
  return true;
}

bool is_valid_text(std::string_view text) noexcept {
  if (!is_valid_utf8(text)) {
    return false;
  }
  for (const char raw : text) {
    const auto byte = static_cast<unsigned char>(raw);
    if (byte < 0x20u || byte == 0x7Fu) {
      return false;
    }
    if (byte >= 0x80u && byte <= 0x9Fu) {
      return false;  // C1 controls appear as continuation bytes; reject explicitly
    }
  }
  return true;
}

bool is_valid_text(std::string_view text, std::size_t max_bytes) noexcept {
  return text.size() <= max_bytes && is_valid_text(text);
}

Status validate_text(std::string_view text, std::size_t max_bytes, const char* field) {
  if (text.size() > max_bytes) {
    return make_error_status(ErrorCode::BoundsExceeded, std::string(field) + " exceeds the maximum length");
  }
  if (!is_valid_utf8(text)) {
    return make_error_status(ErrorCode::InvalidArgument, std::string(field) + " is not valid UTF-8");
  }
  for (const char raw : text) {
    const auto byte = static_cast<unsigned char>(raw);
    if (byte < 0x20u || byte == 0x7Fu || (byte >= 0x80u && byte <= 0x9Fu)) {
      return make_error_status(ErrorCode::InvalidArgument, std::string(field) + " contains a control character");
    }
  }
  return Status::success();
}

std::string to_hex(const void* data, std::size_t length) {
  static constexpr char kDigits[] = "0123456789abcdef";
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::string out;
  out.resize(length * 2);
  for (std::size_t i = 0; i < length; ++i) {
    out[i * 2] = kDigits[(bytes[i] >> 4) & 0x0Fu];
    out[i * 2 + 1] = kDigits[bytes[i] & 0x0Fu];
  }
  return out;
}

}  // namespace bandwidth_broker
