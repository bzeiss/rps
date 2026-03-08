/// Minimal test framework for rps-coordinator unit tests.
/// No external dependency — just assertion macros and a test runner.

#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace rps::test {

struct TestCase {
    std::string name;
    std::function<void()> func;
};

inline std::vector<TestCase>& testRegistry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int registerTest(const char* name, std::function<void()> func) {
    testRegistry().push_back({name, std::move(func)});
    return 0;
}

struct TestFailure {
    std::string message;
};

inline int runAllTests() {
    int passed = 0;
    int failed = 0;
    for (const auto& test : testRegistry()) {
        try {
            test.func();
            ++passed;
            std::printf("  [PASS] %s\n", test.name.c_str());
        } catch (const TestFailure& e) {
            ++failed;
            std::printf("  [FAIL] %s\n         %s\n", test.name.c_str(), e.message.c_str());
        } catch (const std::exception& e) {
            ++failed;
            std::printf("  [FAIL] %s\n         Exception: %s\n", test.name.c_str(), e.what());
        }
    }
    std::printf("\n  Results: %d passed, %d failed, %d total\n\n",
                passed, failed, passed + failed);
    return failed > 0 ? 1 : 0;
}

} // namespace rps::test

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

#define TEST(name) \
    static void test_##name(); \
    static int test_reg_##name = rps::test::registerTest(#name, test_##name); \
    static void test_##name()

#define ASSERT_TRUE(expr) \
    do { if (!(expr)) throw rps::test::TestFailure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": ASSERT_TRUE(" #expr ") failed"}; } while(0)

#define ASSERT_FALSE(expr) \
    do { if (expr) throw rps::test::TestFailure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": ASSERT_FALSE(" #expr ") failed"}; } while(0)

#define ASSERT_EQ(a, b) \
    do { if ((a) != (b)) throw rps::test::TestFailure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": ASSERT_EQ(" #a ", " #b ") failed"}; } while(0)

#define ASSERT_NE(a, b) \
    do { if ((a) == (b)) throw rps::test::TestFailure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": ASSERT_NE(" #a ", " #b ") failed"}; } while(0)

#define ASSERT_NEAR(a, b, eps) \
    do { if (std::fabs(static_cast<double>(a) - static_cast<double>(b)) > (eps)) \
        throw rps::test::TestFailure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": ASSERT_NEAR(" #a ", " #b ") failed — diff=" + std::to_string(std::fabs(static_cast<double>(a) - static_cast<double>(b)))}; } while(0)

#define ASSERT_THROWS(expr) \
    do { bool threw = false; try { expr; } catch (...) { threw = true; } \
    if (!threw) throw rps::test::TestFailure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": ASSERT_THROWS(" #expr ") — no exception thrown"}; } while(0)
