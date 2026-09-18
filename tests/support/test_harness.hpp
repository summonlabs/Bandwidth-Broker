// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Minimal in-tree test harness. Tests are proof obligations: a test either
// demonstrates the claimed property or fails with the exact observation that
// contradicted it. No test in this repository is wrapped in a timeout.

#ifndef BB_TESTS_SUPPORT_TEST_HARNESS_HPP
#define BB_TESTS_SUPPORT_TEST_HARNESS_HPP

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "bandwidth_broker/identity.hpp"
#include "bandwidth_broker/quantity.hpp"

namespace bb_test {

struct TestCase final {
  const char* suite;
  const char* name;
  void (*body)();
};

class Registry final {
 public:
  static Registry& instance() {
    static Registry registry;
    return registry;
  }

  void add(const char* suite, const char* name, void (*body)()) { cases_.push_back(TestCase{suite, name, body}); }

  [[nodiscard]] const std::vector<TestCase>& cases() const noexcept { return cases_; }

 private:
  std::vector<TestCase> cases_;
};

struct Registrar final {
  Registrar(const char* suite, const char* name, void (*body)()) { Registry::instance().add(suite, name, body); }
};

// Per-test failure accumulator, read by the runner.
class Context final {
 public:
  static Context& instance() {
    static Context context;
    return context;
  }

  void fail(const char* file, int line, const std::string& message) {
    ++failure_count_;
    if (failure_count_ <= kMaxReportedFailures) {
      std::printf("    FAIL %s:%d: %s\n", file, line, message.c_str());
    } else if (failure_count_ == kMaxReportedFailures + 1) {
      std::printf("    ... further failures in this test suppressed\n");
    }
  }

  void note(const std::string& message) { std::printf("    note: %s\n", message.c_str()); }

  void reset() { failure_count_ = 0; }
  [[nodiscard]] std::size_t failure_count() const noexcept { return failure_count_; }

  static constexpr std::size_t kMaxReportedFailures = 20;

 private:
  std::size_t failure_count_{0};
};

[[nodiscard]] inline std::string describe(const char* value) { return std::string("\"") + value + "\""; }
[[nodiscard]] inline std::string describe(const std::string& value) { return std::string("\"") + value + "\""; }
[[nodiscard]] inline std::string describe(std::string_view value) { return std::string("\"") + std::string(value) + "\""; }
[[nodiscard]] inline std::string describe(bool value) { return value ? "true" : "false"; }
[[nodiscard]] inline std::string describe(std::nullptr_t) { return "null"; }
[[nodiscard]] inline std::string describe(bandwidth_broker::Bandwidth value) { return value.to_string(); }

template <typename Tag>
[[nodiscard]] std::string describe(bandwidth_broker::Id<Tag> value) {
  return value.to_string();
}

template <typename Tag>
[[nodiscard]] std::string describe(bandwidth_broker::Generation<Tag> value) {
  return std::to_string(value.value());
}

template <typename T>
[[nodiscard]] std::string describe(const T& value) {
  if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (std::is_arithmetic_v<T>) {
    return std::to_string(value);
  } else {
    return "<value>";
  }
}

}  // namespace bb_test

#define BB_TEST(suite_name, test_name)                                                          \
  static void suite_name##_##test_name##_body();                                                \
  namespace {                                                                                   \
  const ::bb_test::Registrar bb_test_registrar_##suite_name##_##test_name(                     \
      #suite_name, #test_name, &suite_name##_##test_name##_body);                               \
  }                                                                                             \
  static void suite_name##_##test_name##_body()

#define BB_FAIL(message) ::bb_test::Context::instance().fail(__FILE__, __LINE__, (message))

#define BB_CHECK(condition)                                                                     \
  do {                                                                                          \
    if (!(condition)) {                                                                         \
      BB_FAIL(std::string("expected: ") + #condition);                                          \
    }                                                                                           \
  } while (false)

#define BB_REQUIRE(condition)                                                                   \
  do {                                                                                          \
    if (!(condition)) {                                                                         \
      BB_FAIL(std::string("required: ") + #condition);                                          \
      return;                                                                                   \
    }                                                                                           \
  } while (false)

#define BB_CHECK_EQ(actual, expected)                                                           \
  do {                                                                                          \
    const auto& bb_actual_ = (actual);                                                          \
    const auto& bb_expected_ = (expected);                                                      \
    if (!(bb_actual_ == bb_expected_)) {                                                        \
      BB_FAIL(std::string(#actual) + " == " + #expected + " (actual=" +                         \
              ::bb_test::describe(bb_actual_) + ", expected=" + ::bb_test::describe(bb_expected_) + ")"); \
    }                                                                                           \
  } while (false)

#define BB_CHECK_NE(actual, unexpected)                                                         \
  do {                                                                                          \
    const auto& bb_actual_ = (actual);                                                          \
    const auto& bb_unexpected_ = (unexpected);                                                  \
    if (bb_actual_ == bb_unexpected_) {                                                         \
      BB_FAIL(std::string(#actual) + " != " + #unexpected + " (both=" +                         \
              ::bb_test::describe(bb_actual_) + ")");                                           \
    }                                                                                           \
  } while (false)

// Asserts that an expression evaluates to a successful Result and binds it.
#define BB_REQUIRE_OK(name, expression)                                                         \
  auto name##_result = (expression);                                                            \
  if (!name##_result.ok()) {                                                                    \
    BB_FAIL(std::string(#expression) + " failed: " + std::string(::bandwidth_broker::to_string(name##_result.code())) + \
            " (" + name##_result.error().message + ")");                                        \
    return;                                                                                     \
  }                                                                                             \
  auto& name = name##_result.value()

#define BB_CHECK_OK(expression)                                                                 \
  do {                                                                                          \
    const auto bb_result_ = (expression);                                                       \
    if (!bb_result_.ok()) {                                                                     \
      BB_FAIL(std::string(#expression) + " failed: " + std::string(::bandwidth_broker::to_string(bb_result_.code())) + \
              " (" + bb_result_.error().message + ")");                                         \
    }                                                                                           \
  } while (false)

#define BB_REQUIRE_ERR(code_expected, expression)                                               \
  do {                                                                                          \
    const auto bb_result_ = (expression);                                                       \
    if (bb_result_.ok()) {                                                                      \
      BB_FAIL(std::string(#expression) + " unexpectedly succeeded");                            \
      return;                                                                                   \
    }                                                                                           \
    if (bb_result_.code() != (code_expected)) {                                                 \
      BB_FAIL(std::string(#expression) + " failed with " +                                      \
              std::string(::bandwidth_broker::to_string(bb_result_.code())) + " instead of " +  \
              std::string(::bandwidth_broker::to_string(code_expected)));                       \
      return;                                                                                   \
    }                                                                                           \
  } while (false)

#define BB_CHECK_ERR(code_expected, expression)                                                 \
  do {                                                                                          \
    const auto bb_result_ = (expression);                                                       \
    if (bb_result_.ok()) {                                                                      \
      BB_FAIL(std::string(#expression) + " unexpectedly succeeded");                            \
    } else if (bb_result_.code() != (code_expected)) {                                          \
      BB_FAIL(std::string(#expression) + " failed with " +                                      \
              std::string(::bandwidth_broker::to_string(bb_result_.code())) + " instead of " +  \
              std::string(::bandwidth_broker::to_string(code_expected)));                       \
    }                                                                                           \
  } while (false)

#endif  // BB_TESTS_SUPPORT_TEST_HARNESS_HPP
