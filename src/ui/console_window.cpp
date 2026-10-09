#include "ui/console_window.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <fstream>

#include "core/i18n.h"
#include "platform/shell.h"

namespace kamil {

namespace {

constexpr wchar_t kConsoleClass[] = L"Kamil.Console";
constexpr UINT_PTR kRenderTimer = 1;
constexpr UINT_PTR kClockTimer = 2;
constexpr size_t kMaxTabs = 16;

enum Hit { kHitTab, kHitClose, kHitStop, kHitRerun, kHitNext, kHitCopy };

D2D1_COLOR_F rgb(uint32_t v, float a = 1.f) {
    return D2D1::ColorF(static_cast<float>((v >> 16) & 0xFF) / 255.f, static_cast<float>((v >> 8) & 0xFF) / 255.f,
                        static_cast<float>(v & 0xFF) / 255.f, a);
}

D2D1_COLOR_F with_alpha(D2D1_COLOR_F c, float a) {
    c.a = a;
    return c;
}

bool font_exists(IDWriteFactory* dw, const wchar_t* family) {
    ComPtr<IDWriteFontCollection> fonts;
    if (FAILED(dw->GetSystemFontCollection(&fonts))) return false;
    UINT32 index = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists;
}

std::wstring clock_text(ULONGLONG ms) {
    const ULONGLONG s = ms / 1000;
    wchar_t buf[16];
    swprintf(buf, 16, L"%llu:%02llu", s / 60, s % 60);
    return buf;
}

bool contains(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

}  // namespace

ConsoleWindow::~ConsoleWindow() { destroy(); }

bool ConsoleWindow::create(HINSTANCE instance, HICON icon, Callbacks callbacks, std::filesystem::path placement_file) {
    cb_ = std::move(callbacks);
    placement_file_ = std::move(placement_file);
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &ConsoleWindow::wndproc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = icon;
    wc.hIconSm = icon;
    wc.lpszClassName = kConsoleClass;
    RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(0, kConsoleClass, L"Kamil Console", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 560, nullptr,
                            nullptr, instance, this);
    if (!hwnd_) return false;
    dpi_ = static_cast<float>(GetDpiForWindow(hwnd_));
    // Default size in DIPs, centred on the primary work area; a saved placement wins.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int w = static_cast<int>(px(1000)), h = static_cast<int>(px(560));
    SetWindowPos(hwnd_, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    load_placement();
    create_formats();
    set_theme(dark_, accent_);
    return true;
}

void ConsoleWindow::destroy() {
    if (!hwnd_) return;
    save_placement();
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

LRESULT CALLBACK ConsoleWindow::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        static_cast<ConsoleWindow*>(cs->lpCreateParams)->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<ConsoleWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

void ConsoleWindow::set_theme(bool dark, uint32_t accent_rgb) {
    dark_ = dark;
    accent_ = accent_rgb;
    if (hwnd_) {
        const BOOL on = dark ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &on, sizeof(on));
        if (visible()) render();
    }
}

void ConsoleWindow::create_formats() {
    const wchar_t* mono = font_exists(dwrite_.Get(), L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";
    fmt_mono_.Reset();
    fmt_ui_.Reset();
    fmt_ui_small_.Reset();
    dwrite_->CreateTextFormat(mono, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.f, L"",
                              &fmt_mono_);
    dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.5f,
                              L"", &fmt_ui_);
    dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.5f,
                              L"", &fmt_ui_small_);
    for (IDWriteTextFormat* f : {fmt_mono_.Get(), fmt_ui_.Get(), fmt_ui_small_.Get()}) {
        if (!f) continue;
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (fmt_mono_) {
        ComPtr<IDWriteTextLayout> probe;
        if (SUCCEEDED(dwrite_->CreateTextLayout(L"MMMMMMMMMM", 10, fmt_mono_.Get(), 1000.f, 100.f, &probe))) {
            DWRITE_TEXT_METRICS m{};
            probe->GetMetrics(&m);
            char_w_ = m.widthIncludingTrailingWhitespace / 10.f;
            line_h_ = std::round(m.height * 1.25f);
        }
    }
    for (IDWriteTextFormat* f : {fmt_ui_.Get(), fmt_ui_small_.Get()}) {
        if (!f) continue;
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(f, &ellipsis))) f->SetTrimming(&trim, ellipsis.Get());
    }
}

bool ConsoleWindow::ensure_target() {
    if (target_) return true;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), dpi_, dpi_);
    if (FAILED(factory_->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom))),
                                                &target_)))
        return false;
    target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &brush_);
    return brush_ != nullptr;
}

void ConsoleWindow::layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    width_ = static_cast<float>(rc.right) * 96.f / dpi_;
    height_ = static_cast<float>(rc.bottom) * 96.f / dpi_;
}

size_t ConsoleWindow::visible_lines() const {
    const float h = height_ - tabs_h_ - header_h_ - 8.f;
    return h > line_h_ ? static_cast<size_t>(h / line_h_) : 1;
}

ConsoleTab* ConsoleWindow::active() { return active_ < tabs_.size() ? tabs_[active_].get() : nullptr; }

ConsoleTab* ConsoleWindow::tab(uint64_t id) {
    for (auto& t : tabs_)
        if (t->id == id) return t.get();
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Tabs and content

uint64_t ConsoleWindow::open_tab(const std::wstring& title, const std::wstring& detail, const std::wstring& root, bool stoppable,
                                 bool rerunnable) {
    size_t index = tabs_.size();
    for (size_t i = 0; i < tabs_.size(); ++i)
        if (tabs_[i]->title == title && tabs_[i]->state != ConsoleTab::State::Running) index = i;
    if (index == tabs_.size()) {
        // Too many tabs: drop the oldest finished one.
        if (tabs_.size() >= kMaxTabs) {
            for (size_t i = 0; i < tabs_.size(); ++i) {
                if (tabs_[i]->state == ConsoleTab::State::Running) continue;
                tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(i));
                if (active_ >= i && active_ > 0) --active_;
                break;
            }
            index = tabs_.size();
        }
        tabs_.push_back(std::make_unique<ConsoleTab>());
    } else {
        tabs_[index] = std::make_unique<ConsoleTab>();
    }
    ConsoleTab& t = *tabs_[index];
    t.id = next_id_++;
    t.title = title;
    t.detail = detail;
    t.root = root;
    t.stoppable = stoppable;
    t.rerunnable = rerunnable;
    t.started = GetTickCount64();
    active_ = index;
    x_offset_ = 0;
    SetTimer(hwnd_, kClockTimer, 1000, nullptr);
    schedule_render();
    return t.id;
}

void ConsoleWindow::sync_dropped(ConsoleTab& t) {
    const uint64_t d = t.buffer.dropped() - t.seen_dropped;
    if (d == 0) return;
    t.seen_dropped = t.buffer.dropped();
    const size_t n = static_cast<size_t>(d);
    t.top = t.top > n ? t.top - n : 0;
    if (t.sel_first != SIZE_MAX) {
        if (t.sel_last < n) {
            t.sel_first = t.sel_last = SIZE_MAX;
        } else {
            t.sel_first = t.sel_first > n ? t.sel_first - n : 0;
            t.sel_last -= n;
        }
    }
}

void ConsoleWindow::append(uint64_t id, std::wstring_view text) {
    ConsoleTab* t = tab(id);
    if (!t) return;
    t->buffer.append(text);
    sync_dropped(*t);
    if (t->follow) t->top = t->buffer.size() > visible_lines() ? t->buffer.size() - visible_lines() : 0;
    if (t == active()) schedule_render();
}

void ConsoleWindow::note(uint64_t id, std::wstring_view text) {
    ConsoleTab* t = tab(id);
    if (!t) return;
    t->buffer.note(text);
    sync_dropped(*t);
    if (t->follow) t->top = t->buffer.size() > visible_lines() ? t->buffer.size() - visible_lines() : 0;
    if (t == active()) schedule_render();
}

void ConsoleWindow::finish(uint64_t id, bool ok, std::wstring_view summary) {
    ConsoleTab* t = tab(id);
    if (!t) return;
    t->state = ok ? ConsoleTab::State::Ok : ConsoleTab::State::Failed;
    t->finished = GetTickCount64();
    if (!summary.empty()) note(id, summary);
    schedule_render();
}

void ConsoleWindow::activate_tab(size_t index) {
    if (index >= tabs_.size()) return;
    active_ = index;
    x_offset_ = 0;
    schedule_render();
}

void ConsoleWindow::close_tab(size_t index) {
    if (index >= tabs_.size()) return;
    ConsoleTab& t = *tabs_[index];
    if (t.state == ConsoleTab::State::Running && t.stoppable) {
        const std::wstring q = fmt(loc(L"\"{}\" is still running. Stop it and close the tab?", L"\"{}\" hâlâ çalışıyor. Durdurulup sekme kapatılsın mı?"), t.title);
        if (MessageBoxW(hwnd_, q.c_str(), L"Kamil", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        if (cb_.stop) cb_.stop(t.id);
    }
    const uint64_t id = t.id;
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
    if (active_ >= tabs_.size() && active_ > 0) active_ = tabs_.size() - 1;
    if (cb_.closed) cb_.closed(id);
    schedule_render();
}

void ConsoleWindow::scroll_to(ConsoleTab& t, size_t top) {
    const size_t n = t.buffer.size(), vis = visible_lines();
    const size_t max_top = n > vis ? n - vis : 0;
    t.top = std::min(top, max_top);
    t.follow = t.top == max_top;
    schedule_render();
}

void ConsoleWindow::ensure_line_visible(ConsoleTab& t, size_t line) {
    const size_t vis = visible_lines();
    if (line < t.top) scroll_to(t, line > vis / 3 ? line - vis / 3 : 0);
    else if (line >= t.top + vis) scroll_to(t, line - vis / 3);
}

void ConsoleWindow::open_line(ConsoleTab& t, size_t line) {
    if (line >= t.buffer.size()) return;
    const ConsoleLine& l = t.buffer.line(line);
    if (l.file.empty() || !cb_.open_location) return;
    std::filesystem::path p(l.file);
    if (p.is_relative() && !t.root.empty()) p = std::filesystem::path(t.root) / p;
    cb_.open_location(t, p.lexically_normal().make_preferred().wstring(), l.line);
}

void ConsoleWindow::jump_issue(bool backwards) {
    ConsoleTab* t = active();
    if (!t) return;
    const size_t from = t->sel_first != SIZE_MAX ? t->sel_first : (backwards ? 0 : t->buffer.size() - 1);
    if (auto i = t->buffer.next_issue(from, backwards)) {
        t->sel_first = t->sel_last = *i;
        ensure_line_visible(*t, *i);
        open_line(*t, *i);
        schedule_render();
    }
}

void ConsoleWindow::copy_selection() {
    ConsoleTab* t = active();
    if (!t || t->buffer.size() == 0) return;
    const bool any = t->sel_first != SIZE_MAX;
    const size_t a = any ? std::min(t->sel_first, t->sel_last) : 0;
    const size_t b = any ? std::max(t->sel_first, t->sel_last) : t->buffer.size() - 1;
    copy_to_clipboard(hwnd_, t->buffer.text(a, b));
}

// ---------------------------------------------------------------------------------------------
// Drawing

void ConsoleWindow::schedule_render() {
    if (!hwnd_ || !visible() || render_pending_) return;
    render_pending_ = true;
    SetTimer(hwnd_, kRenderTimer, 30, nullptr);  // output can arrive in many small chunks: ~30 fps
}

void ConsoleWindow::render() {
    render_pending_ = false;
    KillTimer(hwnd_, kRenderTimer);
    if (!visible() || !ensure_target()) return;
    layout();
    const D2D1_COLOR_F bg = rgb(dark_ ? 0x1A1A1A : 0xFBFBFB);
    const D2D1_COLOR_F strip = rgb(dark_ ? 0x202020 : 0xF0F0F0);
    const D2D1_COLOR_F text = rgb(dark_ ? 0xE8E8E8 : 0x1A1A1A);
    const D2D1_COLOR_F secondary = rgb(dark_ ? 0x9A9A9A : 0x666666);
    const D2D1_COLOR_F accent = rgb(accent_);
    const D2D1_COLOR_F error = rgb(dark_ ? 0xF1707B : 0xC42B1C);
    const D2D1_COLOR_F warning = rgb(dark_ ? 0xE5C07B : 0x9D5D00);
    const D2D1_COLOR_F ok = rgb(dark_ ? 0x6CCB5F : 0x0F7B0F);
    const D2D1_COLOR_F faint = dark_ ? D2D1::ColorF(1, 1, 1, 0.06f) : D2D1::ColorF(0, 0, 0, 0.05f);

    ID2D1HwndRenderTarget* rt = target_.Get();
    ID2D1SolidColorBrush* br = brush_.Get();
    rt->BeginDraw();
    rt->Clear(bg);
    hits_.clear();

    auto text_width = [&](const std::wstring& s, IDWriteTextFormat* f) {
        ComPtr<IDWriteTextLayout> l;
        if (FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, 2000.f, 40.f, &l))) return 0.f;
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    };
    auto draw_text = [&](const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
        br->SetColor(c);
        rt->DrawText(s.c_str(), static_cast<UINT32>(s.size()), f, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    auto state_color = [&](const ConsoleTab& t) {
        return t.state == ConsoleTab::State::Running ? accent : t.state == ConsoleTab::State::Ok ? ok : error;
    };

    // Tab strip
    br->SetColor(strip);
    rt->FillRectangle(D2D1::RectF(0, 0, width_, tabs_h_), br);
    float x = 6.f;
    for (size_t i = 0; i < tabs_.size(); ++i) {
        const ConsoleTab& t = *tabs_[i];
        const float w = std::clamp(text_width(t.title, fmt_ui_.Get()) + 46.f, 90.f, 260.f);
        const D2D1_RECT_F r = D2D1::RectF(x, 4.f, x + w, tabs_h_);
        const bool is_active = i == active_;
        const int hit_index = static_cast<int>(hits_.size());
        hits_.push_back({r, kHitTab, i});
        if (is_active || hover_hit_ == hit_index) {
            br->SetColor(is_active ? bg : faint);
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, r.top, r.right, r.bottom + 6.f), 6.f, 6.f), br);
        }
        br->SetColor(state_color(t));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + 14.f, (r.top + r.bottom) / 2.f), 3.5f, 3.5f), br);
        draw_text(t.title, fmt_ui_.Get(), D2D1::RectF(r.left + 24.f, r.top, r.right - 22.f, r.bottom), is_active ? text : secondary);
        const D2D1_RECT_F close = D2D1::RectF(r.right - 22.f, r.top + 6.f, r.right - 6.f, r.bottom - 6.f);
        hits_.push_back({close, kHitClose, i});
        if (is_active || hover_hit_ == hit_index || hover_hit_ == hit_index + 1) {
            if (hover_hit_ == hit_index + 1) {
                br->SetColor(faint);
                rt->FillRoundedRectangle(D2D1::RoundedRect(close, 3.f, 3.f), br);
            }
            draw_text(L"✕", fmt_ui_small_.Get(), D2D1::RectF(close.left + 3.f, close.top, close.right, close.bottom), secondary);
        }
        x += w + 2.f;
        if (x > width_) break;
    }

    ConsoleTab* t = active();
    if (!t) {
        draw_text(loc(L"No output yet. Runs (Alt+R), Visual Studio builds and custom commands show up here.",
                      L"Henüz çıktı yok. Çalıştırmalar (Alt+R), Visual Studio build'leri ve özel komutlar burada görünür."),
                  fmt_ui_.Get(), D2D1::RectF(pad_ + 8.f, tabs_h_ + 20.f, width_ - pad_, tabs_h_ + 50.f), secondary);
        rt->EndDraw();
        return;
    }
    sync_dropped(*t);

    // Header: what runs here, its state, and buttons.
    const float hy = tabs_h_;
    float right = width_ - pad_;
    auto button = [&](const std::wstring& label, int kind) {
        const float w = text_width(label, fmt_ui_small_.Get()) + 18.f;
        const D2D1_RECT_F r = D2D1::RectF(right - w, hy + 4.f, right, hy + header_h_ - 4.f);
        const int hit_index = static_cast<int>(hits_.size());
        hits_.push_back({r, kind, active_});
        br->SetColor(hover_hit_ == hit_index ? with_alpha(text, 0.14f) : faint);
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, 4.f, 4.f), br);
        draw_text(label, fmt_ui_small_.Get(), D2D1::RectF(r.left + 9.f, r.top, r.right, r.bottom), text);
        right = r.left - 6.f;
    };
    const bool running = t->state == ConsoleTab::State::Running;
    button(loc(L"Copy", L"Kopyala"), kHitCopy);
    if (t->buffer.errors() + t->buffer.warnings() > 0) button(loc(L"Next error  F8", L"Sonraki hata  F8"), kHitNext);
    if (running && t->stoppable) button(loc(L"Stop", L"Durdur"), kHitStop);
    if (!running && t->rerunnable) button(loc(L"Run again  F5", L"Tekrar çalıştır  F5"), kHitRerun);

    std::wstring status;
    const ULONGLONG end = running ? GetTickCount64() : t->finished;
    status = (running ? std::wstring(loc(L"running ", L"sürüyor ")) : std::wstring()) + clock_text(end - t->started);
    if (t->buffer.errors()) status += L"     ✗ " + std::to_wstring(t->buffer.errors());
    if (t->buffer.warnings()) status += L"     ⚠ " + std::to_wstring(t->buffer.warnings());
    const float status_w = text_width(status, fmt_ui_small_.Get()) + 12.f;
    draw_text(status, fmt_ui_small_.Get(), D2D1::RectF(right - status_w, hy, right, hy + header_h_),
              t->buffer.errors() ? error : t->state == ConsoleTab::State::Failed ? error : secondary);
    right -= status_w + 8.f;
    draw_text(t->detail, fmt_ui_small_.Get(), D2D1::RectF(pad_, hy, std::max(pad_ + 10.f, right), hy + header_h_), secondary);
    br->SetColor(faint);
    rt->FillRectangle(D2D1::RectF(0, hy + header_h_ - 1.f, width_, hy + header_h_), br);

    // Lines
    const float top_y = tabs_h_ + header_h_ + 4.f;
    const size_t vis = visible_lines();
    const size_t n = t->buffer.size();
    if (t->top > n) t->top = n > vis ? n - vis : 0;
    const float text_right = width_ - scrollbar_w_ - 2.f;
    rt->PushAxisAlignedClip(D2D1::RectF(0, top_y, text_right, height_), D2D1_ANTIALIAS_MODE_ALIASED);
    const size_t sel_a = t->sel_first == SIZE_MAX ? SIZE_MAX : std::min(t->sel_first, t->sel_last);
    const size_t sel_b = t->sel_first == SIZE_MAX ? SIZE_MAX : std::max(t->sel_first, t->sel_last);
    for (size_t row = 0; row < vis && t->top + row < n; ++row) {
        const size_t i = t->top + row;
        const ConsoleLine& l = t->buffer.line(i);
        const float y = top_y + static_cast<float>(row) * line_h_;
        if (sel_a != SIZE_MAX && i >= sel_a && i <= sel_b) {
            br->SetColor(with_alpha(accent, dark_ ? 0.28f : 0.18f));
            rt->FillRectangle(D2D1::RectF(0, y, text_right, y + line_h_), br);
        } else if (l.kind == ConsoleLine::Kind::Error) {
            br->SetColor(with_alpha(error, 0.08f));
            rt->FillRectangle(D2D1::RectF(0, y, text_right, y + line_h_), br);
        }
        D2D1_COLOR_F c = text;
        switch (l.kind) {
            case ConsoleLine::Kind::Error: c = error; break;
            case ConsoleLine::Kind::Warning: c = warning; break;
            case ConsoleLine::Kind::Location: c = rgb(dark_ ? 0x8AB4F8 : 0x1A5FB4); break;
            case ConsoleLine::Kind::Note: c = secondary; break;
            case ConsoleLine::Kind::Normal: break;
        }
        const float x0 = pad_ - x_offset_;
        br->SetColor(c);
        rt->DrawText(l.text.c_str(), static_cast<UINT32>(l.text.size()), fmt_mono_.Get(), D2D1::RectF(x0, y, x0 + 100000.f, y + line_h_), br,
                     D2D1_DRAW_TEXT_OPTIONS_NONE);
        if (hover_line_ == static_cast<int>(i) && !l.file.empty()) {  // clickable: underline
            const float w = std::min(text_right, x0 + static_cast<float>(l.text.size()) * char_w_);
            rt->FillRectangle(D2D1::RectF(x0, y + line_h_ - 2.f, w, y + line_h_ - 1.f), br);
        }
    }
    rt->PopAxisAlignedClip();

    // Scrollbar
    if (n > vis) {
        const float track_top = top_y, track_h = height_ - top_y - 4.f;
        const float thumb_h = std::max(24.f, track_h * static_cast<float>(vis) / static_cast<float>(n));
        const float thumb_y = track_top + (track_h - thumb_h) * static_cast<float>(t->top) / static_cast<float>(n - vis);
        br->SetColor(with_alpha(text, dragging_thumb_ ? 0.35f : 0.18f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(width_ - scrollbar_w_ + 2.f, thumb_y, width_ - 3.f, thumb_y + thumb_h), 3.f, 3.f),
                                 br);
    }

    if (rt->EndDraw() == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
        brush_.Reset();
        target_.Reset();
    }
}

int ConsoleWindow::line_at(float y) const {
    const float top_y = tabs_h_ + header_h_ + 4.f;
    if (y < top_y) return -1;
    const size_t row = static_cast<size_t>((y - top_y) / line_h_);
    const ConsoleTab* t = active_ < tabs_.size() ? tabs_[active_].get() : nullptr;
    if (!t || row >= visible_lines() || t->top + row >= t->buffer.size()) return -1;
    return static_cast<int>(t->top + row);
}

// ---------------------------------------------------------------------------------------------
// Input

void ConsoleWindow::on_click(int x_px, int y_px, bool shift, bool double_click) {
    const float x = static_cast<float>(x_px) * 96.f / dpi_, y = static_cast<float>(y_px) * 96.f / dpi_;
    for (size_t h = hits_.size(); h-- > 0;) {  // close buttons sit on top of their tab
        const HitRect& hit = hits_[h];
        if (!contains(hit.rect, x, y)) continue;
        ConsoleTab* t = hit.index < tabs_.size() ? tabs_[hit.index].get() : nullptr;
        switch (hit.kind) {
            case kHitTab: activate_tab(hit.index); break;
            case kHitClose: close_tab(hit.index); break;
            case kHitStop:
                if (t && cb_.stop) cb_.stop(t->id);
                break;
            case kHitRerun:
                if (t && cb_.rerun) cb_.rerun(t->id);
                break;
            case kHitNext: jump_issue(false); break;
            case kHitCopy: copy_selection(); break;
            default: break;
        }
        return;
    }
    ConsoleTab* t = active();
    if (!t) return;
    if (x >= width_ - scrollbar_w_) {  // scrollbar track: start dragging, the thumb jumps under the cursor
        dragging_thumb_ = true;
        SetCapture(hwnd_);
        drag_offset_ = 0;
        PostMessageW(hwnd_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x_px, y_px));
        return;
    }
    const int line = line_at(y);
    if (line < 0) return;
    const size_t i = static_cast<size_t>(line);
    if (shift && t->sel_first != SIZE_MAX) t->sel_last = i;
    else t->sel_first = t->sel_last = i;
    if (double_click) open_line(*t, i);
    schedule_render();
}

void ConsoleWindow::on_key(WPARAM vk) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    ConsoleTab* t = active();
    const size_t vis = visible_lines();
    auto move_sel = [&](long long delta) {
        if (!t || t->buffer.size() == 0) return;
        const long long cur = t->sel_first == SIZE_MAX ? static_cast<long long>(t->top) : static_cast<long long>(t->sel_last);
        const size_t next = static_cast<size_t>(std::clamp<long long>(cur + delta, 0, static_cast<long long>(t->buffer.size()) - 1));
        if (shift && t->sel_first != SIZE_MAX) t->sel_last = next;
        else t->sel_first = t->sel_last = next;
        ensure_line_visible(*t, next);
        schedule_render();
    };
    switch (vk) {
        case VK_ESCAPE: hide(); break;
        case VK_TAB:
            if (ctrl && !tabs_.empty()) activate_tab((active_ + (shift ? tabs_.size() - 1 : 1)) % tabs_.size());
            break;
        case 'W':
            if (ctrl) close_tab(active_);
            break;
        case 'C':
            if (ctrl) copy_selection();
            break;
        case 'A':
            if (ctrl && t && t->buffer.size()) {
                t->sel_first = 0;
                t->sel_last = t->buffer.size() - 1;
                schedule_render();
            }
            break;
        case VK_F8: jump_issue(shift); break;
        case VK_F5:
            if (t && t->rerunnable && t->state != ConsoleTab::State::Running && cb_.rerun) cb_.rerun(t->id);
            break;
        case VK_RETURN:
            if (t && t->sel_first != SIZE_MAX) open_line(*t, t->sel_last);
            break;
        case VK_UP: move_sel(-1); break;
        case VK_DOWN: move_sel(1); break;
        case VK_PRIOR:
            if (t) scroll_to(*t, t->top > vis ? t->top - vis : 0);
            break;
        case VK_NEXT:
            if (t) scroll_to(*t, t->top + vis);
            break;
        case VK_HOME:
            if (t) scroll_to(*t, 0);
            break;
        case VK_END:
            if (t) scroll_to(*t, SIZE_MAX);
            break;
        case VK_LEFT:
            x_offset_ = std::max(0.f, x_offset_ - char_w_ * 8.f);
            schedule_render();
            break;
        case VK_RIGHT:
            x_offset_ += char_w_ * 8.f;
            schedule_render();
            break;
        default: break;
    }
}

LRESULT ConsoleWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            if (target_) target_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            layout();
            if (ConsoleTab* t = active(); t && t->follow) scroll_to(*t, SIZE_MAX);
            render();
            return 0;
        case WM_DPICHANGED: {
            dpi_ = static_cast<float>(HIWORD(wp));
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            if (target_) target_->SetDpi(dpi_, dpi_);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            EndPaint(hwnd_, &ps);
            render();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (wp == kRenderTimer) {
                render();
            } else if (wp == kClockTimer) {
                const bool any_running = std::any_of(tabs_.begin(), tabs_.end(), [](const auto& t) { return t->state == ConsoleTab::State::Running; });
                if (!any_running) KillTimer(hwnd_, kClockTimer);
                schedule_render();
            }
            return 0;
        case WM_KEYDOWN:
            on_key(wp);
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(hwnd_);
            on_click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), (wp & MK_SHIFT) != 0, false);
            return 0;
        case WM_LBUTTONDBLCLK:
            on_click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false, true);
            return 0;
        case WM_LBUTTONUP:
            if (dragging_thumb_) {
                dragging_thumb_ = false;
                ReleaseCapture();
                schedule_render();
            }
            return 0;
        case WM_MBUTTONUP: {  // middle click closes a tab, like in browsers
            const float x = static_cast<float>(GET_X_LPARAM(lp)) * 96.f / dpi_, y = static_cast<float>(GET_Y_LPARAM(lp)) * 96.f / dpi_;
            for (const auto& h : hits_)
                if (h.kind == kHitTab && contains(h.rect, x, y)) {
                    close_tab(h.index);
                    break;
                }
            return 0;
        }
        case WM_MOUSEMOVE: {
            const float x = static_cast<float>(GET_X_LPARAM(lp)) * 96.f / dpi_, y = static_cast<float>(GET_Y_LPARAM(lp)) * 96.f / dpi_;
            ConsoleTab* t = active();
            if (dragging_thumb_ && t) {
                const float top_y = tabs_h_ + header_h_ + 4.f, track_h = height_ - top_y - 4.f;
                const size_t n = t->buffer.size(), vis = visible_lines();
                if (n > vis) {
                    const float f = std::clamp((y - top_y) / track_h, 0.f, 1.f);
                    scroll_to(*t, static_cast<size_t>(f * static_cast<float>(n - vis)));
                }
                return 0;
            }
            int hit = -1;
            for (size_t h = hits_.size(); h-- > 0;)
                if (contains(hits_[h].rect, x, y)) {
                    hit = static_cast<int>(h);
                    break;
                }
            const int line = hit < 0 ? line_at(y) : -1;
            const int hover_line = line >= 0 && t && !t->buffer.line(static_cast<size_t>(line)).file.empty() ? line : -1;
            if (hit != hover_hit_ || hover_line != hover_line_) {
                hover_hit_ = hit;
                hover_line_ = hover_line;
                schedule_render();
            }
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_hit_ = hover_line_ = -1;
            schedule_render();
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && (hover_hit_ >= 0 || hover_line_ >= 0)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (msg == WM_MOUSEHWHEEL || (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT)) {
                x_offset_ = std::max(0.f, x_offset_ + (msg == WM_MOUSEHWHEEL ? 1.f : -1.f) * static_cast<float>(delta) / 120.f * char_w_ * 8.f);
                schedule_render();
            } else if (ConsoleTab* t = active()) {
                const long long lines = -static_cast<long long>(delta) * 3 / 120;
                scroll_to(*t, static_cast<size_t>(std::max<long long>(0, static_cast<long long>(t->top) + lines)));
            }
            return 0;
        }
        case WM_CLOSE:
            hide();
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------
// Window

void ConsoleWindow::show(bool activate) {
    if (!hwnd_) return;
    if (!visible()) {
        ShowWindow(hwnd_, maximized_on_show_ ? SW_SHOWMAXIMIZED : activate ? SW_SHOW : SW_SHOWNOACTIVATE);
        maximized_on_show_ = false;
    }
    else if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    if (activate) {
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
    }
    render();
}

void ConsoleWindow::hide() {
    if (!visible()) return;
    save_placement();
    ShowWindow(hwnd_, SW_HIDE);
}

void ConsoleWindow::save_placement() {
    if (!hwnd_ || placement_file_.empty()) return;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(hwnd_, &wp)) return;
    std::ofstream out(placement_file_, std::ios::trunc);
    out << wp.rcNormalPosition.left << ' ' << wp.rcNormalPosition.top << ' ' << wp.rcNormalPosition.right << ' ' << wp.rcNormalPosition.bottom
        << ' ' << (wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0) << '\n';
}

void ConsoleWindow::load_placement() {
    std::ifstream in(placement_file_);
    long l = 0, t = 0, r = 0, b = 0;
    int maximized = 0;
    if (!(in >> l >> t >> r >> b >> maximized) || r - l < 200 || b - t < 120) return;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    GetWindowPlacement(hwnd_, &wp);
    wp.rcNormalPosition = RECT{l, t, r, b};
    wp.showCmd = SW_HIDE;
    SetWindowPlacement(hwnd_, &wp);  // Windows moves it onto a monitor if that one is gone
    maximized_on_show_ = maximized != 0;
}

}  // namespace kamil
