#include "core/i18n.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace kamil {

namespace {
std::atomic<Lang> g_lang{Lang::En};
}

Lang language() noexcept { return g_lang.load(std::memory_order_relaxed); }
void set_language(Lang lang) noexcept { g_lang.store(lang, std::memory_order_relaxed); }

Lang system_language() noexcept {
#ifdef _WIN32
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_TURKISH ? Lang::Tr : Lang::En;
#else
    return Lang::En;
#endif
}

Lang parse_language(std::string_view s) noexcept {
    if (s == "tr") return Lang::Tr;
    if (s == "en") return Lang::En;
    return system_language();
}

std::wstring format_args(std::wstring_view pattern, const std::wstring_view* args, size_t count) {
    std::wstring out;
    out.reserve(pattern.size() + 32);
    size_t next = 0;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == L'{' && i + 1 < pattern.size() && pattern[i + 1] == L'}') {
            if (next < count) out += args[next++];
            ++i;
            continue;
        }
        out += pattern[i];
    }
    return out;
}

}  // namespace kamil
