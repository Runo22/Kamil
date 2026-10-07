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
    std::string theme = "auto";   // auto | dark | light
    std::string accent = "auto";  // auto | #rrggbb
};

class LauncherWindow {
public:
    struct Callbacks {
        std::function<void(const std::wstring& query)> query_changed;
        std::function<void(const Item& item, LaunchMode mode)> activate;
        std::function<void(const Item& item, uint32_t size_px)> need_icon;
        std::function<void(const Item& item)> copy_item;
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

    static LRESULT CALLBACK wndproc(HWND, UINT, WPARAM, LPARAM);
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

    ComPtr<IDWriteTextFormat> fmt_input_, fmt_title_, fmt_subtitle_, fmt_hint_, fmt_letter_;
    ComPtr<IDWriteInlineObject> ellipsis_title_, ellipsis_subtitle_;

    LineEdit edit_;
    float input_scroll_ = 0.f;
    bool caret_on_ = true;

    std::vector<Hit> hits_;
    size_t selected_ = 0;
    size_t scroll_ = 0;

    // Layout (DIPs), computed by layout().
    float margin_ = 18.f, panel_w_ = 720.f, panel_h_ = 56.f, input_h_ = 56.f, row_h_ = 48.f, pad_ = 6.f;
    size_t visible_rows_ = 0;
    POINT origin_px_{};  // top-left of the window, physical pixels

    uint64_t hidden_at_ = 0;
    POINT last_mouse_{};
    bool hover_armed_ = false;

    std::unordered_map<std::wstring, Icon> icons_;
    std::unordered_set<std::wstring> icon_pending_;
};

}  // namespace kamil
