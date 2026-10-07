#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace kamil {

// Modifier bits use the same values as Win32 RegisterHotKey (MOD_ALT, MOD_CONTROL, ...).
enum HotkeyMod : uint32_t {
    kModAlt = 0x0001,
    kModCtrl = 0x0002,
    kModShift = 0x0004,
    kModWin = 0x0008,
};

struct Hotkey {
    uint32_t mods = 0;
    uint32_t vk = 0;  // Win32 virtual-key code

    bool operator==(const Hotkey&) const = default;
};

// Parses strings such as "Alt+Space", "Ctrl+Shift+K", "Win+F5", "Ctrl+Alt+Numpad1".
// Case-insensitive; "Control", "Strg", "Windows" and "Meta" are accepted aliases.
std::optional<Hotkey> parse_hotkey(std::string_view text, std::string* error = nullptr);
std::string format_hotkey(const Hotkey& hk);

}  // namespace kamil
