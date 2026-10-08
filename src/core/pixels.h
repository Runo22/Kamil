#pragma once

#include <cstdint>
#include <vector>

namespace kamil {

// Normalizes 32-bit BGRA icon pixels to premultiplied alpha, which Direct2D expects.
// Shell icon bitmaps come either premultiplied or with straight alpha depending on the icon;
// drawing straight alpha as premultiplied makes anti-aliased edges too bright (a white halo).
// Straight alpha is detected by a color channel larger than its alpha, which is impossible in
// premultiplied data. Bitmaps without any alpha (legacy icons) become opaque.
// Returns true if the pixels were converted.
bool normalize_icon_pixels(std::vector<uint32_t>& bgra);

}  // namespace kamil
