#include "platform/spell_checker.h"

#include "platform/win.h"

#include <objbase.h>
#include <spellcheck.h>

#include <algorithm>

#include "core/log.h"

namespace kamil {

namespace {

std::vector<std::wstring> read_strings(IEnumString* e) {
    std::vector<std::wstring> out;
    if (!e) return out;
    LPOLESTR s = nullptr;
    while (e->Next(1, &s, nullptr) == S_OK) {
        if (s) out.emplace_back(s);
        CoTaskMemFree(s);
        s = nullptr;
    }
    return out;
}

class WinSpellLanguage : public SpellLanguage {
public:
    WinSpellLanguage(std::wstring tag, ComPtr<ISpellChecker> checker) : tag_(std::move(tag)), checker_(std::move(checker)) {}
    const std::wstring& tag() const override { return tag_; }

    std::vector<RawSpellError> check(std::wstring_view text) override {
        std::vector<RawSpellError> out;
        const std::wstring s(text);  // the API needs a terminated string
        ComPtr<IEnumSpellingError> errors;
        if (FAILED(checker_->ComprehensiveCheck(s.c_str(), &errors)) || !errors) return out;
        ComPtr<ISpellingError> e;
        while (errors->Next(&e) == S_OK && e) {
            ULONG start = 0, length = 0;
            CORRECTIVE_ACTION action = CORRECTIVE_ACTION_NONE;
            e->get_StartIndex(&start);
            e->get_Length(&length);
            e->get_CorrectiveAction(&action);
            RawSpellError r;
            r.start = start;
            r.length = length;
            if (action == CORRECTIVE_ACTION_REPLACE) {
                r.action = RawSpellError::Action::Replace;
                LPWSTR rep = nullptr;
                if (SUCCEEDED(e->get_Replacement(&rep)) && rep) r.replacement = rep;
                CoTaskMemFree(rep);
            } else if (action == CORRECTIVE_ACTION_DELETE) {
                r.action = RawSpellError::Action::Delete;
            }
            if (action != CORRECTIVE_ACTION_NONE) out.push_back(std::move(r));
            e.Reset();
        }
        return out;
    }

    bool accepts(std::wstring_view word) override {
        const std::wstring s(word);
        ComPtr<IEnumSpellingError> errors;
        if (FAILED(checker_->Check(s.c_str(), &errors)) || !errors) return true;
        ComPtr<ISpellingError> e;
        return errors->Next(&e) != S_OK;  // no error: a known word
    }

    std::vector<std::wstring> suggest(std::wstring_view word) override {
        const std::wstring s(word);
        ComPtr<IEnumString> list;
        if (FAILED(checker_->Suggest(s.c_str(), &list))) return {};
        return read_strings(list.Get());
    }

    void add(std::wstring_view word) override {
        const std::wstring s(word);
        if (FAILED(checker_->Add(s.c_str()))) log_line(L"[spell] could not add to the " + tag_ + L" dictionary: " + s);
    }

private:
    std::wstring tag_;
    ComPtr<ISpellChecker> checker_;
};

}  // namespace

struct WindowsSpell::Impl {
    ComPtr<ISpellCheckerFactory> factory;
    bool tried = false;

    ISpellCheckerFactory* get() {
        if (!tried) {
            tried = true;
            const HRESULT hr = CoCreateInstance(__uuidof(SpellCheckerFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            if (FAILED(hr)) log_line("[spell] the Windows spell checking service is not available (0x" + std::to_string(static_cast<unsigned long>(hr)) + ")");
        }
        return factory.Get();
    }
};

WindowsSpell::WindowsSpell() : impl_(std::make_unique<Impl>()) {}
WindowsSpell::~WindowsSpell() = default;

bool WindowsSpell::available() { return impl_->get() != nullptr; }

std::vector<std::wstring> WindowsSpell::languages() {
    ISpellCheckerFactory* f = impl_->get();
    if (!f) return {};
    ComPtr<IEnumString> list;
    if (FAILED(f->get_SupportedLanguages(&list))) return {};
    auto out = read_strings(list.Get());
    std::sort(out.begin(), out.end());
    return out;
}

bool WindowsSpell::supports(const std::wstring& tag) {
    ISpellCheckerFactory* f = impl_->get();
    BOOL ok = FALSE;
    return f && SUCCEEDED(f->IsSupported(tag.c_str(), &ok)) && ok;
}

std::unique_ptr<SpellLanguage> WindowsSpell::open(const std::wstring& tag) {
    ISpellCheckerFactory* f = impl_->get();
    if (!f || !supports(tag)) return nullptr;
    ComPtr<ISpellChecker> checker;
    if (FAILED(f->CreateSpellChecker(tag.c_str(), &checker)) || !checker) return nullptr;
    return std::make_unique<WinSpellLanguage>(tag, std::move(checker));
}

std::wstring spell_language_name(const std::wstring& tag, bool with_region) {
    wchar_t buf[LOCALE_NAME_MAX_LENGTH * 2] = {};
    const LCTYPE what = with_region ? LOCALE_SNATIVEDISPLAYNAME : LOCALE_SNATIVELANGUAGENAME;
    if (GetLocaleInfoEx(tag.c_str(), what, buf, static_cast<int>(std::size(buf))) <= 0 || !buf[0]) return tag;
    std::wstring name(buf);
    if (!name.empty()) CharUpperBuffW(name.data(), 1);  // "türkçe" -> "Türkçe"
    return name;
}

}  // namespace kamil
