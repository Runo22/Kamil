#include "core/pixels.h"

#include <algorithm>

namespace kamil {

bool normalize_icon_pixels(std::vector<uint32_t>& px) {
    bool any_alpha = false, straight = false;
    for (uint32_t p : px) {
        const uint32_t a = p >> 24;
        if (a) any_alpha = true;
        const uint32_t m = std::max({p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF});
        if (m > a) straight = true;
    }
    if (!any_alpha) {
        for (auto& p : px) p |= 0xFF000000u;
        return true;
    }
    if (!straight) return false;
    for (auto& p : px) {
        const uint32_t a = p >> 24;
        auto mul = [a](uint32_t c) { return (c * a + 127) / 255; };
        p = (a << 24) | (mul((p >> 16) & 0xFF) << 16) | (mul((p >> 8) & 0xFF) << 8) | mul(p & 0xFF);
    }
    return true;
}

}  // namespace kamil
