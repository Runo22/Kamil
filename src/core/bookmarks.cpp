#include "core/bookmarks.h"

#include <unordered_set>

#include "core/text.h"

namespace kamil {

std::wstring normalize_bookmark_path(std::wstring_view raw, const std::wstring& home, const EnvExpander& expand) {
    std::wstring p = expand_user_path(raw, home, expand);
    // "C:\\" keeps its separator ("C:" alone means the current folder of drive C).
    while (p.size() > 1 && p.back() == L'\\' && !(p.size() == 3 && p[1] == L':')) p.pop_back();
    if (p == L"\\") p.clear();
    return p;
}

std::vector<Bookmark> parse_bookmarks(const Value::Objects& objects, const std::wstring& home, const EnvExpander& expand) {
    std::vector<Bookmark> out;
    std::unordered_set<std::wstring> seen;
    for (const auto& obj : objects) {
        const Value* p = obj.find("path");
        if (!p || p->is_null()) continue;
        Bookmark b;
        b.path = normalize_bookmark_path(widen(p->as_string()), home, expand);
        if (b.path.empty() || !seen.insert(fold(b.path)).second) continue;
        if (const Value* n = obj.find("name"); n && !n->is_null()) b.name = std::wstring(trim(std::wstring_view(widen(n->as_string()))));
        if (b.name.empty()) b.name = last_path_part(b.path);
        if (b.name.empty()) b.name = b.path;
        if (const Value* k = obj.find("keyword"); k && !k->is_null()) b.keyword_folded = fold(trim(std::wstring_view(widen(k->as_string()))));
        out.push_back(std::move(b));
    }
    return out;
}

std::wstring bookmark_keywords(const Bookmark& b) {
    std::wstring part = last_path_part(b.path);
    if (fold(part) == fold(b.name)) return {};
    // ".conan" is also found as "conan" (the fuzzy match would find it anyway, but scores it lower).
    if (part.size() > 1 && part[0] == L'.') part += L" " + part.substr(1);
    return part;
}

}  // namespace kamil
