#pragma once

// Windows' own spell checking (Windows Spell Checking API, Windows 8+): the dictionaries of the
// installed languages (Settings > Time & language > Language: "Basic typing"), plus the user's
// dictionary that Windows keeps per language. Nothing is shipped with Kamil.

#include <memory>
#include <string>
#include <vector>

#include "core/spell.h"

namespace kamil {

class WindowsSpell {
public:
    WindowsSpell();
    ~WindowsSpell();

    bool available();                           // the spell checking service could be started
    std::vector<std::wstring> languages();      // tags with a dictionary on this PC ("en-US", "tr-TR")
    bool supports(const std::wstring& tag);
    std::unique_ptr<SpellLanguage> open(const std::wstring& tag);  // nullptr: no dictionary

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// "Türkçe", "English"; with the region when `with_region` ("English (United Kingdom)").
std::wstring spell_language_name(const std::wstring& tag, bool with_region);

}  // namespace kamil
