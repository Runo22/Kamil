#include "core/hotkey.h"

#include <array>
#include <vector>

#include "core/i18n.h"
#include "core/text.h"

namespace kamil {

namespace {

struct KeyName {
    const char* name;
    uint32_t vk;
};

// Primary name first: format_hotkey uses the first entry that matches a vk.
constexpr KeyName kKeys[] = {
    {"Space", 0x20},     {"Enter", 0x0D},     {"Return", 0x0D},    {"Tab", 0x09},
    {"Esc", 0x1B},       {"Escape", 0x1B},    {"Backspace", 0x08}, {"Delete", 0x2E},
    {"Del", 0x2E},       {"Insert", 0x2D},    {"Ins", 0x2D},       {"Home", 0x24},
    {"End", 0x23},       {"PageUp", 0x21},    {"PgUp", 0x21},      {"PageDown", 0x22},
    {"PgDn", 0x22},      {"Up", 0x26},        {"Down", 0x28},      {"Left", 0x25},
    {"Right", 0x27},     {"Pause", 0x13},     {"PrintScreen", 0x2C}, {"PrtSc", 0x2C},
    {"ScrollLock", 0x91}, {"CapsLock", 0x14}, {"Plus", 0xBB},      {"Minus", 0xBD},
    {"Comma", 0xBC},     {"Period", 0xBE},    {"Backtick", 0xC0},  {"Multiply", 0x6A},
    {"Add", 0x6B},       {"Subtract", 0x6D},  {"Decimal", 0x6E},   {"Divide", 0x6F},
};

std::optional<uint32_t> key_from_name(std::string_view name) {
    if (name.size() == 1) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return static_cast<uint32_t>(c);
        if (c == '+') return 0xBB;
        if (c == '-') return 0xBD;
        if (c == ',') return 0xBC;
        if (c == '.') return 0xBE;
        if (c == '`') return 0xC0;
    }
    const std::string lower = to_lower_ascii(name);
    if (lower.size() >= 2 && lower[0] == 'f') {
        int n = 0;
        bool digits = true;
        for (size_t i = 1; i < lower.size(); ++i) {
            if (lower[i] < '0' || lower[i] > '9') {
                digits = false;
                break;
            }
            n = n * 10 + (lower[i] - '0');
        }
        if (digits && n >= 1 && n <= 24) return static_cast<uint32_t>(0x70 + n - 1);
    }
    if (lower.rfind("numpad", 0) == 0 && lower.size() == 7 && lower[6] >= '0' && lower[6] <= '9')
        return static_cast<uint32_t>(0x60 + (lower[6] - '0'));
    for (const auto& k : kKeys)
        if (iequals_ascii(name, k.name)) return k.vk;
    return std::nullopt;
}

std::optional<uint32_t> mod_from_name(std::string_view name) {
    const std::string n = to_lower_ascii(name);
    if (n == "alt") return kModAlt;
    if (n == "ctrl" || n == "control" || n == "strg") return kModCtrl;
    if (n == "shift") return kModShift;
    if (n == "win" || n == "windows" || n == "meta" || n == "super") return kModWin;
    return std::nullopt;
}

std::vector<std::string_view> split_plus(std::string_view s) {
    // "Ctrl++" means Ctrl + Plus: a '+' directly after a separator '+' is a key, not a separator.
    std::vector<std::string_view> parts;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '+') continue;
        if (i == start) continue;  // leading '+' of a part: belongs to the part ("+" key)
        parts.push_back(trim(s.substr(start, i - start)));
        start = i + 1;
    }
    if (start < s.size()) parts.push_back(trim(s.substr(start)));
    return parts;
}

}  // namespace

std::optional<Hotkey> parse_hotkey(std::string_view text, std::string* error) {
    auto fail = [&](std::string msg) -> std::optional<Hotkey> {
        if (error) *error = std::move(msg);
        return std::nullopt;
    };
    text = trim(text);
    if (text.empty()) return fail(loc("hotkey is empty", "kısayol boş"));

    const auto parts = split_plus(text);
    Hotkey hk;
    for (size_t i = 0; i < parts.size(); ++i) {
        const auto part = parts[i];
        if (part.empty()) return fail(loc("hotkey has an empty part", "kısayolda boş parça var"));
        const bool last = i + 1 == parts.size();
        if (!last) {
            auto mod = mod_from_name(part);
            if (!mod) return fail(std::string(loc("unknown modifier: '", "bilinmeyen değiştirici tuş: '")) + std::string(part) + "'");
            hk.mods |= *mod;
            continue;
        }
        if (auto mod = mod_from_name(part)) {
            (void)mod;
            return fail(std::string(loc("a hotkey cannot end with a modifier: '", "kısayol bir değiştirici ile bitemez: '")) + std::string(part) + "'");
        }
        auto vk = key_from_name(part);
        if (!vk) return fail(std::string(loc("unknown key: '", "bilinmeyen tuş: '")) + std::string(part) + "'");
        hk.vk = *vk;
    }
    if (hk.mods == 0 && !(hk.vk >= 0x70 && hk.vk <= 0x87))
        return fail(loc("a global hotkey needs at least one modifier (Ctrl/Alt/Shift/Win)", "global kısayol en az bir değiştirici (Ctrl/Alt/Shift/Win) içermeli"));
    return hk;
}

std::string format_hotkey(const Hotkey& hk) {
    std::string out;
    if (hk.mods & kModCtrl) out += "Ctrl+";
    if (hk.mods & kModAlt) out += "Alt+";
    if (hk.mods & kModShift) out += "Shift+";
    if (hk.mods & kModWin) out += "Win+";
    const uint32_t vk = hk.vk;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        out.push_back(static_cast<char>(vk));
    } else if (vk >= 0x70 && vk <= 0x87) {
        out += "F" + std::to_string(vk - 0x70 + 1);
    } else if (vk >= 0x60 && vk <= 0x69) {
        out += "Numpad" + std::to_string(vk - 0x60);
    } else {
        bool found = false;
        for (const auto& k : kKeys) {
            if (k.vk == vk) {
                out += k.name;
                found = true;
                break;
            }
        }
        if (!found) out += "VK" + std::to_string(vk);
    }
    return out;
}

}  // namespace kamil
