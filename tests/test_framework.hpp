// test_framework.hpp - Simple test framework
#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

// Helper to convert enum types to printable values
template<typename T>
auto to_printable(const T& val) {
    if constexpr (std::is_enum_v<T>) {
        return static_cast<std::underlying_type_t<T>>(val);
    } else {
        return val;
    }
}

// Test assertion macros
#define ASSERT_EQ(a, b)                                                            \
    do {                                                                           \
        auto _a = (a);                                                             \
        auto _b = (b);                                                             \
        if (_a != _b) {                                                            \
            std::ostringstream _oss;                                               \
            _oss << "ASSERT_EQ failed: " << #a << " != " << #b << "\n";            \
            _oss << "  Left:  " << to_printable(_a) << "\n";                       \
            _oss << "  Right: " << to_printable(_b);                               \
            throw std::runtime_error(_oss.str());                                  \
        }                                                                          \
    } while (0)

#define ASSERT_NE(a, b)                                                            \
    do {                                                                           \
        auto _a = (a);                                                             \
        auto _b = (b);                                                             \
        if (_a == _b) {                                                            \
            std::ostringstream _oss;                                               \
            _oss << "ASSERT_NE failed: " << #a << " == " << #b << "\n";            \
            _oss << "  Both: " << to_printable(_a);                                \
            throw std::runtime_error(_oss.str());                                  \
        }                                                                          \
    } while (0)

#define ASSERT_TRUE(x)                                                             \
    do {                                                                           \
        if (!(x)) {                                                                \
            throw std::runtime_error("ASSERT_TRUE failed: " #x " is false");       \
        }                                                                          \
    } while (0)

#define ASSERT_FALSE(x)                                                            \
    do {                                                                           \
        if (x) {                                                                   \
            throw std::runtime_error("ASSERT_FALSE failed: " #x " is true");       \
        }                                                                          \
    } while (0)

#define ASSERT_GT(a, b)                                                            \
    do {                                                                           \
        auto _a = (a);                                                             \
        auto _b = (b);                                                             \
        if (!(_a > _b)) {                                                          \
            std::ostringstream _oss;                                               \
            _oss << "ASSERT_GT failed: " << #a << " <= " << #b << "\n";            \
            _oss << "  Left:  " << _a << "\n";                                     \
            _oss << "  Right: " << _b;                                             \
            throw std::runtime_error(_oss.str());                                  \
        }                                                                          \
    } while (0)

#define ASSERT_GE(a, b)                                                            \
    do {                                                                           \
        auto _a = (a);                                                             \
        auto _b = (b);                                                             \
        if (!(_a >= _b)) {                                                         \
            std::ostringstream _oss;                                               \
            _oss << "ASSERT_GE failed: " << #a << " < " << #b << "\n";             \
            _oss << "  Left:  " << _a << "\n";                                     \
            _oss << "  Right: " << _b;                                             \
            throw std::runtime_error(_oss.str());                                  \
        }                                                                          \
    } while (0)

#define ASSERT_LT(a, b)                                                            \
    do {                                                                           \
        auto _a = (a);                                                             \
        auto _b = (b);                                                             \
        if (!(_a < _b)) {                                                          \
            std::ostringstream _oss;                                               \
            _oss << "ASSERT_LT failed: " << #a << " >= " << #b << "\n";            \
            _oss << "  Left:  " << _a << "\n";                                     \
            _oss << "  Right: " << _b;                                             \
            throw std::runtime_error(_oss.str());                                  \
        }                                                                          \
    } while (0)

#define ASSERT_LE(a, b)                                                            \
    do {                                                                           \
        auto _a = (a);                                                             \
        auto _b = (b);                                                             \
        if (!(_a <= _b)) {                                                         \
            std::ostringstream _oss;                                               \
            _oss << "ASSERT_LE failed: " << #a << " > " << #b << "\n";             \
            _oss << "  Left:  " << _a << "\n";                                     \
            _oss << "  Right: " << _b;                                             \
            throw std::runtime_error(_oss.str());                                  \
        }                                                                          \
    } while (0)

// Check that value is within range [expected - tolerance, expected + tolerance]
#define ASSERT_NEAR(actual, expected, tolerance)                                   \
    do {                                                                           \
        auto _a = (actual);                                                        \
        auto _e = (expected);                                                      \
        auto _t = (tolerance);                                                     \
        if (_a < _e - _t || _a > _e + _t) {                                        \
            std::ostringstream _oss;                                               \
            _oss << "ASSERT_NEAR failed: " << #actual << " not within " << _t      \
                 << " of " << _e << "\n";                                          \
            _oss << "  Actual: " << _a;                                            \
            throw std::runtime_error(_oss.str());                                  \
        }                                                                          \
    } while (0)

// Test case structure
struct TestCase {
    std::string name;
    std::string suite;
    std::function<void()> fn;
};

// Test runner singleton
class TestRunner {
    std::vector<TestCase> tests;

public:
    static TestRunner& instance() {
        static TestRunner runner;
        return runner;
    }

    void add(const std::string& suite, const std::string& name, std::function<void()> fn) {
        tests.push_back({name, suite, std::move(fn)});
    }

    int run(const std::string& filter = "") {
        int passed = 0;
        int failed = 0;
        std::string current_suite;

        for (const auto& test : tests) {
            std::string full_name = test.suite + "." + test.name;

            // Apply filter if specified
            if (!filter.empty() && full_name.find(filter) == std::string::npos) {
                continue;
            }

            // Print suite header
            if (test.suite != current_suite) {
                current_suite = test.suite;
                std::cout << "\n=== " << current_suite << " ===\n";
            }

            try {
                test.fn();
                passed++;
                std::cout << "[PASS] " << test.name << "\n";
            } catch (const std::exception& e) {
                failed++;
                std::cout << "[FAIL] " << test.name << "\n";
                std::cout << "       " << e.what() << "\n";
            }
        }

        std::cout << "\n----------------------------------------\n";
        std::cout << "Results: " << passed << " passed, " << failed << " failed\n";

        return failed > 0 ? 1 : 0;
    }
};

// Test registration helper
#define TEST(suite, name)                                                          \
    void test_##suite##_##name();                                                  \
    namespace {                                                                    \
    struct Register_##suite##_##name {                                             \
        Register_##suite##_##name() {                                              \
            TestRunner::instance().add(#suite, #name, test_##suite##_##name);      \
        }                                                                          \
    } register_##suite##_##name;                                                   \
    }                                                                              \
    void test_##suite##_##name()

// Manual registration for tests that need to be in separate files
#define REGISTER_TEST(suite, name, fn)                                             \
    TestRunner::instance().add(#suite, #name, fn)
