#pragma once

// Two-language user interface (English / Turkish). Strings stay next to the code that uses
// them: loc(L"Open", L"Aç") returns the variant for the current language.

#include <array>
#include <atomic>
#include <string>
#include <string_view>

namespace kamil {

enum class Lang { En, Tr };

Lang language() noexcept;
void set_language(Lang lang) noexcept;
// The Windows display language (GetUserDefaultUILanguage); English on other platforms.
Lang system_language() noexcept;
// "auto" | "en" | "tr"
Lang parse_language(std::string_view setting) noexcept;

inline const wchar_t* loc(const wchar_t* en, const wchar_t* tr) noexcept { return language() == Lang::Tr ? tr : en; }
inline const char* loc(const char* en, const char* tr) noexcept { return language() == Lang::Tr ? tr : en; }

// Replaces each "{}" in `pattern` with the next argument, so translations can order words
// freely: fmt(loc(L"{} could not be opened", L"{} açılamadı"), title).
std::wstring format_args(std::wstring_view pattern, const std::wstring_view* args, size_t count);

template <class... A>
std::wstring fmt(std::wstring_view pattern, const A&... args) {
    const std::array<std::wstring_view, sizeof...(A)> list{std::wstring_view(args)...};
    return format_args(pattern, list.data(), list.size());
}

}  // namespace kamil
