#pragma once

// Fluent UI System Icons (src/ui/fluent_icons.h) as Direct2D path geometry.

#include <d2d1.h>

#include "platform/win.h"
#include "ui/fluent_icons.h"

namespace kamil {

const FluentIcon* find_fluent_icon(wchar_t glyph);  // nullptr: no icon for this glyph code

// The icon's path as geometry in its own viewBox units (scale by size / icon.view to draw).
// Factory resources: built once, they survive a lost device.
ComPtr<ID2D1PathGeometry> build_fluent_geometry(ID2D1Factory* factory, const FluentIcon& icon);

}  // namespace kamil
