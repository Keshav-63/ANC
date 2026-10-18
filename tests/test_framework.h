#pragma once

// Tiny test framework: header-only registry, no external dependencies.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace anctest {

struct Ctx {
    int failures = 0;
    int checks = 0;

    void expect(bool condition, const std::string& message) {
        ++checks;
        if (!condition) {
            ++failures;
            std::printf("    FAIL: %s\n", message.c_str());
        }
    }

    void expectNear(double measured, double expected, double tolerance,
                    const std::string& message) {
        ++checks;
        const double diff = std::fabs(measured - expected);
        if (diff > tolerance) {
            ++failures;
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s | measured %.6g, expected %.6g, diff %.3g > tol %.3g",
                          message.c_str(), measured, expected, diff, tolerance);
            std::printf("    FAIL: %s\n", buf);
        }
    }
};

using TestFn = void (*)(Ctx&);

struct TestCase {
    const char* name;
    TestFn fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, TestFn fn) { registry().push_back({name, fn}); }
};

}  // namespace anctest

#define ANCTEST(name)                                                     \
    static void name(anctest::Ctx& ctx);                                  \
    static anctest::Registrar anctest_reg_##name(#name, name);            \
    static void name(anctest::Ctx& ctx)
