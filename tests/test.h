#pragma once

// Minimal self-contained test harness (no third-party dependency, builds offline).

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace kt {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int n = 0;
    return n;
}

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

}  // namespace kt

#define KT_CAT2(a, b) a##b
#define KT_CAT(a, b) KT_CAT2(a, b)
#define TEST(name)                                                   \
    static void name();                                              \
    static ::kt::Register KT_CAT(kt_reg_, name)(#name, &name);       \
    static void name()

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            ++::kt::failures();                                                     \
        }                                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        if (!((a) == (b))) {                                                        \
            std::printf("  FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);    \
            ++::kt::failures();                                                     \
        }                                                                           \
    } while (0)
