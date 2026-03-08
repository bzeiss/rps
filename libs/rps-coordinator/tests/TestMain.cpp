#include "TestFramework.hpp"
#include <cstdio>

int main() {
    std::printf("=== rps-coordinator unit tests ===\n\n");
    return rps::test::runAllTests();
}
