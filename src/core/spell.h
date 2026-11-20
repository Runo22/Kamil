#pragma once

// Spell checking with several languages at once (Turkish + English text is common at work): a
// word is wrong only when no active language accepts it. The dictionaries are Windows' own
// (platform/spell_checker); this part is platform independent and unit tested with fakes.

#include <cstddef>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace kamil {

// What one language's checker reports for a piece of text.
struct RawSpellError {
    enum class Action { Suggest, Replace, Delete };  // Replace: a known autocorrection; Delete: a repeated word
    size_t start = 0;
    size_t length = 0;
    Action action = Action::Suggest;
    std::wstring replacement;  // Action::Replace
};

// One dictionary (Windows: an ISpellChecker for a language tag).
class SpellLanguage {
public:
    virtual ~SpellLanguage() = default;
    virtual const std::wstring& tag() const = 0;  // "tr-TR"
    virtual std::vector<RawSpellError> check(std::wstring_view text) = 0;
    virtual bool accepts(std::wstring_view word) = 0;
    virtual std::vector<std::wstring> suggest(std::wstring_view word) = 0;
    virtual void add(std::wstring_view word) = 0;  // to the user's dictionary of this language
};

struct SpellIssue {
    enum class Kind { Misspelled, Repeated };
    size_t start = 0;
    size_t length = 0;
    Kind kind = Kind::Misspelled;
    std::wstring autocorrect;  // a known correction ("teh" -> "the"); the first suggestion when set
};

// Text that is not prose and is never marked: anything with digits, '_', '@', '/', '\\', '://',
// an inner '.', inner capitals (camelCase, PascalCase), acronyms (CI, API, VS2026), one letter.
// `token` is the whitespace-delimited run around the word (a URL / path / identifier as a whole).
bool skip_spell_word(std::wstring_view word, std::wstring_view token);

// The whitespace-delimited run of `text` that contains [start, start + length).
std::wstring_view token_around(std::wstring_view text, size_t start, size_t length);

// A replacement in the text.
struct SpellEdit {
    size_t start = 0;
    size_t length = 0;
    std::wstring text;
};

// The edit that fixes `issue` with `replacement`: a repeated word is removed together with the
// space before it ("the the x" -> "the x").
SpellEdit spell_edit(std::wstring_view text, const SpellIssue& issue, std::wstring_view replacement);

// `text` with `edits` applied (any order; overlapping edits after the first are skipped).
std::wstring apply_spell_edits(std::wstring_view text, std::vector<SpellEdit> edits);

// The checked text in all active languages, with what the user ignored this session.
class SpellSession {
public:
    void set_languages(std::vector<std::unique_ptr<SpellLanguage>> languages);
    const std::vector<std::unique_ptr<SpellLanguage>>& languages() const { return languages_; }
    bool empty() const { return languages_.empty(); }

    // The issues of `text`, in order, not overlapping.
    std::vector<SpellIssue> check(std::wstring_view text);
    // Suggestions for an issue (the autocorrection first, then each language's, no duplicates);
    // a repeated word has none (the fix is to delete it).
    std::vector<std::wstring> suggestions(std::wstring_view text, const SpellIssue& issue, size_t max = 6);
    // `text` with every issue fixed by its first suggestion (issues without one stay as they are).
    // `fixed` receives how many were fixed.
    std::wstring fix_all(std::wstring_view text, size_t* fixed = nullptr);

    void ignore(std::wstring_view word);              // this session, every occurrence
    void add_to_dictionary(std::wstring_view word);   // the first language's user dictionary
    bool ignored(std::wstring_view word) const;

private:
    std::vector<std::unique_ptr<SpellLanguage>> languages_;
    std::set<std::wstring> ignored_;
};

}  // namespace kamil
