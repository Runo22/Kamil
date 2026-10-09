#pragma once

// Kamil Console: program output in tabs (runs, Visual Studio builds, custom commands), with
// errors and warnings coloured and one click / F8 away from their source line.

#include <d2d1.h>
#include <dwrite.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/console_buffer.h"
#include "platform/win.h"

namespace kamil {

struct ConsoleTab {
    enum class State { Running, Ok, Failed };
    uint64_t id = 0;
    std::wstring title;    // "Run SensorUI · x64-debug" style, shown on the tab
    std::wstring detail;   // command line or source, shown above the output
    std::wstring root;     // folder that relative file names in the output are resolved against
    bool stoppable = false;  // a program Kamil started for this tab (Stop / close ends it)
    bool rerunnable = false;
    State state = State::Running;
    ULONGLONG started = 0, finished = 0;
    ConsoleBuffer buffer;

    // View state
    size_t top = 0;              // first visible line
    bool follow = true;          // stick to the end while output arrives
    size_t sel_first = SIZE_MAX, sel_last = SIZE_MAX;
    uint64_t seen_dropped = 0;   // buffer.dropped() when top/selection were last adjusted
};

class ConsoleWindow {
public:
    struct Callbacks {
        std::function<void(uint64_t tab)> stop;
        std::function<void(uint64_t tab)> rerun;
        std::function<void(const ConsoleTab& tab, const std::wstring& file, int line)> open_location;
        std::function<void(uint64_t tab)> closed;  // after the user closed it
    };

    ~ConsoleWindow();
    bool create(HINSTANCE instance, HICON icon, Callbacks callbacks, std::filesystem::path placement_file);
    void destroy();

    // A tab for a new run. A finished tab with the same title is reused (cleared) instead of
    // piling up tabs; a running one is kept and a new tab opens next to it.
    uint64_t open_tab(const std::wstring& title, const std::wstring& detail, const std::wstring& root, bool stoppable, bool rerunnable);
    void append(uint64_t tab, std::wstring_view text);
    void note(uint64_t tab, std::wstring_view text);
    void finish(uint64_t tab, bool ok, std::wstring_view summary);
    ConsoleTab* tab(uint64_t id);

    void show(bool activate);
    void hide();
    bool visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    void set_theme(bool dark, uint32_t accent_rgb);

private:
    static LRESULT CALLBACK wndproc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    void render();
    void schedule_render();
    bool ensure_target();
    void create_formats();
    void layout();
    ConsoleTab* active();
    void activate_tab(size_t index);
    void close_tab(size_t index);
    void scroll_to(ConsoleTab& t, size_t top);
    void sync_dropped(ConsoleTab& t);
    size_t visible_lines() const;
    void ensure_line_visible(ConsoleTab& t, size_t line);
    void open_line(ConsoleTab& t, size_t line);
    void jump_issue(bool backwards);
    void copy_selection();
    void on_key(WPARAM vk);
    void on_click(int x, int y, bool shift, bool double_click);
    int line_at(float y) const;
    void save_placement();
    void load_placement();

    float px(float dip) const { return dip * dpi_ / 96.f; }

    HWND hwnd_ = nullptr;
    Callbacks cb_;
    std::filesystem::path placement_file_;
    float dpi_ = 96.f;
    bool dark_ = true;
    uint32_t accent_ = 0x0078D4;
    bool render_pending_ = false;

    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTextFormat> fmt_mono_, fmt_ui_, fmt_ui_small_;
    float line_h_ = 18.f, char_w_ = 8.f;

    // Layout (DIPs)
    float width_ = 900.f, height_ = 500.f;
    const float tabs_h_ = 34.f, header_h_ = 28.f, pad_ = 10.f, scrollbar_w_ = 10.f;

    struct HitRect {
        D2D1_RECT_F rect;
        int kind;   // 0 tab, 1 tab close, 2 stop, 3 rerun, 4 next error, 5 copy
        size_t index;
    };
    std::vector<HitRect> hits_;
    int hover_hit_ = -1;
    int hover_line_ = -1;
    bool dragging_thumb_ = false;
    float drag_offset_ = 0;

    std::vector<std::unique_ptr<ConsoleTab>> tabs_;
    size_t active_ = 0;
    uint64_t next_id_ = 1;
    float x_offset_ = 0;  // horizontal scroll of long lines (DIPs)
    bool maximized_on_show_ = false;
};

}  // namespace kamil
