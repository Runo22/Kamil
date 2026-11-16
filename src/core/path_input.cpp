#include "core/path_input.h"

#include <algorithm>
#include <filesystem>

#include "core/cmake.h"
#include "core/fuzzy.h"
#include "core/text.h"

namespace kamil {

namespace {

bool is_sep(wchar_t c) { return c == L'\\' || c == L'/'; }
bool is_ascii_letter(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }
bool is_dec(wchar_t c) { return c >= L'0' && c <= L'9'; }

std::wstring_view strip_quotes(std::wstring_view v) {
    v = trim(v);
    if (v.size() >= 2 && ((v.front() == L'"' && v.back() == L'"') || (v.front() == L'\'' && v.back() == L'\'')))
        v = trim(v.substr(1, v.size() - 2));
    return v;
}

// Reads 1..7 digits at `i`; returns the number and moves `i` past them.
std::optional<int> read_number(std::wstring_view s, size_t& i) {
    size_t j = i;
    int n = 0;
    while (j < s.size() && is_dec(s[j]) && j - i < 7) n = n * 10 + (s[j++] - L'0');
    if (j == i || (j < s.size() && is_dec(s[j]))) return std::nullopt;
    i = j;
    return n;
}

// The written file of a location must look like a file: an extension or a folder, no URL, no
// characters Windows does not allow in names.
bool plausible_file(std::wstring_view f) {
    if (f.empty() || f.size() > 1024 || f.find(L"://") != std::wstring_view::npos) return false;
    if (f.front() == L' ' || f.back() == L' ') return false;
    for (size_t i = 0; i < f.size(); ++i) {
        const wchar_t c = f[i];
        if (c < 0x20 || c == L'<' || c == L'>' || c == L'|' || c == L'?' || c == L'*' || c == L'"') return false;
        if (c == L':' && i != 1) return false;
    }
    const std::wstring name = last_path_part(f);
    const size_t dot = name.rfind(L'.');
    const bool has_ext = dot != std::wstring::npos && dot + 1 < name.size();
    return has_ext || f.find_first_of(L"\\/") != std::wstring_view::npos;
}

// "file:///C:/a%20b/c" -> "C:/a b/c"; "file://server/share/x" -> "//server/share/x".
std::optional<std::wstring> from_file_uri(std::wstring_view s) {
    if (s.size() < 8 || !iequals_ascii(narrow(s.substr(0, 7)), "file://")) return std::nullopt;
    std::wstring_view rest = s.substr(7);
    std::wstring raw;
    if (!rest.empty() && rest.front() == L'/') rest.remove_prefix(1);  // file:///C:/...
    else raw = L"//";                                                    // file://server/share
    raw.append(rest);
    std::string bytes;  // percent-decoding works on UTF-8 bytes
    const std::string utf8 = narrow(raw);
    for (size_t i = 0; i < utf8.size(); ++i) {
        auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        if (utf8[i] == '%' && i + 2 < utf8.size() && hex(utf8[i + 1]) >= 0 && hex(utf8[i + 2]) >= 0) {
            bytes.push_back(static_cast<char>(hex(utf8[i + 1]) * 16 + hex(utf8[i + 2])));
            i += 2;
        } else {
            bytes.push_back(utf8[i]);
        }
    }
    return widen(bytes);
}

std::wstring strip_trailing_separators(std::wstring p) {
    while (p.size() > 1 && p.back() == L'\\' && !(p.size() == 3 && p[1] == L':')) p.pop_back();
    return p;
}

}  // namespace

std::wstring expand_user_path(std::wstring_view raw, const std::wstring& home, const EnvExpander& expand) {
    std::wstring p(strip_quotes(raw));
    if (!p.empty() && p[0] == L'~' && (p.size() == 1 || is_sep(p[1])) && !home.empty()) p = home + p.substr(1);
    if (expand && p.find(L'%') != std::wstring::npos) p = expand(p);
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    return p;
}

std::wstring last_path_part(std::wstring_view path) {
    while (path.size() > 1 && is_sep(path.back())) path.remove_suffix(1);
    const size_t pos = path.find_last_of(L"\\/");
    return std::wstring(pos == std::wstring_view::npos ? path : path.substr(pos + 1));
}

bool is_unc_path(std::wstring_view path) { return path.size() >= 2 && is_sep(path[0]) && is_sep(path[1]); }

bool is_absolute_path(std::wstring_view p) {
    if (p.size() >= 3 && is_ascii_letter(p[0]) && p[1] == L':' && is_sep(p[2])) return true;
    return p.size() >= 3 && is_unc_path(p) && !is_sep(p[2]);  // "\\server..."
}

bool looks_like_path_start(std::wstring_view q) {
    q = trim(q);
    if (!q.empty() && q.front() == L'"') q.remove_prefix(1);
    if (q.empty()) return false;
    if (is_sep(q[0]) || q[0] == L'~' || q[0] == L'%') return true;
    return q.size() >= 2 && is_ascii_letter(q[0]) && q[1] == L':';
}

// ---- Browsing ----------------------------------------------------------------------------------

std::optional<PathQuery> parse_path_query(std::wstring_view query, const std::wstring& home, const EnvExpander& expand) {
    std::wstring_view q = trim(query);
    if (!q.empty() && q.front() == L'"') q.remove_prefix(1);  // a pasted "C:\x y\  (closing quote not typed yet)
    if (!q.empty() && q.back() == L'"') q.remove_suffix(1);
    // A separator must be typed: "~" or "%APPDATA%" alone stay a normal search.
    if (q.find_first_of(L"\\/") == std::wstring_view::npos) return std::nullopt;
    const std::wstring p = expand_user_path(q, home, expand);
    if (!is_absolute_path(p)) return std::nullopt;
    const size_t sep = p.find_last_of(L'\\');
    if (is_unc_path(p) && (sep == std::wstring::npos || sep < 2 || p.find(L'\\', 2) == std::wstring::npos)) return std::nullopt;  // "\\server" alone
    PathQuery r;
    r.dir = p.substr(0, sep + 1);
    r.filter = p.substr(sep + 1);
    return r;
}

std::optional<std::wstring> step_into_query(std::wstring_view folder, std::wstring_view query, const std::wstring& home,
                                            const EnvExpander& expand) {
    if (folder.empty()) return std::nullopt;
    const auto current = parse_path_query(query, home, expand);
    if (!current && looks_like_path_start(query)) return std::nullopt;
    std::wstring dir(folder);
    for (auto& c : dir)
        if (c == L'/') c = L'\\';
    while (dir.size() > 3 && dir.back() == L'\\') dir.pop_back();
    if (dir.back() != L'\\') dir.push_back(L'\\');
    if (current && fold(dir) == fold(current->dir)) return std::wstring(query);
    return dir;
}

DirListing list_directory(const std::wstring& dir, size_t max_entries) {
    namespace fs = std::filesystem;
    DirListing out;
    out.dir = dir;
    std::error_code ec;
    fs::directory_iterator it(fs::path(dir), fs::directory_options::skip_permission_denied, ec);
    if (ec) return out;
    out.ok = true;
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (out.entries.size() >= max_entries) {
            out.truncated = true;
            break;
        }
        DirEntry e;
        e.name = it->path().filename().wstring();
        if (e.name.empty() || e.name == L"." || e.name == L"..") continue;
        std::error_code e2;
        e.is_dir = it->is_directory(e2);
        e.folded = fold(e.name);
        e.mask = char_mask(e.folded);
        out.entries.push_back(std::move(e));
    }
    return out;
}

std::vector<DirMatch> rank_dir_entries(const DirListing& listing, std::wstring_view filter, size_t limit) {
    std::vector<DirMatch> out;
    const auto& es = listing.entries;
    const FuzzyMatcher m(filter);
    for (size_t i = 0; i < es.size(); ++i) {
        if (m.empty()) {
            out.push_back({static_cast<uint32_t>(i), 0});
            continue;
        }
        if ((es[i].mask & m.mask()) != m.mask()) continue;
        if (auto sc = m.match(es[i].name, es[i].folded)) out.push_back({static_cast<uint32_t>(i), *sc});
    }
    auto better = [&](const DirMatch& a, const DirMatch& b) {
        if (a.score != b.score) return a.score > b.score;
        const DirEntry& x = es[a.index];
        const DirEntry& y = es[b.index];
        if (x.is_dir != y.is_dir) return x.is_dir;
        if (!m.empty() && x.name.size() != y.name.size()) return x.name.size() < y.name.size();
        if (x.folded != y.folded) return x.folded < y.folded;
        return x.name < y.name;
    };
    const size_t n = std::min(limit, out.size());
    std::partial_sort(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n), out.end(), better);
    out.resize(n);
    return out;
}

// ---- Locations ---------------------------------------------------------------------------------

std::optional<FileLocation> parse_file_location(std::wstring_view text) {
    const std::wstring_view t = strip_quotes(text);
    if (t.size() < 3 || t.size() > 2000 || t.find_first_of(L"\r\n") != std::wstring_view::npos) return std::nullopt;

    // Whole compiler messages and Python traceback lines: the parser of the build output.
    if (auto issue = parse_build_line(narrow(t)); issue && issue->line > 0) {
        FileLocation loc{std::wstring(strip_quotes(widen(issue->file))), issue->line, 0};
        if (plausible_file(loc.file)) {
            // The column, when the message has one: "a.cpp(12,5)" or "a.cpp:12:5".
            const size_t at = t.find(loc.file);
            if (at != std::wstring_view::npos) {
                size_t i = at + loc.file.size();
                if (i < t.size() && (t[i] == L'(' || t[i] == L':')) {
                    ++i;
                    if (read_number(t, i) && i + 1 < t.size() && (t[i] == L',' || t[i] == L':') && is_dec(t[i + 1])) {
                        ++i;
                        if (auto col = read_number(t, i)) loc.column = *col;
                    }
                }
            }
            return loc;
        }
    }

    // Short forms. MSVC: "a.cpp(12)", "a.cpp(12,5)" (anything after "):" is the message). The first
    // "(digits" wins: "C:\Program Files (x86)\a.h(12)" skips "(x86)".
    for (size_t open = t.find(L'('); open != std::wstring_view::npos; open = t.find(L'(', open + 1)) {
        if (open == 0) continue;
        size_t i = open + 1;
        if (auto line = read_number(t, i)) {
            int col = 0;
            if (i < t.size() && t[i] == L',') {
                ++i;
                if (auto c = read_number(t, i)) col = *c;
                else i = std::wstring_view::npos;
            }
            if (i != std::wstring_view::npos && i < t.size() && t[i] == L')' && (i + 1 == t.size() || t[i + 1] == L':')) {
                FileLocation loc{std::wstring(trim(t.substr(0, open))), *line, col};
                if (*line > 0 && plausible_file(loc.file)) return loc;
            }
            break;
        }
    }
    // GCC / Clang / grep: "a.cpp:12", "a.cpp:12:5", "C:\x\a.cpp:12: ..." (the drive's colon is skipped).
    for (size_t c = t.find(L':'); c != std::wstring_view::npos; c = t.find(L':', c + 1)) {
        if (c == 1 && is_ascii_letter(t[0])) continue;
        size_t i = c + 1;
        auto line = read_number(t, i);
        if (!line) continue;
        int col = 0;
        if (i + 1 < t.size() && t[i] == L':' && is_dec(t[i + 1])) {
            ++i;
            if (auto cn = read_number(t, i)) col = *cn;
        }
        if (i < t.size() && t[i] != L':' && t[i] != L' ' && t[i] != L'\t') continue;
        FileLocation loc{std::wstring(trim(t.substr(0, c))), *line, col};
        if (*line > 0 && plausible_file(loc.file)) return loc;
        break;
    }
    return std::nullopt;
}

int location_match(std::wstring_view path, std::wstring_view written) {
    auto parts = [](std::wstring_view p) {
        std::vector<std::wstring> out;
        size_t start = 0;
        for (size_t i = 0; i <= p.size(); ++i) {
            if (i == p.size() || is_sep(p[i])) {
                if (i > start) out.push_back(fold(p.substr(start, i - start)));
                start = i + 1;
            }
        }
        return out;
    };
    const auto a = parts(path);
    const auto b = parts(written);
    int n = 0;
    for (auto x = a.rbegin(), y = b.rbegin(); x != a.rend() && y != b.rend() && *x == *y; ++x, ++y) {
        if (*y == L".") break;
        ++n;
    }
    return n;
}

// ---- Clipboard ---------------------------------------------------------------------------------

std::optional<ClipboardPath> parse_clipboard_path(std::wstring_view text, const std::wstring& home, const EnvExpander& expand) {
    if (text.size() > 8192) return std::nullopt;  // a document, not a path
    size_t start = 0;
    for (int lines = 0; start <= text.size() && lines < 20; ++lines) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring_view::npos) end = text.size();
        std::wstring_view line = trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty()) continue;
        std::wstring candidate(strip_quotes(line));
        if (auto uri = from_file_uri(candidate)) candidate = *uri;

        ClipboardPath out;
        std::wstring file = candidate;
        if (auto loc = parse_file_location(candidate)) {
            file = loc->file;
            out.line = loc->line;
            out.column = loc->column;
        }
        const std::wstring p = expand_user_path(file, home, expand);
        if (!is_absolute_path(p) || !plausible_file(p)) continue;
        out.path = strip_trailing_separators(p);
        return out;
    }
    return std::nullopt;
}

}  // namespace kamil
