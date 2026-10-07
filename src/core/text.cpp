#include "core/text.h"

namespace kamil {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

void append_code_point(std::wstring& out, char32_t cp) {
    if constexpr (sizeof(wchar_t) == 2) {
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
            return;
        }
    }
    out.push_back(static_cast<wchar_t>(cp));
}

void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}  // namespace

std::wstring widen(std::string_view s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        int extra = 0;
        char32_t cp = 0;
        if (b0 < 0x80) {
            cp = b0;
        } else if ((b0 & 0xE0) == 0xC0) {
            cp = b0 & 0x1F;
            extra = 1;
        } else if ((b0 & 0xF0) == 0xE0) {
            cp = b0 & 0x0F;
            extra = 2;
        } else if ((b0 & 0xF8) == 0xF0) {
            cp = b0 & 0x07;
            extra = 3;
        } else {
            out.push_back(static_cast<wchar_t>(kReplacement));
            ++i;
            continue;
        }
        bool ok = i + static_cast<size_t>(extra) < s.size();  // continuation bytes available
        for (int k = 1; ok && k <= extra; ++k) {
            const auto b = static_cast<unsigned char>(s[i + k]);
            if ((b & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (b & 0x3F);
        }
        if (!ok) {
            out.push_back(static_cast<wchar_t>(kReplacement));
            ++i;
            continue;
        }
        const bool overlong = (extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000);
        if (overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = kReplacement;
        append_code_point(out, cp);
        i += static_cast<size_t>(extra) + 1;
    }
    return out;
}

std::string narrow(std::wstring_view w) {
    std::string out;
    out.reserve(w.size());
    for (size_t i = 0; i < w.size(); ++i) {
        char32_t cp = static_cast<char32_t>(w[i]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size()) {
                const char32_t lo = static_cast<char32_t>(w[i + 1]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ++i;
                }
            }
        }
        if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = kReplacement;
        append_utf8(out, cp);
    }
    return out;
}

wchar_t fold_char(wchar_t c) noexcept {
    if (c < 0x80) {
        if (c >= L'A' && c <= L'Z') return static_cast<wchar_t>(c + 32);
        return c;
    }
    switch (c) {
        // Turkish
        case 0x0130: case 0x0131: return L'i';   // İ ı
        case 0x015E: case 0x015F: return L's';   // Ş ş
        case 0x011E: case 0x011F: return L'g';   // Ğ ğ
        case 0x00DC: case 0x00FC: return L'u';   // Ü ü
        case 0x00D6: case 0x00F6: return L'o';   // Ö ö
        case 0x00C7: case 0x00E7: return L'c';   // Ç ç
        // Latin-1 / common Latin Extended-A
        case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5:
        case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5:
        case 0x0102: case 0x0103: case 0x0104: case 0x0105:
            return L'a';
        case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB:
        case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB:
        case 0x0118: case 0x0119: case 0x011A: case 0x011B:
            return L'e';
        case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF:
        case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF:
            return L'i';
        case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x00D8:
        case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00F8:
        case 0x0150: case 0x0151:
            return L'o';
        case 0x00D9: case 0x00DA: case 0x00DB:
        case 0x00F9: case 0x00FA: case 0x00FB:
        case 0x016E: case 0x016F: case 0x0170: case 0x0171:
            return L'u';
        case 0x00D1: case 0x00F1: case 0x0143: case 0x0144: case 0x0147: case 0x0148:
            return L'n';
        case 0x00DD: case 0x00FD: case 0x00FF: return L'y';
        case 0x0106: case 0x0107: case 0x010C: case 0x010D: return L'c';
        case 0x015A: case 0x015B: case 0x0160: case 0x0161: return L's';
        case 0x0179: case 0x017A: case 0x017B: case 0x017C: case 0x017D: case 0x017E: return L'z';
        case 0x0141: case 0x0142: return L'l';
        case 0x010E: case 0x010F: case 0x0110: case 0x0111: return L'd';
        case 0x0158: case 0x0159: return L'r';
        case 0x0164: case 0x0165: return L't';
        default: break;
    }
    // Generic Latin-1 / Greek / Cyrillic uppercase -> lowercase.
    if (c >= 0x00C0 && c <= 0x00DE && c != 0x00D7) return static_cast<wchar_t>(c + 32);
    if (c >= 0x0391 && c <= 0x03A9 && c != 0x03A2) return static_cast<wchar_t>(c + 32);
    if (c >= 0x0410 && c <= 0x042F) return static_cast<wchar_t>(c + 32);
    if (c >= 0x0400 && c <= 0x040F) return static_cast<wchar_t>(c + 80);
    return c;
}

std::wstring fold(std::wstring_view s) {
    std::wstring out(s.size(), L'\0');
    for (size_t i = 0; i < s.size(); ++i) out[i] = fold_char(s[i]);
    return out;
}

bool is_upper(wchar_t c) noexcept {
    if (c < 0x80) return c >= L'A' && c <= L'Z';
    if (c == 0x0130 || c == 0x015E || c == 0x011E) return true;  // İ Ş Ğ
    if (c >= 0x00C0 && c <= 0x00DE && c != 0x00D7) return true;
    if (c >= 0x0391 && c <= 0x03A9) return true;
    if (c >= 0x0400 && c <= 0x042F) return true;
    return false;
}

bool is_lower(wchar_t c) noexcept {
    if (c < 0x80) return c >= L'a' && c <= L'z';
    if (c == 0x0131 || c == 0x015F || c == 0x011F) return true;  // ı ş ğ
    if (c >= 0x00DF && c <= 0x00FF && c != 0x00F7) return true;
    if (c >= 0x03B1 && c <= 0x03C9) return true;
    if (c >= 0x0430 && c <= 0x045F) return true;
    return false;
}

bool is_digit(wchar_t c) noexcept { return c >= L'0' && c <= L'9'; }

bool is_separator(wchar_t c) noexcept {
    switch (c) {
        case L' ': case L'\t': case L'_': case L'-': case L'.': case L'/': case L'\\': case L':':
        case L'(': case L')': case L'[': case L']': case L'{': case L'}': case L',': case L'+':
        case L'&': case L'@': case L'#': case L'\'': case L'"': case L'|': case L';': case L'=':
            return true;
        default:
            return false;
    }
}

uint64_t char_mask(std::wstring_view folded) noexcept {
    uint64_t m = 0;
    for (wchar_t c : folded) {
        if (c >= L'a' && c <= L'z') {
            m |= uint64_t{1} << (c - L'a');
        } else if (c >= L'0' && c <= L'9') {
            m |= uint64_t{1} << (26 + (c - L'0'));
        } else if (c == L' ') {
            continue;
        } else {
            m |= uint64_t{1} << (36 + (static_cast<uint32_t>(c) % 28));
        }
    }
    return m;
}

bool glob_match(std::wstring_view p, std::wstring_view t) noexcept {
    size_t pi = 0, ti = 0, star = std::wstring_view::npos, mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == L'?' || p[pi] == t[ti])) {
            ++pi;
            ++ti;
        } else if (pi < p.size() && p[pi] == L'*') {
            star = pi++;
            mark = ti;
        } else if (star != std::wstring_view::npos) {
            pi = star + 1;
            ti = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == L'*') ++pi;
    return pi == p.size();
}

std::wstring_view trim(std::wstring_view s) noexcept {
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == L'\r' || s.front() == L'\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r' || s.back() == L'\n')) s.remove_suffix(1);
    return s;
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

bool iequals_ascii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
        if (x != y) return false;
    }
    return true;
}

std::string to_lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return out;
}

}  // namespace kamil
