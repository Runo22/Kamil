#pragma once

#include <string>

#include "core/item.h"

namespace kamil {

// Glyphs for actions without a program icon. Private-use code points (U+E000..U+F8FF) are drawn
// with Segoe MDL2 Assets; other symbols (⎇ ◎ ⇄), which that font does not have (they showed as
// empty boxes), with Segoe UI Symbol. See LauncherWindow::draw_row.
inline constexpr wchar_t kGlyphCopy = 0xE8C8;      // MDL2 Copy
inline constexpr wchar_t kGlyphFolder = 0xE8B7;    // MDL2 Folder
inline constexpr wchar_t kGlyphAdmin = 0xE7EF;     // MDL2 Admin
inline constexpr wchar_t kGlyphTerminal = 0xE756;  // MDL2 CommandPrompt
inline constexpr wchar_t kGlyphPlay = 0xE768;      // MDL2 Play
inline constexpr wchar_t kGlyphEdit = 0xE70F;      // MDL2 Edit
inline constexpr wchar_t kGlyphRefresh = 0xE72C;   // MDL2 Refresh
inline constexpr wchar_t kGlyphPreset = 0xE713;    // MDL2 Setting (gear)
inline constexpr wchar_t kGlyphCheck = 0xE73E;     // MDL2 CheckMark
inline constexpr wchar_t kGlyphCancel = 0xE711;    // MDL2 Cancel
inline constexpr wchar_t kGlyphInfo = 0xE946;      // MDL2 Info
inline constexpr wchar_t kGlyphList = 0xE8FD;      // MDL2 BulletedList (git status)
inline constexpr wchar_t kGlyphSync = 0xE895;      // MDL2 Sync (git fetch)
inline constexpr wchar_t kGlyphDownload = 0xE896;  // MDL2 Download (git pull)
inline constexpr wchar_t kGlyphUpload = 0xE898;    // MDL2 Upload (git push)
inline constexpr wchar_t kGlyphHistory = 0xE81C;   // MDL2 History (git log)
inline constexpr wchar_t kGlyphBranch = 0x2387;    // ⎇ (Segoe UI Symbol)
inline constexpr wchar_t kGlyphTarget = 0x25CE;    // ◎ (Segoe UI Symbol)
inline constexpr wchar_t kGlyphSerial = 0x21C4;    // ⇄ (Segoe UI Symbol)

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
