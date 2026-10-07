#include "ui/launcher_window.h"

#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "core/text.h"
#include "platform/system_theme.h"

namespace kamil {

namespace {

constexpr UINT_PTR kCaretTimer = 1;

D2D1_COLOR_F rgb(uint32_t rgb, float a = 1.f) {
    return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xFF) / 255.f, static_cast<float>((rgb >> 8) & 0xFF) / 255.f,
                        static_cast<float>(rgb & 0xFF) / 255.f, a);
}

D2D1_COLOR_F mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

D2D1_COLOR_F with_alpha(D2D1_COLOR_F c, float a) {
    c.a = a;
    return c;
}

bool parse_hex_color(const std::string& s, uint32_t* out) {
    if (s.size() != 7 && s.size() != 4 && s.size() != 9) return false;
    if (s[0] != '#') return false;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    uint32_t v = 0;
    if (s.size() == 4) {
        for (int i = 1; i <= 3; ++i) {
            const int h = hex(s[i]);
            if (h < 0) return false;
            v = (v << 8) | static_cast<uint32_t>(h * 17);
        }
    } else {
        for (int i = 1; i <= 6; ++i) {
            const int h = hex(s[i]);
            if (h < 0) return false;
            v = (v << 4) | static_cast<uint32_t>(h);
        }
    }
    *out = v;
    return true;
}

bool key_down(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

const wchar_t* font_family() {
    static const wchar_t* family = is_windows11() ? L"Segoe UI Variable Text" : L"Segoe UI";
    return family;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Creation

bool LauncherWindow::create(HINSTANCE instance, Callbacks callbacks) {
    cb_ = std::move(callbacks);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &LauncherWindow::wndproc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kLauncherWindowClass;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, kLauncherWindowClass, L"Kamil",
                            WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;
    if (!renderer_.init(hwnd_)) return false;
    update_dpi(static_cast<float>(GetDpiForWindow(hwnd_)));
    update_palette();
    layout();
    return true;
}

void LauncherWindow::destroy() {
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

LRESULT CALLBACK LauncherWindow::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        static_cast<LauncherWindow*>(cs->lpCreateParams)->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<LauncherWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------
// Style, palette, metrics

void LauncherWindow::apply_style(const LauncherStyle& style) {
    const bool fonts_changed = style.font_size != style_.font_size;
    style_ = style;
    if (fonts_changed || !fmt_input_) create_text_formats();
    if (fonts_changed) clear_icons();
    update_palette();
    layout();
    if (visible()) {
        place_window();
        render();
    }
}

void LauncherWindow::on_system_theme_changed() {
    update_palette();
    if (visible()) render();
}

void LauncherWindow::update_palette() {
    const SystemTheme sys = read_system_theme();
    dark_ = style_.theme == "dark" || (style_.theme == "auto" && sys.apps_dark);
    uint32_t accent_rgb = sys.accent_rgb;
    if (style_.accent != "auto") parse_hex_color(style_.accent, &accent_rgb);
    const D2D1_COLOR_F accent = rgb(accent_rgb);
    const D2D1_COLOR_F white = D2D1::ColorF(1, 1, 1, 1), black = D2D1::ColorF(0, 0, 0, 1);

    if (dark_) {
        pal_.surface = rgb(0x1F1F1F);
        pal_.border = D2D1::ColorF(1, 1, 1, 0.08f);
        pal_.text = rgb(0xF2F2F2);
        pal_.text_secondary = rgb(0xA0A0A0);
        pal_.divider = D2D1::ColorF(1, 1, 1, 0.07f);
        pal_.selection = with_alpha(accent, 0.26f);
        pal_.accent_text = mix(accent, white, 0.45f);
        pal_.shadow_alpha = 0.055f;
    } else {
        pal_.surface = rgb(0xF9F9F9);
        pal_.border = D2D1::ColorF(0, 0, 0, 0.10f);
        pal_.text = rgb(0x1A1A1A);
        pal_.text_secondary = rgb(0x5F5F5F);
        pal_.divider = D2D1::ColorF(0, 0, 0, 0.06f);
        pal_.selection = with_alpha(accent, 0.16f);
        pal_.accent_text = mix(accent, black, 0.15f);
        pal_.shadow_alpha = 0.035f;
    }
    pal_.caret = pal_.text;
    pal_.text_selection = with_alpha(accent, 0.40f);
}

void LauncherWindow::create_text_formats() {
    IDWriteFactory* dw = renderer_.dwrite();
    if (!dw) return;
    const float base = static_cast<float>(style_.font_size);
    auto make = [&](float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>* out) {
        out->Reset();
        dw->CreateTextFormat(font_family(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"tr-TR",
                             out->GetAddressOf());
        if (*out) {
            (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
    };
    make(std::round(base * 1.43f), DWRITE_FONT_WEIGHT_NORMAL, &fmt_input_);
    make(base, DWRITE_FONT_WEIGHT_NORMAL, &fmt_title_);
    make(std::round(base * 0.86f), DWRITE_FONT_WEIGHT_NORMAL, &fmt_subtitle_);
    make(std::round(base * 0.86f), DWRITE_FONT_WEIGHT_NORMAL, &fmt_hint_);
    make(std::round(base * 1.15f), DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmt_letter_);
    if (fmt_hint_) fmt_hint_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (fmt_letter_) {
        fmt_letter_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt_letter_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    ellipsis_title_.Reset();
    ellipsis_subtitle_.Reset();
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    if (fmt_title_ && SUCCEEDED(dw->CreateEllipsisTrimmingSign(fmt_title_.Get(), &ellipsis_title_)))
        fmt_title_->SetTrimming(&trimming, ellipsis_title_.Get());
    if (fmt_subtitle_ && SUCCEEDED(dw->CreateEllipsisTrimmingSign(fmt_subtitle_.Get(), &ellipsis_subtitle_)))
        fmt_subtitle_->SetTrimming(&trimming, ellipsis_subtitle_.Get());
}

void LauncherWindow::update_dpi(float dpi) {
    if (dpi <= 0) dpi = 96.f;
    if (dpi == dpi_ && fmt_input_) return;
    dpi_ = dpi;
    renderer_.set_dpi(dpi_);
    create_text_formats();
    clear_icons();
}

void LauncherWindow::clear_icons() {
    icons_.clear();
    icon_pending_.clear();
}

void LauncherWindow::layout() {
    margin_ = 18.f;
    panel_w_ = static_cast<float>(style_.width);
    input_h_ = std::round(s(56.f));
    row_h_ = std::round(s(48.f));
    pad_ = 6.f;
    const size_t max_rows = static_cast<size_t>(std::max(1, style_.max_rows));
    visible_rows_ = std::min(hits_.size(), max_rows);
    const bool empty_notice = hits_.empty() && !trim(std::wstring_view(edit_.text())).empty();
    const size_t rows_for_height = empty_notice ? 1 : visible_rows_;
    panel_h_ = input_h_ + (rows_for_height ? 1.f + 2.f * pad_ + static_cast<float>(rows_for_height) * row_h_ : 0.f);

    // Swap chain sized for the largest window, so result changes never reallocate buffers.
    const float max_h = input_h_ + 1.f + 2.f * pad_ + static_cast<float>(max_rows) * row_h_;
    renderer_.ensure_size(static_cast<UINT>(std::ceil(px(panel_w_ + 2 * margin_))),
                          static_cast<UINT>(std::ceil(px(max_h + 2 * margin_))));
}

void LauncherWindow::place_window() {
    const int w = static_cast<int>(std::ceil(px(panel_w_ + 2 * margin_)));
    const int h = static_cast<int>(std::ceil(px(panel_h_ + 2 * margin_)));
    SetWindowPos(hwnd_, HWND_TOPMOST, origin_px_.x, origin_px_.y, w, h, SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------------------------
// Show / hide

void LauncherWindow::show() {
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    update_dpi(static_cast<float>(dx));

    if (style_.remember_query) {
        edit_.select_all();
    } else if (!edit_.text().empty()) {
        edit_.clear();
        input_scroll_ = 0;
        if (cb_.query_changed) cb_.query_changed(edit_.text());
    } else if (cb_.query_changed) {
        cb_.query_changed(edit_.text());  // refresh "frequent" list
    }
    selected_ = 0;
    scroll_ = 0;
    layout();

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    const RECT& work = mi.rcWork;
    const int panel_w = static_cast<int>(std::round(px(panel_w_)));
    const int x = work.left + ((work.right - work.left) - panel_w) / 2 - static_cast<int>(std::round(px(margin_)));
    const int y = work.top + (work.bottom - work.top) * style_.position / 100 - static_cast<int>(std::round(px(margin_)));
    origin_px_ = {x, y};

    place_window();
    renderer_.animate_in(false);
    render();
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    renderer_.animate_in(style_.animations);

    GetCursorPos(&last_mouse_);
    hover_armed_ = false;
    restart_caret();
}

void LauncherWindow::hide() {
    if (!hwnd_) return;
    KillTimer(hwnd_, kCaretTimer);
    if (IsWindowVisible(hwnd_)) hidden_at_ = GetTickCount64();
    ShowWindow(hwnd_, SW_HIDE);
}

void LauncherWindow::restart_caret() {
    caret_on_ = true;
    SetTimer(hwnd_, kCaretTimer, GetCaretBlinkTime(), nullptr);
}

// ---------------------------------------------------------------------------------------------
// Results

void LauncherWindow::set_results(std::vector<Hit> hits) {
    hits_ = std::move(hits);
    selected_ = 0;
    scroll_ = 0;
    layout();
    if (visible()) {
        place_window();
        render();
    }
}

void LauncherWindow::move_selection(int delta) {
    if (hits_.empty()) return;
    const int n = static_cast<int>(hits_.size());
    int next = static_cast<int>(selected_) + delta;
    if (delta == 1 && next >= n) next = 0;            // wrap with single steps only
    else if (delta == -1 && next < 0) next = n - 1;
    next = std::clamp(next, 0, n - 1);
    selected_ = static_cast<size_t>(next);
    ensure_visible();
    render();
}

void LauncherWindow::ensure_visible() {
    if (visible_rows_ == 0) return;
    if (selected_ < scroll_) scroll_ = selected_;
    if (selected_ >= scroll_ + visible_rows_) scroll_ = selected_ + 1 - visible_rows_;
}

LaunchMode LauncherWindow::mode_from_keyboard() const {
    if (key_down(VK_SHIFT) && key_down(VK_CONTROL)) return LaunchMode::Admin;
    if (key_down(VK_SHIFT)) return LaunchMode::Admin;
    if (key_down(VK_CONTROL)) return LaunchMode::OpenLocation;
    return LaunchMode::Normal;
}

void LauncherWindow::activate(size_t index, LaunchMode mode) {
    if (index >= hits_.size()) return;
    const Item item = *hits_[index].item;  // the callback may rebuild the item list
    if (cb_.activate) cb_.activate(item, mode);
}

// ---------------------------------------------------------------------------------------------
// Input

void LauncherWindow::text_changed() {
    restart_caret();
    if (cb_.query_changed) cb_.query_changed(edit_.text());  // calls set_results()
    render();
}

void LauncherWindow::on_char(wchar_t ch) {
    if (ch < 0x20 || ch == 0x7F) return;  // control characters are handled in on_key
    if (key_down(VK_CONTROL) && !key_down(VK_MENU)) return;  // Ctrl+letter shortcuts (AltGr = Ctrl+Alt is text)
    if (edit_.insert(std::wstring_view(&ch, 1))) text_changed();
}

bool LauncherWindow::on_key(WPARAM vk, bool alt) {
    // AltGr arrives as Ctrl+Alt: it types characters (@, \, |, # on a Turkish Q keyboard),
    // so it must not trigger Ctrl shortcuts.
    const bool ctrl = key_down(VK_CONTROL) && !key_down(VK_MENU);
    const bool shift = key_down(VK_SHIFT);

    if (alt) {
        if (vk >= '1' && vk <= '9') {
            const size_t index = scroll_ + (vk - '1');
            if (index < scroll_ + visible_rows_) activate(index, LaunchMode::Normal);
            return true;
        }
        if (vk == VK_F4) {
            hide();
            return true;
        }
        return false;
    }

    switch (vk) {
        case VK_ESCAPE:
            if (edit_.has_selection() && style_.remember_query) {
                edit_.end(false);
                render();
            } else {
                hide();
            }
            return true;
        case VK_RETURN:
            activate(selected_, mode_from_keyboard());
            return true;
        case VK_UP:
            move_selection(-1);
            return true;
        case VK_DOWN:
            move_selection(1);
            return true;
        case VK_PRIOR:
            move_selection(-static_cast<int>(std::max<size_t>(1, visible_rows_)));
            return true;
        case VK_NEXT:
            move_selection(static_cast<int>(std::max<size_t>(1, visible_rows_)));
            return true;
        case VK_LEFT:
            edit_.move_left(ctrl, shift);
            restart_caret();
            render();
            return true;
        case VK_RIGHT:
            edit_.move_right(ctrl, shift);
            restart_caret();
            render();
            return true;
        case VK_HOME:
            if (ctrl) {
                move_selection(-static_cast<int>(hits_.size()));
            } else {
                edit_.home(shift);
                restart_caret();
                render();
            }
            return true;
        case VK_END:
            if (ctrl) {
                move_selection(static_cast<int>(hits_.size()));
            } else {
                edit_.end(shift);
                restart_caret();
                render();
            }
            return true;
        case VK_BACK:
            if (edit_.backspace(ctrl)) text_changed();
            return true;
        case VK_DELETE:
            if (edit_.del(ctrl)) text_changed();
            return true;
        default:
            break;
    }

    if (ctrl) {
        switch (vk) {
            case 'A':
                edit_.select_all();
                render();
                return true;
            case 'V':
                if (edit_.insert(read_clipboard_text(hwnd_))) text_changed();
                return true;
            case 'X':
                if (edit_.has_selection()) {
                    copy_to_clipboard(hwnd_, edit_.selected_text());
                    edit_.insert(L"");
                    text_changed();
                }
                return true;
            case 'C':
                if (edit_.has_selection()) copy_to_clipboard(hwnd_, edit_.selected_text());
                else if (selected_ < hits_.size() && cb_.copy_item) cb_.copy_item(*hits_[selected_].item);
                return true;
            case 'J':
                move_selection(1);
                return true;
            case 'K':
                move_selection(-1);
                return true;
            default:
                break;
        }
    }
    return false;
}

int LauncherWindow::row_at(int x_px, int y_px) const {
    const float x = static_cast<float>(x_px) * 96.f / dpi_ - margin_;
    const float y = static_cast<float>(y_px) * 96.f / dpi_ - margin_;
    if (x < 0 || x > panel_w_) return -1;
    const float list_top = input_h_ + 1.f + pad_;
    if (y < list_top) return -1;
    const int row = static_cast<int>((y - list_top) / row_h_);
    if (row < 0 || static_cast<size_t>(row) >= visible_rows_) return -1;
    return static_cast<int>(scroll_) + row;
}

// ---------------------------------------------------------------------------------------------
// Window procedure

LRESULT LauncherWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && style_.hide_on_focus_loss) hide();
            return 0;
        case WM_CHAR:
            on_char(static_cast<wchar_t>(wp));
            return 0;
        case WM_KEYDOWN:
            if (on_key(wp, false)) return 0;
            break;
        case WM_SYSKEYDOWN:
            if (on_key(wp, true)) return 0;
            break;
        case WM_SYSCHAR:
            return 0;  // no beep for Alt+digit
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU) return 0;  // a lone Alt must not enter (invisible) menu mode
            break;
        case WM_TIMER:
            if (wp == kCaretTimer) {
                caret_on_ = !caret_on_;
                render();
            }
            return 0;
        case WM_MOUSEMOVE: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            POINT screen = p;
            ClientToScreen(hwnd_, &screen);
            if (!hover_armed_) {  // ignore the cursor that happens to be over the window when it opens
                if (std::abs(screen.x - last_mouse_.x) + std::abs(screen.y - last_mouse_.y) < 4) return 0;
                hover_armed_ = true;
            }
            const int row = row_at(p.x, p.y);
            if (row >= 0 && static_cast<size_t>(row) != selected_) {
                selected_ = static_cast<size_t>(row);
                render();
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const float x = static_cast<float>(GET_X_LPARAM(lp)) * 96.f / dpi_ - margin_;
            const float y = static_cast<float>(GET_Y_LPARAM(lp)) * 96.f / dpi_ - margin_;
            if (x < 0 || y < 0 || x > panel_w_ || y > panel_h_) hide();  // click on the shadow = outside
            return 0;
        }
        case WM_LBUTTONUP: {
            const int row = row_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (row >= 0) activate(static_cast<size_t>(row), mode_from_keyboard());
            return 0;
        }
        case WM_MOUSEWHEEL:
            move_selection(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
            return 0;
        case WM_DPICHANGED:
            update_dpi(static_cast<float>(HIWORD(wp)));
            layout();
            if (visible()) {
                place_window();
                render();
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            EndPaint(hwnd_, &ps);
            render();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------
// Drawing

void LauncherWindow::on_icon(const IconResult& r) {
    icon_pending_.erase(r.key);
    Icon& icon = icons_[r.key];
    icon.failed = r.bgra.empty();
    icon.size = r.size;
    icon.pixels = r.bgra;
    icon.bitmap.Reset();
    if (visible()) render();
}

ID2D1Bitmap1* LauncherWindow::icon_for(const Item& item) {
    const uint32_t want = static_cast<uint32_t>(std::lround(px(s(32.f))));
    auto it = icons_.find(item.key);
    if (it == icons_.end() || (!it->second.failed && it->second.size != want)) {
        if (!icon_pending_.count(item.key) && cb_.need_icon) {
            icon_pending_.insert(item.key);
            cb_.need_icon(item, want);
        }
        return nullptr;
    }
    Icon& icon = it->second;
    if (icon.failed) return nullptr;
    if (!icon.bitmap || icon.generation != renderer_.generation()) {
        icon.bitmap = renderer_.create_bitmap(icon.pixels.data(), icon.size, icon.size);
        icon.generation = renderer_.generation();
    }
    return icon.bitmap.Get();
}

void LauncherWindow::render() {
    if (!hwnd_) return;
    ID2D1DeviceContext* dc = renderer_.begin_draw();
    if (!dc) return;
    const uint64_t gen = renderer_.generation();

    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc->SetTransform(D2D1::Matrix3x2F::Identity());

    ComPtr<ID2D1SolidColorBrush> brush;
    dc->CreateSolidColorBrush(pal_.surface, &brush);
    if (!brush) {
        renderer_.end_draw();
        return;
    }

    const float x0 = margin_, y0 = margin_;
    const float radius = 10.f;

    // Soft shadow: concentric rounded rectangles with decreasing alpha.
    constexpr int kLayers = 14;
    for (int i = kLayers; i >= 1; --i) {
        const float k = static_cast<float>(i);
        const float t = 1.f - k / (kLayers + 1.f);
        brush->SetColor(D2D1::ColorF(0, 0, 0, pal_.shadow_alpha * t * t));
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF(x0 - k, y0 - k + 3.f, x0 + panel_w_ + k, y0 + panel_h_ + k + 3.f), radius + k, radius + k);
        dc->FillRoundedRectangle(rr, brush.Get());
    }

    const D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(D2D1::RectF(x0, y0, x0 + panel_w_, y0 + panel_h_), radius, radius);
    brush->SetColor(pal_.surface);
    dc->FillRoundedRectangle(panel, brush.Get());
    brush->SetColor(pal_.border);
    const D2D1_ROUNDED_RECT border =
        D2D1::RoundedRect(D2D1::RectF(x0 + 0.5f, y0 + 0.5f, x0 + panel_w_ - 0.5f, y0 + panel_h_ - 0.5f), radius, radius);
    dc->DrawRoundedRectangle(border, brush.Get(), 1.f);

    // ---- search box
    IDWriteFactory* dw = renderer_.dwrite();
    const float text_x = x0 + 20.f;
    const float text_w = panel_w_ - 40.f;
    if (fmt_input_ && dw) {
        const std::wstring& text = edit_.text();
        const bool placeholder = text.empty();
        const std::wstring shown = placeholder ? std::wstring(L"Uygulama veya komut ara…") : text;
        ComPtr<IDWriteTextLayout> tl;
        dw->CreateTextLayout(shown.c_str(), static_cast<UINT32>(shown.size()), fmt_input_.Get(), 10000.f, input_h_, &tl);
        if (tl) {
            DWRITE_TEXT_METRICS m{};
            tl->GetMetrics(&m);
            const float ty = y0 + (input_h_ - m.height) / 2.f;

            float caret_x = 0, caret_y = 0;
            DWRITE_HIT_TEST_METRICS hm{};
            if (!placeholder) tl->HitTestTextPosition(static_cast<UINT32>(edit_.caret()), FALSE, &caret_x, &caret_y, &hm);
            if (caret_x - input_scroll_ > text_w) input_scroll_ = caret_x - text_w;
            if (caret_x < input_scroll_) input_scroll_ = caret_x;
            if (placeholder) input_scroll_ = 0;

            dc->PushAxisAlignedClip(D2D1::RectF(text_x - 2.f, y0, text_x + text_w + 2.f, y0 + input_h_), D2D1_ANTIALIAS_MODE_ALIASED);
            const float ox = text_x - input_scroll_;
            if (!placeholder && edit_.has_selection()) {
                UINT32 count = 0;
                tl->HitTestTextRange(static_cast<UINT32>(edit_.selection_start()),
                                     static_cast<UINT32>(edit_.selection_end() - edit_.selection_start()), 0, 0, nullptr, 0, &count);
                std::vector<DWRITE_HIT_TEST_METRICS> ranges(count);
                if (count &&
                    SUCCEEDED(tl->HitTestTextRange(static_cast<UINT32>(edit_.selection_start()),
                                                   static_cast<UINT32>(edit_.selection_end() - edit_.selection_start()), 0, 0,
                                                   ranges.data(), count, &count))) {
                    brush->SetColor(pal_.text_selection);
                    for (const auto& r : ranges)
                        dc->FillRectangle(D2D1::RectF(ox + r.left, ty + r.top, ox + r.left + r.width, ty + r.top + r.height), brush.Get());
                }
            }
            brush->SetColor(placeholder ? pal_.text_secondary : pal_.text);
            dc->DrawTextLayout(D2D1::Point2F(ox, ty), tl.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            if (caret_on_ && GetForegroundWindow() == hwnd_) {
                brush->SetColor(pal_.caret);
                const float cx = std::round(ox + caret_x) + 0.5f;
                const float ch = placeholder ? m.height : hm.height;
                dc->DrawLine(D2D1::Point2F(cx, ty + caret_y), D2D1::Point2F(cx, ty + caret_y + ch), brush.Get(), 1.5f);
            }
            dc->PopAxisAlignedClip();
        }
    }

    // ---- results
    const bool empty_notice = hits_.empty() && !trim(std::wstring_view(edit_.text())).empty();
    if (visible_rows_ > 0 || empty_notice) {
        brush->SetColor(pal_.divider);
        dc->FillRectangle(D2D1::RectF(x0 + 1.f, y0 + input_h_, x0 + panel_w_ - 1.f, y0 + input_h_ + 1.f), brush.Get());
    }
    const float list_top = y0 + input_h_ + 1.f + pad_;
    if (empty_notice && fmt_title_ && dw) {
        const std::wstring msg = L"Sonuç yok";
        brush->SetColor(pal_.text_secondary);
        ComPtr<IDWriteTextLayout> tl;
        dw->CreateTextLayout(msg.c_str(), static_cast<UINT32>(msg.size()), fmt_title_.Get(), panel_w_, row_h_, &tl);
        if (tl) {
            DWRITE_TEXT_METRICS m{};
            tl->GetMetrics(&m);
            dc->DrawTextLayout(D2D1::Point2F(text_x, list_top + (row_h_ - m.height) / 2.f), tl.Get(), brush.Get());
        }
    }
    for (size_t i = 0; i < visible_rows_; ++i) draw_row(dc, scroll_ + i, list_top + static_cast<float>(i) * row_h_);

    if (!renderer_.end_draw() || renderer_.generation() != gen) {
        // Device lost: everything device-dependent is gone; draw again with fresh resources.
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void LauncherWindow::draw_row(ID2D1DeviceContext* dc, size_t index, float y) {
    if (index >= hits_.size()) return;
    const Hit& hit = hits_[index];
    const Item& item = *hit.item;
    IDWriteFactory* dw = renderer_.dwrite();
    const float x0 = margin_;
    const bool selected = index == selected_;

    ComPtr<ID2D1SolidColorBrush> brush, accent;
    dc->CreateSolidColorBrush(pal_.text, &brush);
    dc->CreateSolidColorBrush(pal_.accent_text, &accent);
    if (!brush || !accent) return;

    if (selected) {
        brush->SetColor(pal_.selection);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0 + 6.f, y + 1.f, x0 + panel_w_ - 6.f, y + row_h_ - 1.f), 6.f, 6.f),
                                 brush.Get());
    }

    // icon
    const float icon = s(32.f);
    const float ix = x0 + 16.f, iy = y + (row_h_ - icon) / 2.f;
    if (ID2D1Bitmap1* bmp = icon_for(item)) {
        dc->DrawBitmap(bmp, D2D1::RectF(ix, iy, ix + icon, iy + icon), 1.f, D2D1_INTERPOLATION_MODE_LINEAR);
    } else {
        brush->SetColor(with_alpha(pal_.text_secondary, 0.18f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ix, iy, ix + icon, iy + icon), 7.f, 7.f), brush.Get());
        if (fmt_letter_ && dw && !item.title.empty()) {
            const wchar_t letter[2] = {item.title[0], 0};
            brush->SetColor(pal_.text_secondary);
            dc->DrawText(letter, 1, fmt_letter_.Get(), D2D1::RectF(ix, iy, ix + icon, iy + icon), brush.Get());
        }
    }

    // hint
    const float hint_w = 64.f;
    const float right = x0 + panel_w_ - 18.f;
    std::wstring hint;
    if (selected) hint = L"↵";
    else if (index >= scroll_ && index - scroll_ < 9) hint = L"Alt+" + std::to_wstring(index - scroll_ + 1);
    if (!hint.empty() && fmt_hint_ && dw) {
        brush->SetColor(pal_.text_secondary);
        dc->DrawText(hint.c_str(), static_cast<UINT32>(hint.size()), fmt_hint_.Get(),
                     D2D1::RectF(right - hint_w, y + (row_h_ - s(16.f)) / 2.f, right, y + row_h_), brush.Get());
    }

    // title + subtitle
    if (!dw || !fmt_title_ || !fmt_subtitle_) return;
    const float tx = ix + icon + 12.f;
    const float tw = std::max(10.f, right - hint_w - 8.f - tx);
    ComPtr<IDWriteTextLayout> title, sub;
    dw->CreateTextLayout(item.title.c_str(), static_cast<UINT32>(item.title.size()), fmt_title_.Get(), tw, row_h_, &title);
    if (!item.subtitle.empty())
        dw->CreateTextLayout(item.subtitle.c_str(), static_cast<UINT32>(item.subtitle.size()), fmt_subtitle_.Get(), tw, row_h_, &sub);
    if (!title) return;

    // Highlight matched characters: consecutive positions become one range.
    for (size_t i = 0; i < hit.positions.size();) {
        size_t j = i + 1;
        while (j < hit.positions.size() && hit.positions[j] == hit.positions[j - 1] + 1) ++j;
        title->SetDrawingEffect(accent.Get(), DWRITE_TEXT_RANGE{hit.positions[i], static_cast<UINT32>(j - i)});
        title->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_RANGE{hit.positions[i], static_cast<UINT32>(j - i)});
        i = j;
    }

    DWRITE_TEXT_METRICS tm{}, sm{};
    title->GetMetrics(&tm);
    if (sub) sub->GetMetrics(&sm);
    const float gap = sub ? 1.f : 0.f;
    const float total = tm.height + gap + (sub ? sm.height : 0.f);
    const float ty = y + (row_h_ - total) / 2.f;
    brush->SetColor(pal_.text);
    dc->DrawTextLayout(D2D1::Point2F(tx, ty), title.Get(), brush.Get());
    if (sub) {
        brush->SetColor(pal_.text_secondary);
        dc->DrawTextLayout(D2D1::Point2F(tx, ty + tm.height + gap), sub.Get(), brush.Get());
    }
}

}  // namespace kamil
