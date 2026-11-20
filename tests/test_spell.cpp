#include <map>
#include <set>

#include "core/spell.h"
#include "core/text.h"
#include "test.h"

using namespace kamil;

namespace {

// A dictionary that knows a fixed word list; it reports every other word, "the the" style repeats
// and the autocorrections it was given.
// Case insensitive only (fold() would also drop accents: "Lutfen" must not match "lütfen").
std::wstring lower(std::wstring_view w) {
    std::wstring out(w);
    for (auto& c : out)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return out;
}

class FakeLanguage : public SpellLanguage {
public:
    FakeLanguage(std::wstring tag, std::set<std::wstring> words, std::map<std::wstring, std::vector<std::wstring>> suggestions = {},
                 std::map<std::wstring, std::wstring> autocorrect = {})
        : tag_(std::move(tag)), suggestions_(std::move(suggestions)), autocorrect_(std::move(autocorrect)) {
        for (const auto& w : words) words_.insert(lower(w));
    }
    const std::wstring& tag() const override { return tag_; }
    std::vector<RawSpellError> check(std::wstring_view text) override {
        std::vector<RawSpellError> out;
        std::wstring prev;
        size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && !is_word_char(text[i])) ++i;
            const size_t start = i;
            while (i < text.size() && is_word_char(text[i])) ++i;
            if (i == start) break;
            const std::wstring w(text.substr(start, i - start));
            if (!prev.empty() && lower(prev) == lower(w)) out.push_back({start, w.size(), RawSpellError::Action::Delete, {}});
            else if (auto a = autocorrect_.find(w); a != autocorrect_.end()) out.push_back({start, w.size(), RawSpellError::Action::Replace, a->second});
            else if (!words_.count(lower(w))) out.push_back({start, w.size(), RawSpellError::Action::Suggest, {}});
            prev = w;
        }
        ++checks;
        return out;
    }
    bool accepts(std::wstring_view word) override {
        ++accept_calls;
        return words_.count(lower(word)) != 0;
    }
    std::vector<std::wstring> suggest(std::wstring_view word) override {
        auto it = suggestions_.find(std::wstring(word));
        return it == suggestions_.end() ? std::vector<std::wstring>{} : it->second;
    }
    void add(std::wstring_view word) override { added.emplace_back(word); }

    int checks = 0, accept_calls = 0;
    std::vector<std::wstring> added;

private:
    static bool is_word_char(wchar_t c) { return c != L' ' && c != L'.' && c != L',' && c != L'\r' && c != L'\n'; }
    std::wstring tag_;
    std::set<std::wstring> words_;
    std::map<std::wstring, std::vector<std::wstring>> suggestions_;
    std::map<std::wstring, std::wstring> autocorrect_;
};

SpellSession session_tr_en(FakeLanguage** tr_out = nullptr, FakeLanguage** en_out = nullptr) {
    auto tr = std::make_unique<FakeLanguage>(
        L"tr-TR", std::set<std::wstring>{L"yarın", L"build", L"hatalarını", L"lütfen", L"değişiklikleri", L"test", L"edin", L"önce"},
        std::map<std::wstring, std::vector<std::wstring>>{{L"degişiklikleri", {L"değişiklikleri", L"değişikleri"}}, {L"Lutfen", {L"Lütfen"}}});
    auto en = std::make_unique<FakeLanguage>(L"en-US", std::set<std::wstring>{L"the", L"release", L"branch", L"is", L"received", L"from", L"test", L"build", L"once"},
                                             std::map<std::wstring, std::vector<std::wstring>>{{L"recieved", {L"received", L"relieved"}},
                                                                                               {L"degişiklikleri", {L"designs"}}},
                                             std::map<std::wstring, std::wstring>{{L"teh", L"the"}});
    if (tr_out) *tr_out = tr.get();
    if (en_out) *en_out = en.get();
    std::vector<std::unique_ptr<SpellLanguage>> langs;
    langs.push_back(std::move(tr));
    langs.push_back(std::move(en));
    SpellSession s;
    s.set_languages(std::move(langs));
    return s;
}

std::vector<std::wstring> words_of(std::wstring_view text, const std::vector<SpellIssue>& issues) {
    std::vector<std::wstring> out;
    for (const auto& i : issues) out.emplace_back(text.substr(i.start, i.length));
    return out;
}

}  // namespace

TEST(spell_two_languages) {
    FakeLanguage* tr = nullptr;
    SpellSession s = session_tr_en(&tr);
    const std::wstring text = widen("Yarın build hatalarını Lutfen degişiklikleri test edin. The the release branch is recieved from teh build.");
    const auto issues = s.check(text);
    // "build", "test" are accepted by one language: not issues. "Yarın" is accepted (case insensitive here).
    CHECK((words_of(text, issues) == std::vector<std::wstring>{L"Lutfen", widen("degişiklikleri"), L"the", L"recieved", L"teh"}));
    if (issues.size() == 5) {
        CHECK(issues[2].kind == SpellIssue::Kind::Repeated);
        CHECK(issues[4].autocorrect == L"the");
        CHECK(s.suggestions(text, issues[1]) == (std::vector<std::wstring>{widen("değişiklikleri"), L"designs", widen("değişikleri")}));  // round robin
        CHECK(s.suggestions(text, issues[3]) == (std::vector<std::wstring>{L"received", L"relieved"}));
        CHECK(s.suggestions(text, issues[4]).front() == L"the");
        CHECK(s.suggestions(text, issues[2]).empty());  // a repeated word is deleted
        CHECK_EQ(s.suggestions(text, issues[1], 1).size(), 1u);
    }
    // "once" is English: with both languages it is not marked (the known limit of mixing languages).
    CHECK(s.check(L"once").empty());
}

TEST(spell_fix_all_and_edits) {
    SpellSession s = session_tr_en();
    const std::wstring text = widen("Lutfen degişiklikleri test edin. The the branch is recieved from teh build. Qwxyz.");
    size_t fixed = 0;
    CHECK_EQ(s.fix_all(text, &fixed), widen("Lütfen değişiklikleri test edin. The branch is received from the build. Qwxyz."));
    CHECK_EQ(fixed, 5u);  // Qwxyz has no suggestion: left as it is

    // Edits: a repeated word goes with the space before it.
    SpellIssue rep{4, 3, SpellIssue::Kind::Repeated, {}};
    const SpellEdit e = spell_edit(L"the the x", rep, L"ignored");
    CHECK(e.start == 3 && e.length == 4 && e.text.empty());
    CHECK_EQ(apply_spell_edits(L"the the x", {e}), L"the x");
    CHECK_EQ(apply_spell_edits(L"abc def", {{4, 3, L"DEF"}, {0, 3, L"ABC"}}), L"ABC DEF");  // any order
    CHECK_EQ(apply_spell_edits(L"abcdef", {{0, 4, L"X"}, {2, 2, L"Y"}}), L"Xef");           // overlapping: skipped
    CHECK_EQ(apply_spell_edits(L"abc", {{2, 5, L"X"}}), L"abc");                            // out of range: skipped
}

TEST(spell_ignore_and_dictionary) {
    FakeLanguage* tr = nullptr;
    SpellSession s = session_tr_en(&tr);
    const std::wstring text = L"Kamil qwxyz test Qwxyz";
    CHECK_EQ(s.check(text).size(), 3u);
    s.ignore(L"qwxyz");  // every occurrence, any case
    CHECK((words_of(text, s.check(text)) == std::vector<std::wstring>{L"Kamil"}));
    s.add_to_dictionary(L"Kamil");
    CHECK(s.check(text).empty());
    CHECK(tr->added.size() == 1 && tr->added[0] == L"Kamil");  // the first language's dictionary

    SpellSession none;
    CHECK(none.empty());
    CHECK(none.check(L"anything").empty());
}

TEST(spell_skips_code_and_names) {
    auto skip = [](const wchar_t* text, const wchar_t* word) {
        const std::wstring t(text);
        const size_t at = t.find(word);
        return skip_spell_word(std::wstring_view(t).substr(at, std::wcslen(word)), token_around(t, at, std::wcslen(word)));
    };
    CHECK(skip(L"see https://exampel.com/pathh now", L"exampel"));
    CHECK(skip(L"mail tuna@exampel.com", L"tuna"));
    CHECK(skip(L"open D:\\src\\resimm\\x", L"resimm"));
    CHECK(skip(L"call get_valuee() here", L"valuee"));
    CHECK(skip(L"edit CMakeListss.txt now", L"CMakeListss"));
    CHECK(skip(L"use std::vectr here", L"vectr"));
    CHECK(skip(L"the getValuee method", L"getValuee"));
    CHECK(skip(L"the SensorUii class", L"SensorUii"));
    CHECK(skip(L"build on CI and VS", L"CI"));
    CHECK(skip(L"VS2026 is here", L"VS2026"));
    CHECK(skip(L"a b", L"b"));
    // Prose is checked, with punctuation around it.
    CHECK(!skip(L"Bu bir denemee.", L"denemee"));
    CHECK(!skip(L"(recieved)", L"recieved"));
    CHECK(!skip(L"\"Lutfen\" dedi", L"Lutfen"));
    CHECK(!skip(L"Recieved it", L"Recieved"));
    CHECK(!skip(widen("Çalışmaa bitti").c_str(), widen("Çalışmaa").c_str()));
    CHECK_EQ(std::wstring(token_around(L"one two.three four", 5, 3)), L"two.three");
    CHECK_EQ(std::wstring(token_around(L"x", 5, 1)), L"");
}

TEST(spell_cache_and_counts) {
    FakeLanguage *tr = nullptr, *en = nullptr;
    SpellSession s = session_tr_en(&tr, &en);
    const std::wstring text = L"zzz zzz2 zzz qqq";  // zzz2: digits, skipped
    const auto issues = s.check(text);
    CHECK_EQ(issues.size(), 3u);
    CHECK_EQ(tr->checks, 1);
    CHECK_EQ(en->checks, 1);
    // Each language reported "zzz" and "qqq" itself: no extra lookups were needed.
    CHECK_EQ(tr->accept_calls + en->accept_calls, 0);
}
