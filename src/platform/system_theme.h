#pragma once

#include <cstdint>

namespace kamil {

struct SystemTheme {
    bool apps_dark = true;
    uint32_t accent_rgb = 0x0078D4;  // 0xRRGGBB
};

SystemTheme read_system_theme();
bool is_windows11();

}  // namespace kamil
