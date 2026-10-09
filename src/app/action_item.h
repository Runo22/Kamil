#pragma once

#include <string>

#include "core/item.h"

namespace kamil {

// Glyphs for actions without a program icon: Segoe MDL2 Assets code points, or plain Unicode
// symbols that DirectWrite draws through font fallback (Segoe UI Symbol).
inline constexpr wchar_t kGlyphCopy = 0xE8C8;
inline constexpr wchar_t kGlyphFolder = 0xE8B7;
inline constexpr wchar_t kGlyphAdmin = 0xE7EF;
inline constexpr wchar_t kGlyphTerminal = 0xE756;
inline constexpr wchar_t kGlyphPlay = 0xE768;
inline constexpr wchar_t kGlyphBranch = 0x2387;   // ⎇
inline constexpr wchar_t kGlyphPreset = 0x2699;   // ⚙
inline constexpr wchar_t kGlyphTarget = 0x25CE;   // ◎
inline constexpr wchar_t kGlyphSerial = 0x21C4;   // ⇄
inline constexpr wchar_t kGlyphEdit = 0x270E;     // ✎
inline constexpr wchar_t kGlyphRefresh = 0x21BB;  // ↻
inline constexpr wchar_t kGlyphCheck = 0x2713;    // ✓
inline constexpr wchar_t kGlyphCancel = 0xE711;   // MDL2 "Cancel"
inline constexpr wchar_t kGlyphInfo = 0xE946;     // MDL2 "Info"

// An entry of the action panel / a pick list. Entries with the same program share one icon
// cache slot (the key is derived from the icon source).
inline Item make_action(std::wstring id, std::wstring title, std::wstring icon_source, wchar_t glyph, std::wstring subtitle = {}) {
    Item a;
    a.kind = ItemKind::Action;
    a.target = std::move(id);
    a.key = L"action:" + (icon_source.empty() ? a.target : icon_source);
    a.title = std::move(title);
    a.subtitle = std::move(subtitle);
    a.icon_source = std::move(icon_source);
    a.glyph = glyph;
    return a;
}

}  // namespace kamil
