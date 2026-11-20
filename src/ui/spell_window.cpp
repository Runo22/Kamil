#define _RICHEDIT_VER 0x0800  // CHARFORMAT2::bUnderlineColor (Windows 8+ RichEdit)

#include "ui/spell_window.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <richedit.h>
#include <richole.h>
#include <tom.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "core/i18n.h"
#include "core/text.h"
#include "ui/fluent_geometry.h"

namespace kamil {

namespace {

constexpr wchar_t kSpellClass[] = L"Kamil.Spell";
constexpr UINT_PTR kTimerCheck = 1, kTimerFlash = 2;
constexpr UINT kCheckDelayMs = 250;
constexpr wchar_t kSpellGlyph = 0xE8D2;  // text_grammar_checkmark (src/ui/fluent_icons.h)
// RichEdit underline colours (Word's colour index): 6 red, 2 blue.
constexpr BYTE kUnderlineRed = 6, kUnderlineBlue = 2;
// IID_ITextDocument, here so that no import library has to provide it.
const IID kIIDTextDocument = {0x8CC497C0, 0xA1DF, 0x11CE, {0x80, 0x98, 0x00, 0xAA, 0x00, 0x47, 0xBE, 0x5D}};

D2D1_COLOR_F rgb(uint32_t v, float a = 1.f) {
    return D2D1::ColorF(((v >> 16) & 0xFF) / 255.f, ((v >> 8) & 0xFF) / 255.f, (v & 0xFF) / 255.f, a);
}
COLORREF colorref(uint32_t v) { return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF); }

struct Palette {
    uint32_t bar, edit, text, secondary, line;
};
Palette palette(bool dark) {
    return dark ? Palette{0x25282D, 0x1E2125, 0xE6E8EB, 0x9AA1AA, 0x34383E} : Palette{0xEFEDEA, 0xFAF9F7, 0x1D1F22, 0x6B7078, 0xDEDBD6};
}

constexpr uint32_t kGreen = 0x2E9A57, kRed = 0xD13438;

bool key_down(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

}  // namespace

SpellWindow::~SpellWindow() { destroy(); }

bool SpellWindow::create(HINSTANCE instance, HICON icon, Callbacks callbacks) {
    instance_ = instance;
    cb_ = std::move(callbacks);
    if (!LoadLibraryW(L"Msftedit.dll")) return false;  // RICHEDIT50W
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &SpellWindow::wndproc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = icon;
    wc.hIconSm = icon;
    wc.lpszClassName = kSpellClass;
    RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(0, kSpellClass, loc(L"Spell check — Kamil", L"Yazım denetimi — Kamil"), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, 860, 560, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;
    dpi_ = static_cast<float>(GetDpiForWindow(hwnd_));

    edit_ = CreateWindowExW(0, MSFTEDIT_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL,
                            0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(1), instance, nullptr);
    if (!edit_) return false;
    SendMessageW(edit_, EM_SETTEXTMODE, TM_RICHTEXT | TM_MULTILEVELUNDO | TM_MULTICODEPAGE, 0);
    SendMessageW(edit_, EM_SETUNDOLIMIT, 200, 0);
    SendMessageW(edit_, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE);
    SendMessageW(edit_, EM_SETLANGOPTIONS, 0, 0);  // Kamil checks the spelling itself (all languages at once)
    SetWindowSubclass(edit_, &SpellWindow::edit_proc, 1, reinterpret_cast<DWORD_PTR>(this));
    IRichEditOle* ole = nullptr;
    if (SendMessageW(edit_, EM_GETOLEINTERFACE, 0, reinterpret_cast<LPARAM>(&ole)) && ole) {
        ole->QueryInterface(kIIDTextDocument, reinterpret_cast<void**>(&doc_));
        ole->Release();
    }
    if (const FluentIcon* fi = find_fluent_icon(kSpellGlyph)) icon_ = build_fluent_geometry(factory_.Get(), *fi);
    create_formats();
    apply_colors();
    layout();  // no WM_SIZE comes when the size does not change: place the editor now
    return true;
}

void SpellWindow::destroy() {
    if (!hwnd_) return;
    if (edit_) RemoveWindowSubclass(edit_, &SpellWindow::edit_proc, 1);
    if (doc_) doc_->Release();
    doc_ = nullptr;
    DestroyWindow(hwnd_);
    hwnd_ = edit_ = nullptr;
}

LRESULT CALLBACK SpellWindow::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        static_cast<SpellWindow*>(cs->lpCreateParams)->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<SpellWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK SpellWindow::edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    return reinterpret_cast<SpellWindow*>(ref)->on_edit(h, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------
// Opening

void SpellWindow::open(const std::wstring& t, SpellSession* session, std::vector<SpellLanguageChip> chips, bool paste_back) {
    session_ = session;
    chips_ = std::move(chips);
    paste_back_ = paste_back;
    flash_.clear();
    // Centred on the monitor with the mouse, at the default size (DIPs).
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
    if (!visible()) {
        dpi_ = static_cast<float>(GetDpiForWindow(hwnd_));
        const RECT& w = mi.rcWork;
        const int cw = static_cast<int>(px(860)), ch = static_cast<int>(px(560));
        SetWindowPos(hwnd_, nullptr, w.left + (w.right - w.left - cw) / 2, w.top + (w.bottom - w.top - ch) / 2, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
    }
    set_text(t);
    check_now();
    SendMessageW(edit_, EM_SETSEL, 0, 0);
    SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    SetFocus(edit_);
    render();
}

void SpellWindow::set_chips(std::vector<SpellLanguageChip> chips) {
    chips_ = std::move(chips);
    check_now();
    render();
}

void SpellWindow::hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void SpellWindow::set_theme(bool dark, uint32_t accent_rgb) {
    dark_ = dark;
    accent_ = accent_rgb;
    if (!hwnd_) return;
    const BOOL on = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &on, sizeof(on));
    apply_colors();
    if (visible()) render();
}

void SpellWindow::apply_colors() {
    if (!edit_) return;
    const Palette p = palette(dark_);
    SendMessageW(edit_, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(colorref(p.edit)));
    apply_base_format();
}

// ---------------------------------------------------------------------------------------------
// Text and checking

std::wstring SpellWindow::text(bool crlf) const {
    GETTEXTLENGTHEX gl{GTL_NUMCHARS | GTL_PRECISE | (crlf ? GTL_USECRLF : 0u), 1200};
    const LRESULT n = SendMessageW(edit_, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&gl), 0);
    if (n <= 0) return {};
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GETTEXTEX gt{};
    gt.cb = static_cast<DWORD>((s.size()) * sizeof(wchar_t));
    gt.flags = crlf ? GT_USECRLF : GT_DEFAULT;  // GT_DEFAULT: '\r' line ends, one per character position
    gt.codepage = 1200;
    const LRESULT got = SendMessageW(edit_, EM_GETTEXTEX, reinterpret_cast<WPARAM>(&gt), reinterpret_cast<LPARAM>(s.data()));
    s.resize(static_cast<size_t>(std::max<LRESULT>(0, got)));
    return s;
}

void SpellWindow::set_text(const std::wstring& t) {
    marking_ = true;
    SETTEXTEX st{ST_DEFAULT, 1200};  // also empties the undo history
    SendMessageW(edit_, EM_SETTEXTEX, reinterpret_cast<WPARAM>(&st), reinterpret_cast<LPARAM>(t.c_str()));
    apply_base_format();
    SendMessageW(edit_, EM_EMPTYUNDOBUFFER, 0, 0);
    marking_ = false;
}

void SpellWindow::apply_base_format() {
    const Palette p = palette(dark_);
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_CHARSET;
    cf.yHeight = 230;  // 11.5 pt (twips)
    cf.crTextColor = colorref(p.text);
    cf.bCharSet = DEFAULT_CHARSET;
    wcscpy_s(cf.szFaceName, L"Segoe UI");
    const bool was = marking_;
    marking_ = true;
    if (doc_) doc_->Undo(tomSuspend, nullptr);
    SendMessageW(edit_, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&cf));
    SendMessageW(edit_, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&cf));
    PARAFORMAT2 pf{};
    pf.cbSize = sizeof(pf);
    pf.dwMask = PFM_LINESPACING | PFM_SPACEAFTER;
    pf.bLineSpacingRule = 5;   // dyLineSpacing in 20ths of a line
    pf.dyLineSpacing = 25;     // 1.25 lines
    pf.dySpaceAfter = 80;      // 4 pt between paragraphs
    CHARRANGE sel{};
    SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&sel));
    SendMessageW(edit_, EM_SETSEL, 0, -1);
    SendMessageW(edit_, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&pf));
    SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&sel));
    if (doc_) doc_->Undo(tomResume, nullptr);
    marking_ = was;
}

void SpellWindow::schedule_check() {
    check_pending_ = true;
    SetTimer(hwnd_, kTimerCheck, kCheckDelayMs, nullptr);
}

void SpellWindow::sync() {
    if (check_pending_) check_now();
}

void SpellWindow::check_now() {
    KillTimer(hwnd_, kTimerCheck);
    check_pending_ = false;
    text_ = text(false);
    first_cache_.clear();
    issues_ = session_ && !session_->empty() ? session_->check(text_) : std::vector<SpellIssue>{};
    mark();
    render();
}

void SpellWindow::mark() {
    if (!edit_) return;
    marking_ = true;
    SendMessageW(edit_, WM_SETREDRAW, FALSE, 0);
    if (doc_) doc_->Undo(tomSuspend, nullptr);  // the underlines are not edits: Ctrl+Z must not undo them
    CHARRANGE sel{};
    SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&sel));
    POINT scroll{};
    SendMessageW(edit_, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));

    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_UNDERLINE | CFM_UNDERLINETYPE;
    cf.dwEffects = 0;
    cf.bUnderlineType = CFU_UNDERLINENONE;
    SendMessageW(edit_, EM_SETSEL, 0, -1);
    SendMessageW(edit_, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
    for (const auto& issue : issues_) {
        CHARRANGE r{static_cast<LONG>(issue.start), static_cast<LONG>(issue.start + issue.length)};
        SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&r));
        cf.dwEffects = CFE_UNDERLINE;
        cf.bUnderlineType = CFU_UNDERLINEWAVE;
        cf.bUnderlineColor = issue.kind == SpellIssue::Kind::Repeated ? kUnderlineBlue : kUnderlineRed;
        SendMessageW(edit_, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
    }
    SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&sel));
    // Typing right after a marked word must not continue its underline.
    cf.dwEffects = 0;
    cf.bUnderlineType = CFU_UNDERLINENONE;
    if (sel.cpMin == sel.cpMax) SendMessageW(edit_, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
    SendMessageW(edit_, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
    if (doc_) doc_->Undo(tomResume, nullptr);
    SendMessageW(edit_, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(edit_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    marking_ = false;
}

void SpellWindow::selection(long* from, long* to) const {
    CHARRANGE sel{};
    SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&sel));
    *from = sel.cpMin;
    *to = sel.cpMax;
}

int SpellWindow::issue_at(long pos) const {
    for (size_t i = 0; i < issues_.size(); ++i) {
        const auto& is = issues_[i];
        if (pos >= static_cast<long>(is.start) && pos <= static_cast<long>(is.start + is.length)) return static_cast<int>(i);
    }
    return -1;
}

std::wstring SpellWindow::first_suggestion(size_t issue) {
    const SpellIssue& is = issues_[issue];
    for (const auto& [start, s] : first_cache_)
        if (start == is.start) return s;
    std::wstring s;
    if (session_) {
        const auto list = session_->suggestions(text_, is, 1);
        if (!list.empty()) s = list.front();
    }
    first_cache_.emplace_back(is.start, s);
    return s;
}

// ---------------------------------------------------------------------------------------------
// Actions

void SpellWindow::run(Action a) {
    switch (a) {
        case kNext: jump(key_down(VK_SHIFT)); break;
        case kApply: apply_at_caret(); break;
        case kMenu: context_menu(-1); break;
        case kFixAll: fix_all(); break;
        case kDone: finish(paste_back_); break;
        case kClose: hide(); break;
    }
    SetFocus(edit_);
}

void SpellWindow::jump(bool backwards) {
    sync();
    if (issues_.empty()) {
        flash(loc(L"No mistakes", L"Hata yok"));
        return;
    }
    long from = 0, to = 0;
    selection(&from, &to);
    const SpellIssue* target = nullptr;
    if (!backwards) {
        // The next mistake after the caret; when a mistake is selected (F8 again), the one after it.
        const bool on_issue = to > from;
        for (const auto& is : issues_) {
            const long s = static_cast<long>(is.start);
            if (on_issue ? s > from : s >= from) {
                target = &is;
                break;
            }
        }
        if (!target) target = &issues_.front();  // wrap around
    } else {
        for (auto it = issues_.rbegin(); it != issues_.rend(); ++it)
            if (static_cast<long>(it->start) < from) {
                target = &*it;
                break;
            }
        if (!target) target = &issues_.back();
    }
    CHARRANGE r{static_cast<LONG>(target->start), static_cast<LONG>(target->start + target->length)};
    SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&r));
    SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
}

bool SpellWindow::apply_at_caret() {
    sync();
    long from = 0, to = 0;
    selection(&from, &to);
    const int i = issue_at(from);
    if (i < 0) return false;
    const SpellIssue& is = issues_[static_cast<size_t>(i)];
    if (is.kind == SpellIssue::Kind::Repeated) {
        apply(static_cast<size_t>(i), L"");
        return true;
    }
    const std::wstring s = first_suggestion(static_cast<size_t>(i));
    if (s.empty()) flash(loc(L"No suggestion for this word", L"Bu kelime için öneri yok"));
    else apply(static_cast<size_t>(i), s);
    return true;
}

void SpellWindow::apply(size_t issue, const std::wstring& replacement) {
    if (issue >= issues_.size()) return;
    const SpellEdit e = spell_edit(text_, issues_[issue], replacement);
    CHARRANGE r{static_cast<LONG>(e.start), static_cast<LONG>(e.start + e.length)};
    SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&r));
    SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(e.text.c_str()));  // can be undone
    check_now();
}

void SpellWindow::replace_all(const std::wstring& new_text) {
    CHARRANGE sel{};
    SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&sel));
    POINT scroll{};
    SendMessageW(edit_, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
    SendMessageW(edit_, EM_SETSEL, 0, -1);
    SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(new_text.c_str()));  // one undo step
    const LONG n = static_cast<LONG>(new_text.size());
    sel.cpMin = std::min(sel.cpMin, n);
    sel.cpMax = std::min(sel.cpMax, n);
    SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&sel));
    SendMessageW(edit_, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
    check_now();
}

void SpellWindow::fix_all() {
    sync();
    if (!session_ || issues_.empty()) {
        flash(loc(L"No mistakes", L"Hata yok"));
        return;
    }
    size_t fixed = 0;
    const std::wstring fixed_text = session_->fix_all(text_, &fixed);
    if (!fixed) {
        flash(loc(L"No suggestions to apply", L"Uygulanacak öneri yok"));
        return;
    }
    replace_all(fixed_text);
    flash(fmt(loc(L"{} fixed", L"{} düzeltildi"), std::to_wstring(fixed)));
}

void SpellWindow::finish(bool paste_back) {
    if (cb_.done) cb_.done(text(true), paste_back);
}

void SpellWindow::paste_plain() {
    SendMessageW(edit_, EM_PASTESPECIAL, CF_UNICODETEXT, 0);  // never the source's fonts and colours
}

void SpellWindow::context_menu(LPARAM screen_point) {
    sync();
    POINT at{};
    long pos = 0;
    if (screen_point == -1) {  // keyboard (menu key, Shift+F10, Ctrl+.)
        long to = 0;
        selection(&pos, &to);
        POINTL p{};
        SendMessageW(edit_, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&p), pos);
        at = {p.x, p.y + static_cast<LONG>(px(24))};
        ClientToScreen(edit_, &at);
    } else {
        at = {GET_X_LPARAM(screen_point), GET_Y_LPARAM(screen_point)};
        POINT c = at;
        ScreenToClient(edit_, &c);
        POINTL pl{c.x, c.y};
        pos = static_cast<long>(SendMessageW(edit_, EM_CHARFROMPOS, 0, reinterpret_cast<LPARAM>(&pl)));
    }
    const int i = issue_at(pos);
    if (i >= 0) {
        const SpellIssue& is = issues_[static_cast<size_t>(i)];
        CHARRANGE r{static_cast<LONG>(is.start), static_cast<LONG>(is.start + is.length)};
        SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&r));  // the word the menu is about
        issue_menu(static_cast<size_t>(i), at);
    } else {
        edit_menu(at);
    }
}

void SpellWindow::issue_menu(size_t index, POINT at) {
    const SpellIssue issue = issues_[index];
    const std::wstring word = text_.substr(issue.start, issue.length);
    const bool repeated = issue.kind == SpellIssue::Kind::Repeated;
    const std::vector<std::wstring> suggestions = session_ && !repeated ? session_->suggestions(text_, issue, 6) : std::vector<std::wstring>{};
    enum : UINT { kSuggestion = 100, kEverywhere = 200, kAdd = 201, kIgnore = 202 };
    HMENU m = CreatePopupMenu();
    if (repeated) {
        AppendMenuW(m, MF_STRING, kSuggestion, loc(L"Delete the repeated word", L"Tekrarlanan kelimeyi sil"));
    } else if (suggestions.empty()) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 1, loc(L"No suggestions", L"Öneri yok"));
    } else {
        for (size_t k = 0; k < suggestions.size(); ++k) AppendMenuW(m, MF_STRING, kSuggestion + static_cast<UINT>(k), suggestions[k].c_str());
    }
    if (repeated || !suggestions.empty()) SetMenuDefaultItem(m, kSuggestion, FALSE);
    if (!repeated) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        size_t same = 0;
        for (const auto& is : issues_) same += text_.compare(is.start, is.length, word) == 0;
        if (!suggestions.empty() && same > 1)
            AppendMenuW(m, MF_STRING, kEverywhere, fmt(loc(L"Fix everywhere ({})", L"Her yerde düzelt ({})"), std::to_wstring(same)).c_str());
        std::wstring dict;
        for (const auto& c : chips_)
            if (c.active && c.installed) {
                dict = c.name;
                break;
            }
        AppendMenuW(m, MF_STRING, kAdd,
                    (dict.empty() ? std::wstring(loc(L"Add to dictionary", L"Sözlüğe ekle")) : fmt(loc(L"Add to dictionary ({})", L"Sözlüğe ekle ({})"), dict)).c_str());
        AppendMenuW(m, MF_STRING, kIgnore, loc(L"Ignore", L"Yoksay"));
    }
    const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, at.x, at.y, hwnd_, nullptr));
    DestroyMenu(m);
    if (cmd >= kSuggestion && cmd < kSuggestion + 10) {
        apply(index, repeated ? std::wstring() : suggestions[cmd - kSuggestion]);
    } else if (cmd == kEverywhere && !suggestions.empty()) {
        std::vector<SpellEdit> edits;
        for (const auto& is : issues_)
            if (text_.compare(is.start, is.length, word) == 0) edits.push_back(spell_edit(text_, is, suggestions.front()));
        replace_all(apply_spell_edits(text_, edits));
        flash(fmt(loc(L"{} fixed", L"{} düzeltildi"), std::to_wstring(edits.size())));
    } else if (cmd == kAdd && session_) {
        session_->add_to_dictionary(word);
        check_now();
        flash(fmt(loc(L"“{}” added to the dictionary", L"“{}” sözlüğe eklendi"), word));
    } else if (cmd == kIgnore && session_) {
        session_->ignore(word);
        check_now();
    }
}

void SpellWindow::edit_menu(POINT at) {
    enum : UINT { kUndo = 1, kCut, kCopy, kPaste, kAll };
    long from = 0, to = 0;
    selection(&from, &to);
    const bool has_sel = to > from;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (SendMessageW(edit_, EM_CANUNDO, 0, 0) ? 0u : MF_GRAYED), kUndo, loc(L"Undo\tCtrl+Z", L"Geri al\tCtrl+Z"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (has_sel ? 0u : MF_GRAYED), kCut, loc(L"Cut\tCtrl+X", L"Kes\tCtrl+X"));
    AppendMenuW(m, MF_STRING | (has_sel ? 0u : MF_GRAYED), kCopy, loc(L"Copy\tCtrl+C", L"Kopyala\tCtrl+C"));
    AppendMenuW(m, MF_STRING, kPaste, loc(L"Paste\tCtrl+V", L"Yapıştır\tCtrl+V"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kAll, loc(L"Select all\tCtrl+A", L"Tümünü seç\tCtrl+A"));
    const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, at.x, at.y, hwnd_, nullptr));
    DestroyMenu(m);
    switch (cmd) {
        case kUndo: SendMessageW(edit_, EM_UNDO, 0, 0); break;
        case kCut: SendMessageW(edit_, WM_CUT, 0, 0); break;
        case kCopy: SendMessageW(edit_, WM_COPY, 0, 0); break;
        case kPaste: paste_plain(); break;
        case kAll: SendMessageW(edit_, EM_SETSEL, 0, -1); break;
        default: break;
    }
}

void SpellWindow::toggle_chip(size_t index) {
    if (index >= chips_.size()) return;
    SpellLanguageChip& c = chips_[index];
    if (!c.installed) {
        if (cb_.install_languages) cb_.install_languages();
        return;
    }
    size_t active = 0;
    for (const auto& x : chips_) active += x.active && x.installed;
    if (c.active && active <= 1) {
        flash(loc(L"At least one language stays on", L"En az bir dil açık kalır"));
        return;
    }
    c.active = !c.active;
    std::vector<std::wstring> tags;
    for (const auto& x : chips_)
        if (x.active && x.installed) tags.push_back(x.tag);
    if (cb_.languages_changed) cb_.languages_changed(tags);  // rebuilds the session, then set_chips()
}

void SpellWindow::flash(std::wstring message) {
    flash_ = std::move(message);
    SetTimer(hwnd_, kTimerFlash, 2500, nullptr);
    render();
}

// ---------------------------------------------------------------------------------------------
// Messages

LRESULT SpellWindow::on_edit(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_KEYDOWN: {
            swallow_char_ = false;
            const bool ctrl = key_down(VK_CONTROL) && !key_down(VK_MENU), shift = key_down(VK_SHIFT);
            if (wp == VK_ESCAPE) {
                swallow_char_ = true;
                hide();
                return 0;
            }
            if (wp == VK_RETURN && ctrl) {
                swallow_char_ = true;
                finish(!shift && paste_back_);
                return 0;
            }
            if (wp == VK_F8) {
                jump(shift);
                return 0;
            }
            if (wp == VK_TAB && !ctrl && !shift && apply_at_caret()) {
                swallow_char_ = true;
                return 0;
            }
            if ((wp == VK_OEM_PERIOD && ctrl) || (wp == VK_F10 && shift) || wp == VK_APPS) {
                context_menu(-1);
                return 0;
            }
            if (wp == 'F' && ctrl && shift) {
                swallow_char_ = true;
                fix_all();
                return 0;
            }
            if ((wp == 'V' && ctrl) || (wp == VK_INSERT && shift && !ctrl)) {
                swallow_char_ = wp == 'V';
                paste_plain();
                return 0;
            }
            break;
        }
        case WM_CHAR:
            if (swallow_char_) {
                swallow_char_ = false;
                return 0;
            }
            break;
        case WM_PASTE:
            paste_plain();
            return 0;
        case WM_CONTEXTMENU:
            context_menu(lp);
            return 0;
        default:
            break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT SpellWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            layout();
            if (target_) target_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            render();
            return 0;
        case WM_DPICHANGED: {
            dpi_ = static_cast<float>(HIWORD(wp));
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (target_) target_->SetDpi(dpi_, dpi_);
            layout();
            render();
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            render();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE) SetFocus(edit_);
            return 0;
        case WM_CLOSE:
            hide();
            return 0;
        case WM_TIMER:
            if (wp == kTimerCheck) check_now();
            else if (wp == kTimerFlash) {
                KillTimer(hwnd_, kTimerFlash);
                flash_.clear();
                render();
            }
            return 0;
        case WM_COMMAND:
            if (reinterpret_cast<HWND>(lp) == edit_ && HIWORD(wp) == EN_CHANGE && !marking_) schedule_check();
            return 0;
        case WM_NOTIFY: {
            const auto* nh = reinterpret_cast<const NMHDR*>(lp);
            if (nh->hwndFrom == edit_ && nh->code == EN_SELCHANGE && !marking_) render();  // the Tab hint follows the caret
            return 0;
        }
        case WM_MOUSEMOVE: {
            const float x = GET_X_LPARAM(lp) * 96.f / dpi_, y = GET_Y_LPARAM(lp) * 96.f / dpi_;
            int over = -1;
            for (size_t i = 0; i < hits_.size(); ++i) {
                const auto& r = hits_[i].rect;
                if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) over = static_cast<int>(i);
            }
            if (over != hover_) {
                hover_ = over;
                render();
            }
            if (over >= 0) SetCursor(LoadCursorW(nullptr, IDC_HAND));
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (hover_ != -1) {
                hover_ = -1;
                render();
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && hover_ >= 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_LBUTTONUP: {
            const float x = GET_X_LPARAM(lp) * 96.f / dpi_, y = GET_Y_LPARAM(lp) * 96.f / dpi_;
            for (const auto& h : hits_) {
                if (x < h.rect.left || x >= h.rect.right || y < h.rect.top || y >= h.rect.bottom) continue;
                if (h.kind == kHitChip) toggle_chip(h.index);
                else run(static_cast<Action>(h.index));
                break;
            }
            SetFocus(edit_);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------
// Drawing: the header (icon, title, language chips, mistakes) and the key hints below the text

void SpellWindow::create_formats() {
    auto make = [&](float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>* out) {
        out->Reset();
        dwrite_->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", out->GetAddressOf());
        if (*out) {
            (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    };
    make(15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmt_title_);
    make(12.5f, DWRITE_FONT_WEIGHT_NORMAL, &fmt_chip_);
    make(11.f, DWRITE_FONT_WEIGHT_NORMAL, &fmt_key_);
    make(12.5f, DWRITE_FONT_WEIGHT_NORMAL, &fmt_hint_);
}

void SpellWindow::layout() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    width_ = (rc.right - rc.left) * 96.f / dpi_;
    height_ = (rc.bottom - rc.top) * 96.f / dpi_;
    const int top = static_cast<int>(std::lround(px(top_h_)));
    const int bottom = static_cast<int>(std::lround(px(bottom_h_)));
    const int w = rc.right - rc.left, h = std::max(0, static_cast<int>(rc.bottom - rc.top) - top - bottom);
    MoveWindow(edit_, 0, top, w, h, TRUE);
    // Text margins inside the control.
    RECT inner{static_cast<LONG>(px(24)), static_cast<LONG>(px(16)), static_cast<LONG>(w - px(16)), static_cast<LONG>(h - px(12))};
    SendMessageW(edit_, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&inner));
}

bool SpellWindow::ensure_target() {
    if (target_) return true;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.dpiX = props.dpiY = dpi_;
    if (FAILED(factory_->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top)),
                                                target_.GetAddressOf())))
        return false;
    target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), brush_.GetAddressOf());
    return brush_ != nullptr;
}

float SpellWindow::text_width(const std::wstring& s, IDWriteTextFormat* f) const {
    ComPtr<IDWriteTextLayout> layout;
    if (!f || FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, 4000.f, 100.f, &layout))) return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

void SpellWindow::render() {
    if (!hwnd_ || !IsWindowVisible(hwnd_) || !ensure_target()) return;
    const Palette p = palette(dark_);
    ID2D1HwndRenderTarget* t = target_.Get();
    ID2D1SolidColorBrush* b = brush_.Get();
    hits_.clear();
    t->BeginDraw();
    t->Clear(rgb(p.bar));
    auto text_at = [&](const std::wstring& s, IDWriteTextFormat* f, float x, float y0, float y1, uint32_t color, float alpha = 1.f) {
        b->SetColor(rgb(color, alpha));
        t->DrawText(s.c_str(), static_cast<UINT32>(s.size()), f, D2D1::RectF(x, y0, x + 2000.f, y1), b);
    };
    auto pill = [&](float x, float y0, float w, float h, uint32_t color, float alpha) {
        b->SetColor(rgb(color, alpha));
        t->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y0, x + w, y0 + h), h / 2.f, h / 2.f), b);
    };

    // Header: icon tile + title on the left; language chips and the number of mistakes on the right.
    const float cy = top_h_ / 2.f;
    b->SetColor(rgb(kGreen, dark_ ? 0.22f : 0.13f));
    t->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(18, cy - 15, 48, cy + 15), 7, 7), b);
    if (icon_) {
        D2D1_MATRIX_3X2_F old;
        t->GetTransform(&old);
        t->SetTransform(D2D1::Matrix3x2F::Scale(19.f / 24.f, 19.f / 24.f) * D2D1::Matrix3x2F::Translation(23.5f, cy - 9.5f) * old);
        b->SetColor(rgb(kGreen));
        t->FillGeometry(icon_.Get(), b);
        t->SetTransform(old);
    }
    text_at(loc(L"Spell check", L"Yazım denetimi"), fmt_title_.Get(), 60, 0, top_h_, p.text);

    float x = width_ - 18.f;
    std::wstring count;
    uint32_t count_color = kGreen;
    if (!session_ || session_->empty()) {
        count = loc(L"No language", L"Dil yok");
        count_color = p.secondary;
    } else if (issues_.empty()) {
        count = loc(L"No mistakes", L"Hata yok");
    } else {
        count = fmt(loc(L"{} mistakes", L"{} hata"), std::to_wstring(issues_.size()));
        count_color = kRed;
    }
    const float chip_h = 26.f;
    {
        const float w = text_width(count, fmt_chip_.Get()) + 24.f;
        x -= w;
        pill(x, cy - chip_h / 2, w, chip_h, count_color, dark_ ? 0.24f : 0.13f);
        text_at(count, fmt_chip_.Get(), x + 12, cy - chip_h / 2, cy + chip_h / 2, count_color == p.secondary ? p.secondary : count_color);
        x -= 14.f;
    }
    for (size_t i = chips_.size(); i-- > 0;) {
        const auto& c = chips_[i];
        const std::wstring label = c.installed ? c.name : fmt(loc(L"{} (not installed)", L"{} (yüklü değil)"), c.name);
        const float w = text_width(label, fmt_chip_.Get()) + 24.f;
        x -= w;
        const bool hover = hover_ == static_cast<int>(hits_.size());
        const D2D1_RECT_F r = D2D1::RectF(x, cy - chip_h / 2, x + w, cy + chip_h / 2);
        if (c.active && c.installed) pill(x, r.top, w, chip_h, kGreen, (dark_ ? 0.26f : 0.15f) + (hover ? 0.08f : 0.f));
        else pill(x, r.top, w, chip_h, p.secondary, (dark_ ? 0.16f : 0.10f) + (hover ? 0.08f : 0.f));
        text_at(label, fmt_chip_.Get(), x + 12, r.top, r.bottom, c.active && c.installed ? (dark_ ? 0x7FD49B : 0x1F6E3E) : p.secondary);
        hits_.push_back({r, kHitChip, i});
        x -= 8.f;
    }
    b->SetColor(rgb(p.line));
    t->FillRectangle(D2D1::RectF(0, top_h_ - 1, width_, top_h_), b);

    // Footer: key hints (clickable), and a short message on the right.
    const float fy0 = height_ - bottom_h_, fcy = height_ - bottom_h_ / 2.f;
    b->SetColor(rgb(p.line));
    t->FillRectangle(D2D1::RectF(0, fy0, width_, fy0 + 1), b);
    long from = 0, to = 0;
    if (edit_) selection(&from, &to);
    const int at = check_pending_ ? -1 : issue_at(from);
    std::wstring tab_label;
    if (at >= 0) {
        const SpellIssue& is = issues_[static_cast<size_t>(at)];
        if (is.kind == SpellIssue::Kind::Repeated) tab_label = loc(L"Delete the repeat", L"Tekrarı sil");
        else if (const std::wstring s = first_suggestion(static_cast<size_t>(at)); !s.empty()) tab_label = L"→ " + s;
    }
    struct HintDef {
        const wchar_t* key;
        std::wstring label;
        Action action;
    };
    std::vector<HintDef> hints{{L"F8", loc(L"Next mistake", L"Sonraki hata"), kNext}};
    if (!tab_label.empty()) hints.push_back({L"Tab", tab_label, kApply});
    hints.push_back({L"Ctrl+.", loc(L"Suggestions", L"Öneriler"), kMenu});
    hints.push_back({L"Ctrl+Shift+F", loc(L"Fix all", L"Hepsini düzelt"), kFixAll});
    hints.push_back({L"Ctrl+Enter", paste_back_ ? loc(L"Copy and paste back", L"Kopyala ve geri yapıştır") : loc(L"Copy", L"Kopyala"), kDone});
    hints.push_back({L"Esc", loc(L"Close", L"Kapat"), kClose});
    float hx = 18.f;
    const float key_h = 20.f;
    for (const auto& h : hints) {
        const float kw = text_width(h.key, fmt_key_.Get()) + 12.f;
        const float lw = text_width(h.label, fmt_hint_.Get());
        if (hx + kw + 6 + lw > width_ - 160.f && h.action != kDone) continue;  // narrow window: drop what does not fit
        const D2D1_RECT_F r = D2D1::RectF(hx - 4, fcy - 14, hx + kw + 6 + lw + 4, fcy + 14);
        const bool hover = hover_ == static_cast<int>(hits_.size());
        if (hover) {
            b->SetColor(rgb(p.secondary, 0.12f));
            t->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), b);
        }
        b->SetColor(rgb(p.secondary, dark_ ? 0.22f : 0.12f));
        t->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(hx, fcy - key_h / 2, hx + kw, fcy + key_h / 2), 5, 5), b);
        text_at(h.key, fmt_key_.Get(), hx + 6, fcy - key_h / 2, fcy + key_h / 2, p.text, 0.85f);
        const bool apply_hint = h.action == kApply;
        text_at(h.label, fmt_hint_.Get(), hx + kw + 6, fy0, height_, apply_hint ? kGreen : p.secondary);
        hits_.push_back({r, kHitHint, h.action});
        hx += kw + 6 + lw + 18.f;
    }
    if (!flash_.empty()) {
        const float w = text_width(flash_, fmt_hint_.Get());
        text_at(flash_, fmt_hint_.Get(), width_ - 18.f - w, fy0, height_, p.text);
    }
    if (t->EndDraw() == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
        target_.Reset();
        brush_.Reset();
    }
}

}  // namespace kamil
