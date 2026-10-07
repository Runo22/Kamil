#include "test.h"

int main() {
    int failed_cases = 0;
    for (const auto& c : kt::registry()) {
        const int before = kt::failures();
        c.fn();
        const bool ok = kt::failures() == before;
        if (!ok) ++failed_cases;
        std::printf("%s %s\n", ok ? "[ ok ]" : "[FAIL]", c.name);
    }
    std::printf("\n%zu tests, %d failed\n", kt::registry().size(), failed_cases);
    return failed_cases == 0 ? 0 : 1;
}
