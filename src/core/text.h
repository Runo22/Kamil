#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace kamil {

// UTF-8 <-> wide. On Windows wchar_t is UTF-16 (surrogate pairs are produced/consumed);
// elsewhere it is UTF-32. Invalid input sequences become U+FFFD.
std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);

// Search folding: lowercase + diacritics removed, Turkish aware (İ I ı i -> i, ş -> s, ğ -> g,
// ü -> u, ö -> o, ç -> c, plus common Latin-1 accents). Folding is strictly one code unit to one
// code unit, so match positions in the folded string are positions in the original string.
wchar_t fold_char(wchar_t c) noexcept;
std::wstring fold(std::wstring_view s);

bool is_upper(wchar_t c) noexcept;
bool is_lower(wchar_t c) noexcept;
bool is_digit(wchar_t c) noexcept;
bool is_separator(wchar_t c) noexcept;  // characters that start a new "word" after them

// Bit set of the (folded) characters present in s. Used as a cheap pre-filter: an item can only
// match a query if (item_mask & query_mask) == query_mask.
uint64_t char_mask(std::wstring_view folded) noexcept;

// Wildcard match ('*' any run, '?' one character). Both arguments should already be folded.
bool glob_match(std::wstring_view pattern, std::wstring_view text) noexcept;

std::wstring_view trim(std::wstring_view s) noexcept;
std::string_view trim(std::string_view s) noexcept;
bool iequals_ascii(std::string_view a, std::string_view b) noexcept;
std::string to_lower_ascii(std::string_view s);

}  // namespace kamil
