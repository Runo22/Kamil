#pragma once

// Kamil's spell check window: a text editor (the RichEdit control that ships with Windows) whose
// misspelled words get a red wavy underline (a repeated word a blue one). Right click or Ctrl+.
// lists suggestions; Tab applies the first one, F8 jumps to the next mistake, Ctrl+Shift+F fixes
// everything, Ctrl+Enter copies the text (and pastes it back into the program it came from).
// The header has a chip per language (click to switch it on / off) and the number of mistakes.

#include <d2d1.h>
#include <dwrite.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/spell.h"
#include "platform/win.h"

struct ITextDocument;

namespace kamil {

struct SpellLanguageChip {
    std::wstring tag;     // "tr-TR"
    std::wstring name;    // "Türkçe"
    bool installed = true;
    bool active = true;
};

class SpellWindow {
public:
    struct Callbacks {
        std::function<void(const std::wstring& text, bool paste_back)> done;  // Ctrl+Enter (paste back) / Ctrl+Shift+Enter
        std::function<void(const std::vector<std::wstring>& active_tags)> languages_changed;
        std::function<void()> install_languages;  // a language without a dictionary was clicked
    };

    ~SpellWindow();
    bool create(HINSTANCE instance, HICON icon, Callbacks callbacks);
    void destroy();

    // Shows the window with `text`. `session` belongs to the caller and must outlive the window.
    void open(const std::wstring& text, SpellSession* session, std::vector<SpellLanguageChip> chips, bool paste_back);
    // After the active languages changed (the session was rebuilt): new chips, check again.
    void set_chips(std::vector<SpellLanguageChip> chips);
    void hide();
    bool visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    HWND hwnd() const { return hwnd_; }
    void set_theme(bool dark, uint32_t accent_rgb);

private:
    struct Hit {
        D2D1_RECT_F rect;
        int kind;       // kHitChip, kHitHint
        size_t index;   // chip index, or the hint's action
    };
    enum { kHitChip = 1, kHitHint = 2 };
    enum Action : size_t { kNext, kApply, kMenu, kFixAll, kDone, kClose };

    static LRESULT CALLBACK wndproc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK edit_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT on_edit(HWND h, UINT msg, WPARAM wp, LPARAM lp);

    // Text and checking
    std::wstring text(bool crlf) const;
    void set_text(const std::wstring& t);
    void apply_base_format();
    void schedule_check();
    void check_now();
    void sync();  // a pending check runs now (before acting on the issues)
    void mark();  // underline issues_, keeping the selection, scroll position and undo history
    int issue_at(long pos) const;
    void selection(long* from, long* to) const;
    std::wstring first_suggestion(size_t issue);

    // Actions
    void run(Action a);
    void jump(bool backwards);
    bool apply_at_caret();
    void apply(size_t issue, const std::wstring& replacement);
    void replace_all(const std::wstring& new_text);
    void fix_all();
    void finish(bool paste_back);
    void context_menu(LPARAM screen_point);
    void issue_menu(size_t issue, POINT at);
    void edit_menu(POINT at);
    void paste_plain();
    void toggle_chip(size_t index);
    void flash(std::wstring message);

    // Drawing
    bool ensure_target();
    void create_formats();
    void layout();
    void render();
    void apply_colors();
    float px(float dip) const { return dip * dpi_ / 96.f; }
    float text_width(const std::wstring& s, IDWriteTextFormat* f) const;

    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    HINSTANCE instance_ = nullptr;
    Callbacks cb_;
    SpellSession* session_ = nullptr;
    std::vector<SpellLanguageChip> chips_;
    bool paste_back_ = true;

    std::wstring text_;               // what issues_ refer to ('\r' line ends, as the control counts)
    std::vector<SpellIssue> issues_;
    std::vector<std::pair<size_t, std::wstring>> first_cache_;  // issue start -> first suggestion
    bool check_pending_ = false;
    bool marking_ = false;            // our own formatting: no EN_CHANGE / EN_SELCHANGE handling
    bool swallow_char_ = false;       // the WM_CHAR of a key we handled (Tab, Ctrl+Enter, Esc ...)
    std::wstring flash_;
    ITextDocument* doc_ = nullptr;  // TOM: suspends undo while the underlines are set (released in destroy)

    float dpi_ = 96.f;
    bool dark_ = false;
    uint32_t accent_ = 0x0078D4;
    float width_ = 860.f, height_ = 560.f;  // DIPs
    const float top_h_ = 54.f, bottom_h_ = 46.f;

    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTextFormat> fmt_title_, fmt_chip_, fmt_key_, fmt_hint_;
    ComPtr<ID2D1PathGeometry> icon_;
    std::vector<Hit> hits_;
    int hover_ = -1;
};

}  // namespace kamil
