#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/line_edit.h"
#include "core/search.h"
#include "platform/icon_loader.h"
#include "platform/shell.h"
#include "ui/renderer.h"

namespace kamil {

struct LauncherStyle {
    int width = 720;          // DIPs
    int max_rows = 8;
    int font_size = 14;       // DIPs, result titles
    int position = 22;        // top edge, percent of the work area height
    bool animations = true;
    bool remember_query = false;
    bool hide_on_focus_loss = true;
    bool footer = true;
    std::string theme = "auto";   // auto | dark | light
    std::string accent = "auto";  // auto | #rrggbb
};

// A key hint in the footer, drawn as a keycap chip plus a label ("[Alt+B] Build"). Clicking it
// does what the key does.
struct FooterHint {
    enum class Do { None, Activate, Actions, Back, Complete, Quick };
    std::wstring key;    // "Alt+B", "Enter", "Ctrl+K"
    std::wstring label;  // "Build"
    Do action = Do::None;
    wchar_t letter = 0;  // Do::Quick: the Alt+letter passed to quick_action
};

struct Footer {
    std::wstring left;               // context: git branch, changes, preset, running job ...
    std::vector<FooterHint> hints;   // most important first; trailing ones are dropped when space runs out
};

class LauncherWindow {
public:
    struct Callbacks {
        std::function<void(const std::wstring& query)> query_changed;
        std::function<void(const Item& item, LaunchMode mode)> activate;
        std::function<void(const Item& item, uint32_t size_px)> need_icon;
        std::function<void(const Item& item)> copy_item;
        // Action panel (Ctrl+K): the actions of an item, and running one of them.
        std::function<std::vector<Item>(const Item& item)> actions_for;
        std::function<void(const Item& item, const Item& action)> run_action;
        // Footer text for the selected item (nullptr when nothing is selected).
        std::function<Footer(const Item* selected, const Item* action_parent)> footer;
        // Alt+letter on a result (D = debug, B = build, R = run). Returns true if handled.
        std::function<bool(const Item& item, wchar_t key)> quick_action;
    };

    bool create(HINSTANCE instance, Callbacks callbacks);
    void destroy();

    void apply_style(const LauncherStyle& style);
    void set_results(std::vector<Hit> hits);
    void on_icon(const IconResult& icon);
    void on_system_theme_changed();
    void clear_icons();

    void show();
    void hide();
    void toggle() { visible() ? hide() : show(); }
    bool visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    HWND hwnd() const { return hwnd_; }
    // GetTickCount64() of the last hide, used to ignore the tray click that caused the hide.
    uint64_t hidden_at() const { return hidden_at_; }
    const std::wstring& query() const { return edit_.text(); }
    bool in_action_panel() const { return mode_ != Mode::Results; }
    // The item whose action panel / pick list is open, or nullptr.
    const Item* action_parent() const { return mode_ == Mode::Actions && visible() ? &action_parent_ : nullptr; }

    // Shows `items` as a pickable list for `parent` (an action panel with custom content).
    void show_list(const Item& parent, std::vector<Item> items, std::wstring placeholder);
    // Re-opens the action panel of `parent` (e.g. after a choice was made in a list).
    void reopen_actions(const Item& parent);
    // Lets the user edit one line of text; Enter calls `done`, Esc returns to the results.
    void prompt(const Item& parent, std::wstring placeholder, std::wstring initial, std::wstring help,
                std::function<void(const std::wstring&)> done);
    void refresh_footer();  // re-query the footer (e.g. git status arrived)

private:
    struct Palette {
        D2D1_COLOR_F surface, border, text, text_secondary, divider, selection, accent_text, caret, text_selection;
        float shadow_alpha;
    };
    struct Icon {
        ComPtr<ID2D1Bitmap1> bitmap;
        uint64_t generation = 0;
        uint32_t size = 0;
        std::vector<uint32_t> pixels;
        bool failed = false;
    };

    enum class Mode { Results, Actions, Input };

    static LRESULT CALLBACK wndproc(HWND, UINT, WPARAM, LPARAM);
    void open_actions();
    void close_actions();
    void filter_actions();
    void complete();
    void draw_footer(ID2D1DeviceContext* dc);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    bool on_key(WPARAM vk, bool alt);
    void on_char(wchar_t ch);
    void text_changed();
    void move_selection(int delta);
    void ensure_visible();
    void activate(size_t index, LaunchMode mode);
    LaunchMode mode_from_keyboard() const;

    void update_dpi(float dpi);
    void create_text_formats();
    void update_palette();
    void layout();
    void place_window();
    void render();
    void draw_row(ID2D1DeviceContext* dc, size_t index, float y);
    ID2D1Bitmap1* icon_for(const Item& item);
    int row_at(int x_px, int y_px) const;
    int hint_at(int x_px, int y_px) const;
    void run_hint(const FooterHint& hint);
    void restart_caret();

    float px(float dip) const { return dip * dpi_ / 96.f; }
    float s(float v) const { return v * static_cast<float>(style_.font_size) / 14.f; }

    HWND hwnd_ = nullptr;
    Callbacks cb_;
    LauncherStyle style_;
    Renderer renderer_;
    float dpi_ = 96.f;
    Palette pal_{};
    bool dark_ = true;

    ComPtr<IDWriteTextFormat> fmt_input_, fmt_title_, fmt_subtitle_, fmt_hint_, fmt_letter_, fmt_glyph_, fmt_footer_, fmt_key_;
    ComPtr<IDWriteInlineObject> ellipsis_title_, ellipsis_subtitle_;

    LineEdit edit_;
    float input_scroll_ = 0.f;
    bool caret_on_ = true;

    Mode mode_ = Mode::Results;
    Item action_parent_;               // item whose actions are shown
    std::vector<Item> action_items_;   // owned here; hits_ point into it while in Mode::Actions
    std::wstring placeholder_;         // search box hint in Actions / Input mode
    std::function<void(const std::wstring&)> input_done_;
    std::wstring saved_query_;         // search text restored when leaving the action panel
    size_t saved_selected_ = 0;
    Footer footer_;

    std::vector<Hit> hits_;
    size_t selected_ = 0;
    size_t scroll_ = 0;

    // Layout (DIPs), computed by layout().
    float margin_ = 18.f, panel_w_ = 720.f, panel_h_ = 56.f, input_h_ = 56.f, row_h_ = 48.f, pad_ = 6.f, footer_h_ = 0.f;
    size_t visible_rows_ = 0;
    POINT origin_px_{};  // top-left of the window, physical pixels

    uint64_t hidden_at_ = 0;
    POINT last_mouse_{};
    std::vector<D2D1_RECT_F> hint_rects_;  // DIPs, parallel to the drawn footer_.hints
    int hover_hint_ = -1;
    bool hover_armed_ = false;

    std::unordered_map<std::wstring, Icon> icons_;
    std::unordered_set<std::wstring> icon_pending_;
};

}  // namespace kamil
