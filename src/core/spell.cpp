#include "core/spell.h"

#include <algorithm>
#include <map>

#include "core/text.h"

namespace kamil {

namespace {

bool is_space(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x00A0; }
bool is_digit_char(wchar_t c) { return c >= L'0' && c <= L'9'; }

}  // namespace

std::wstring_view token_around(std::wstring_view text, size_t start, size_t length) {
    if (start > text.size()) return {};
    size_t a = start, b = std::min(text.size(), start + length);
    while (a > 0 && !is_space(text[a - 1])) --a;
    while (b < text.size() && !is_space(text[b])) ++b;
    return text.substr(a, b - a);
}

bool skip_spell_word(std::wstring_view word, std::wstring_view token) {
    if (word.size() < 2) return true;
    for (wchar_t c : token)
        if (c == L'@' || c == L'\\' || c == L'/' || c == L'_' || is_digit_char(c)) return true;  // e-mail, path, URL, identifier
    // An inner dot: file names, namespaces, domains ("CMakeLists.txt", "std.vector", "example.com").
    // (sentence punctuation around the token does not count: "build." is a word, "a.cpp" is not)
    auto punct = [](wchar_t c) { return std::wstring_view(L".,;:!?()[]{}\"'<>«»“”‘’…-").find(c) != std::wstring_view::npos; };
    std::wstring_view t = token;
    while (!t.empty() && punct(t.back())) t.remove_suffix(1);
    while (!t.empty() && punct(t.front())) t.remove_prefix(1);
    if (t.find(L'.') != std::wstring_view::npos || t.find(L"::") != std::wstring_view::npos) return true;
    size_t upper = 0;
    bool inner_capital = false;
    for (size_t i = 0; i < word.size(); ++i) {
        if (is_upper(word[i])) {
            ++upper;
            if (i > 0 && is_lower(word[i - 1])) inner_capital = true;  // camelCase, PascalCase, iPhone
        }
    }
    if (inner_capital) return true;
    return upper == word.size();  // CI, API, CMAKE
}

SpellEdit spell_edit(std::wstring_view text, const SpellIssue& issue, std::wstring_view replacement) {
    SpellEdit e{issue.start, issue.length, std::wstring(replacement)};
    if (issue.kind == SpellIssue::Kind::Repeated) {
        e.text.clear();
        // "the the": the second word goes with the space before it.
        if (e.start > 0 && e.start <= text.size() && (text[e.start - 1] == L' ' || text[e.start - 1] == L'\t')) {
            --e.start;
            ++e.length;
        }
    }
    return e;
}

std::wstring apply_spell_edits(std::wstring_view text, std::vector<SpellEdit> edits) {
    std::sort(edits.begin(), edits.end(), [](const SpellEdit& a, const SpellEdit& b) { return a.start < b.start; });
    std::wstring out;
    out.reserve(text.size());
    size_t pos = 0;
    for (const auto& e : edits) {
        if (e.start < pos || e.start + e.length > text.size()) continue;  // overlaps the previous one
        out.append(text.substr(pos, e.start - pos));
        out.append(e.text);
        pos = e.start + e.length;
    }
    out.append(text.substr(pos));
    return out;
}

void SpellSession::set_languages(std::vector<std::unique_ptr<SpellLanguage>> languages) { languages_ = std::move(languages); }

void SpellSession::ignore(std::wstring_view word) { ignored_.insert(fold(word)); }

bool SpellSession::ignored(std::wstring_view word) const { return ignored_.count(fold(word)) != 0; }

void SpellSession::add_to_dictionary(std::wstring_view word) {
    if (!languages_.empty()) languages_.front()->add(word);
    ignore(word);  // accepted at once, also by the other languages
}

std::vector<SpellIssue> SpellSession::check(std::wstring_view text) {
    std::vector<SpellIssue> out;
    if (languages_.empty() || text.empty()) return out;
    const size_t n = languages_.size();
    std::vector<std::vector<RawSpellError>> found(n);
    for (size_t i = 0; i < n; ++i) found[i] = languages_[i]->check(text);

    std::map<std::wstring, bool> accepted_by;  // word + '\0' + language index -> accepted (cache)
    auto accepts = [&](size_t lang, std::wstring_view word) {
        // A language that reported the same range itself does not accept it.
        for (const auto& e : found[lang])
            if (e.action != RawSpellError::Action::Delete && text.substr(e.start, e.length) == word) return false;
        std::wstring key(word);
        key.push_back(L'\0');
        key.push_back(static_cast<wchar_t>(L'0' + lang));
        auto it = accepted_by.find(key);
        if (it == accepted_by.end()) it = accepted_by.emplace(key, languages_[lang]->accepts(word)).first;
        return it->second;
    };

    for (size_t i = 0; i < n; ++i) {
        for (const auto& e : found[i]) {
            if (e.start >= text.size() || e.length == 0 || e.start + e.length > text.size()) continue;
            const std::wstring_view word = text.substr(e.start, e.length);
            SpellIssue issue{e.start, e.length, SpellIssue::Kind::Misspelled, {}};
            if (e.action == RawSpellError::Action::Delete) {
                issue.kind = SpellIssue::Kind::Repeated;
            } else {
                if (ignored(word) || skip_spell_word(word, token_around(text, e.start, e.length))) continue;
                bool other_accepts = false;
                for (size_t j = 0; j < n && !other_accepts; ++j)
                    if (j != i) other_accepts = accepts(j, word);
                if (other_accepts) continue;
                if (e.action == RawSpellError::Action::Replace) issue.autocorrect = e.replacement;
            }
            out.push_back(std::move(issue));
        }
    }
    // In text order; the same range reported by several languages once; no overlaps.
    std::stable_sort(out.begin(), out.end(), [](const SpellIssue& a, const SpellIssue& b) { return a.start < b.start; });
    std::vector<SpellIssue> merged;
    for (auto& issue : out) {
        if (!merged.empty() && issue.start < merged.back().start + merged.back().length) {
            if (merged.back().autocorrect.empty() && !issue.autocorrect.empty() && issue.start == merged.back().start)
                merged.back().autocorrect = issue.autocorrect;
            continue;
        }
        merged.push_back(std::move(issue));
    }
    return merged;
}

std::vector<std::wstring> SpellSession::suggestions(std::wstring_view text, const SpellIssue& issue, size_t max) {
    std::vector<std::wstring> out;
    if (issue.kind == SpellIssue::Kind::Repeated || issue.start + issue.length > text.size()) return out;
    const std::wstring_view word = text.substr(issue.start, issue.length);
    auto add = [&](const std::wstring& s) {
        if (out.size() < max && !s.empty() && s != word && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    };
    add(issue.autocorrect);
    // Round robin over the languages: the best of each first ("degisiklik" -> Turkish, "recieved" -> English).
    std::vector<std::vector<std::wstring>> lists;
    for (auto& l : languages_) lists.push_back(l->suggest(word));
    for (size_t k = 0; out.size() < max; ++k) {
        bool any = false;
        for (const auto& list : lists)
            if (k < list.size()) {
                any = true;
                add(list[k]);
            }
        if (!any) break;
    }
    return out;
}

std::wstring SpellSession::fix_all(std::wstring_view text, size_t* fixed) {
    std::vector<SpellEdit> edits;
    for (const auto& issue : check(text)) {
        if (issue.kind == SpellIssue::Kind::Repeated) {
            edits.push_back(spell_edit(text, issue, L""));
            continue;
        }
        const auto s = suggestions(text, issue, 1);
        if (!s.empty()) edits.push_back(spell_edit(text, issue, s.front()));
    }
    if (fixed) *fixed = edits.size();
    return apply_spell_edits(text, std::move(edits));
}

}  // namespace kamil
