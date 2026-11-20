// Spell check: the "Spell check" command (window with the copied text), "yaz <words>" in the
// search, and pasting the corrected text back into the program Kamil was opened over.

#include <algorithm>
#include <fstream>
#include <iterator>

#include "../res/resource.h"
#include "app/action_item.h"
#include "app/app.h"
#include "core/i18n.h"
#include "core/log.h"
#include "core/settings_schema.h"
#include "core/text.h"
#include "platform/shell.h"

#include <commctrl.h>
#include <shellapi.h>

namespace kamil {

namespace {

constexpr size_t kMaxSpellText = 200'000;  // characters; more is a document, not a message

bool process_elevated(HWND window) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return true;  // not even that: assume it is out of reach
    bool elevated = true;
    HANDLE token = nullptr;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION e{};
        DWORD size = 0;
        elevated = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size) && e.TokenIsElevated;
        CloseHandle(token);
    }
    CloseHandle(process);
    return elevated;
}

bool self_elevated() {
    static const bool elevated = [] {
        HANDLE token = nullptr;
        bool e = false;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            TOKEN_ELEVATION te{};
            DWORD size = 0;
            e = GetTokenInformation(token, TokenElevation, &te, sizeof(te), &size) && te.TokenIsElevated;
            CloseHandle(token);
        }
        return e;
    }();
    return elevated;
}

}  // namespace

void App::remember_foreground() {
    HWND fg = GetForegroundWindow();
    foreground_before_ = nullptr;
    if (!fg) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return;  // Kamil's own windows
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, static_cast<int>(std::size(cls)));
    const std::wstring_view c(cls);
    if (c == L"Shell_TrayWnd" || c == L"Shell_SecondaryTrayWnd" || c == L"Progman" || c == L"WorkerW") return;  // taskbar, desktop
    foreground_before_ = fg;
}

void App::ensure_spell() {
    if (spell_) return;
    spell_ = std::make_unique<WindowsSpell>();
    // The languages switched on in the window last time.
    std::ifstream in(paths_.config_dir / "spell-languages.txt", std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        const std::wstring tag(trim(std::wstring_view(widen(line))));
        if (!tag.empty()) spell_active_.push_back(tag);
    }
    rebuild_spell_session();
}

std::vector<SpellLanguageChip> App::spell_chips() {
    std::vector<SpellLanguageChip> chips;
    if (!spell_) return chips;
    std::vector<std::wstring> offered;
    for (const auto& s : store_.current()->get_list(keys::kSpellLanguages)) {
        const std::wstring tag(trim(std::wstring_view(widen(s))));
        if (!tag.empty() && std::find(offered.begin(), offered.end(), tag) == offered.end()) offered.push_back(tag);
    }
    for (const auto& tag : offered) {
        // "English (United Kingdom)" only when two variants of a language are offered.
        const std::wstring lang = tag.substr(0, tag.find(L'-'));
        const bool twins = std::count_if(offered.begin(), offered.end(), [&](const std::wstring& o) { return o.substr(0, o.find(L'-')) == lang; }) > 1;
        SpellLanguageChip c;
        c.tag = tag;
        c.name = spell_language_name(tag, twins);
        c.installed = spell_->supports(tag);
        c.active = spell_active_.empty() || std::find(spell_active_.begin(), spell_active_.end(), tag) != spell_active_.end();
        chips.push_back(std::move(c));
    }
    // Every offered language switched off (or the saved ones are gone): all of them again.
    if (std::none_of(chips.begin(), chips.end(), [](const SpellLanguageChip& c) { return c.active && c.installed; }))
        for (auto& c : chips) c.active = true;
    return chips;
}

void App::rebuild_spell_session() {
    std::vector<std::unique_ptr<SpellLanguage>> langs;
    std::wstring names;
    for (const auto& c : spell_chips()) {
        if (!c.active || !c.installed) continue;
        if (auto l = spell_->open(c.tag)) {
            langs.push_back(std::move(l));
            names += (names.empty() ? L"" : L", ") + c.tag;
        }
    }
    spell_session_.set_languages(std::move(langs));
    log_line(L"[spell] languages: " + (names.empty() ? std::wstring(L"none") : names));
}

void App::open_spell(std::wstring text) {
    ensure_spell();
    if (!spell_window_ok_) {
        HICON big = nullptr;
        if (FAILED(LoadIconMetric(instance_, MAKEINTRESOURCEW(IDI_KAMIL), LIM_LARGE, &big))) big = icon_;
        SpellWindow::Callbacks cb;
        cb.done = [this](const std::wstring& t, bool paste_back) { spell_done(t, paste_back); };
        cb.languages_changed = [this](const std::vector<std::wstring>& tags) {
            spell_active_ = tags;
            std::ofstream out(paths_.config_dir / "spell-languages.txt", std::ios::binary | std::ios::trunc);
            for (const auto& t : tags) out << narrow(t) << "\n";
            rebuild_spell_session();
            spell_window_.set_chips(spell_chips());
        };
        cb.install_languages = [] { ShellExecuteW(nullptr, L"open", L"ms-settings:regionlanguage", nullptr, nullptr, SW_SHOWNORMAL); };
        spell_window_ok_ = spell_window_.create(instance_, big, std::move(cb));
        if (!spell_window_ok_) {
            notify(loc(L"Spell check could not start", L"Yazım denetimi açılamadı"), loc(L"See kamil.log.", L"Ayrıntı: kamil.log."),
                   Tray::Balloon::Error);
            log_line("[spell] the window could not be created (Msftedit.dll / Direct2D)");
            return;
        }
        update_console_theme();  // also the spell window's
    }
    if (text.size() > kMaxSpellText) {
        notify(loc(L"Spell check", L"Yazım denetimi"), loc(L"The copied text is too long; the window opens empty.", L"Panodaki metin çok uzun; pencere boş açılıyor."),
               Tray::Balloon::Info);
        text.clear();
    }
    if (spell_session_.empty())
        notify(loc(L"No spell checker language", L"Yazım denetimi dili yok"),
               loc(L"Add Turkish or English in Windows: Settings > Time & language > Language.",
                   L"Windows'ta Türkçe ya da İngilizce ekle: Ayarlar > Saat ve dil > Dil."),
               Tray::Balloon::Warning);
    spell_target_ = foreground_before_;
    spell_window_.open(text, &spell_session_, spell_chips(), store_.current()->get_bool(keys::kSpellPasteBack));
}

void App::spell_done(const std::wstring& text, bool paste_back) {
    copy_to_clipboard(hwnd_, text);
    spell_window_.hide();
    HWND target = spell_target_;
    if (!paste_back) {
        notify(loc(L"Copied", L"Kopyalandı"), loc(L"The corrected text is on the clipboard.", L"Düzeltilmiş metin panoda."), Tray::Balloon::Info);
        return;
    }
    if (!target || !IsWindow(target)) {
        notify(loc(L"Copied", L"Kopyalandı"), loc(L"The corrected text is on the clipboard: paste it with Ctrl+V.", L"Düzeltilmiş metin panoda: Ctrl+V ile yapıştır."),
               Tray::Balloon::Info);
        return;
    }
    if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
    SetForegroundWindow(target);
    if (process_elevated(target) && !self_elevated()) {
        // Windows does not let a normal program type into one running as administrator.
        notify(loc(L"Copied", L"Kopyalandı"),
               loc(L"That program runs as administrator, Kamil cannot paste into it: press Ctrl+V.",
                   L"O program yönetici olarak çalışıyor, Kamil yapıştıramaz: Ctrl+V'ye bas."),
               Tray::Balloon::Info);
        return;
    }
    SetTimer(hwnd_, kTimerPasteBack, 150, nullptr);  // let it take the focus first
}

void App::paste_back_now() {
    KillTimer(hwnd_, kTimerPasteBack);
    HWND target = spell_target_;
    HWND fg = GetForegroundWindow();
    if (!target || (fg != target && GetAncestor(fg, GA_ROOTOWNER) != target)) {
        notify(loc(L"Copied", L"Kopyalandı"), loc(L"The corrected text is on the clipboard: paste it with Ctrl+V.", L"Düzeltilmiş metin panoda: Ctrl+V ile yapıştır."),
               Tray::Balloon::Info);
        return;
    }
    // Ctrl+V; Shift / Alt still held from Ctrl+Shift+Enter are released first.
    std::vector<INPUT> keys;
    auto key = [&](WORD vk, bool up) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        keys.push_back(in);
    };
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) key(VK_SHIFT, true);
    if (GetAsyncKeyState(VK_MENU) & 0x8000) key(VK_MENU, true);
    key(VK_CONTROL, false);
    key('V', false);
    key('V', true);
    key(VK_CONTROL, true);
    SendInput(static_cast<UINT>(keys.size()), keys.data(), sizeof(INPUT));
    log_line("[spell] pasted back");
}

bool App::spell_query(const std::wstring& query, std::vector<Hit>& hits) {
    auto rest = scoped_query(query, L"yaz");
    if (!rest) rest = scoped_query(query, L"spell");
    if (!rest || rest->empty()) return false;  // "yaz" alone finds the Spell check command
    ensure_spell();
    query_items_.clear();
    query_items_.reserve(10);  // hits point into query_items_: it must never reallocate
    int score = 1000;
    auto add = [&](const std::wstring& title, const std::wstring& subtitle, const std::wstring& target) {
        Item it;
        it.kind = ItemKind::Command;
        it.key = L"spell:" + title;
        it.target = target;
        it.title = title;
        it.subtitle = subtitle;
        it.glyph = kGlyphSpell;
        it.prepare();
        query_items_.push_back(std::move(it));
        hits.push_back(Hit{&query_items_.back(), score--, {}});
    };
    if (spell_session_.empty()) {
        add(loc(L"No spell checker language", L"Yazım denetimi dili yok"),
            loc(L"Add Turkish / English in Windows: Settings > Time & language > Language", L"Windows'ta Türkçe / İngilizce ekle: Ayarlar > Saat ve dil > Dil"),
            L"spell-open:");
        return true;
    }
    const std::wstring text = *rest;
    const auto issues = spell_session_.check(text);
    const std::wstring copy = loc(L"Enter: copy", L"Enter: kopyala");
    if (issues.empty()) {
        add(text, loc(L"✓ Spelled correctly · Enter: copy", L"✓ Doğru yazılmış · Enter: kopyala"), L"spell-copy:" + text);
        return true;
    }
    const bool one_word = text.find(L' ') == std::wstring::npos && issues.size() == 1 && issues[0].start == 0 && issues[0].length == text.size();
    if (one_word) {
        const auto suggestions = spell_session_.suggestions(text, issues[0], 7);
        for (const auto& s : suggestions) add(s, copy, L"spell-copy:" + s);
        if (suggestions.empty()) add(text, loc(L"No suggestions", L"Öneri yok"), L"spell-copy:" + text);
        return true;
    }
    size_t fixed = 0;
    const std::wstring corrected = spell_session_.fix_all(text, &fixed);
    add(corrected, fmt(loc(L"{} fixed · Enter: copy", L"{} düzeltildi · Enter: kopyala"), std::to_wstring(fixed)), L"spell-copy:" + corrected);
    add(loc(L"Open in the spell check window", L"Yazım denetimi penceresinde aç"), text, L"spell-open:" + text);
    return true;
}

}  // namespace kamil
