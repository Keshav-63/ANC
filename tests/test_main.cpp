#include <cstdio>

#include "test_framework.h"

int main() {
    int failedTests = 0;
    int totalChecks = 0;
    for (const auto& test : anctest::registry()) {
        std::printf("%s\n", test.name);
        anctest::Ctx ctx;
        test.fn(ctx);
        totalChecks += ctx.checks;
        if (ctx.failures > 0) {
            ++failedTests;
            std::printf("  -> %d failure(s)\n", ctx.failures);
        } else {
            std::printf("  -> pass (%d checks)\n", ctx.checks);
        }
    }
    std::printf("\n%zu test(s), %d check(s), %d failed\n", anctest::registry().size(),
                totalChecks, failedTests);
    return failedTests == 0 ? 0 : 1;
}
