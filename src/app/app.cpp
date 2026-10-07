#include "app/app.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <chrono>
#include <fstream>
#include <sstream>

#include "../res/resource.h"
#include "core/hotkey.h"
#include "core/settings_schema.h"
#include "core/text.h"
#include "platform/system_theme.h"

namespace kamil {

namespace {

constexpr int kHotkeyId = 1;

constexpr UINT_PTR kTimerReloadSettings = 1;
constexpr UINT_PTR kTimerSaveUsage = 2;
constexpr UINT_PTR kTimerRescanApps = 3;

constexpr UINT kRescanIntervalMs = 30 * 60 * 1000;

enum MenuId : UINT {
    IDM_SHOW = 100,
    IDM_SETTINGS,
    IDM_CONFIG_DIR,
    IDM_RELOAD,
    IDM_ISSUES,
    IDM_RESCAN,
    IDM_EXIT,
};

// Built-in commands appear in the result list like any other item.
struct BuiltinCommand {
    const wchar_t* id;
    const wchar_t* title;
    const wchar_t* subtitle;
};

constexpr BuiltinCommand kCommands[] = {
    {L"settings", L"Kamil: Ayarları düzenle", L"settings.yaml dosyasını düzenleyicide açar"},
    {L"config-dir", L"Kamil: Ayar klasörünü aç", L"Ayar, öğrenme ve önbellek dosyalarının bulunduğu klasör"},
    {L"reload", L"Kamil: Ayarları yeniden yükle", L"settings.yaml dosyasını yeniden okur"},
    {L"rescan", L"Kamil: Uygulamaları yeniden tara", L"Başlat Menüsü ve Store uygulamalarını yeniden listeler"},
    {L"forget", L"Kamil: Öğrenilenleri sıfırla", L"Sık kullanılanlar ve arama tercihleri silinir"},
    {L"quit", L"Kamil: Çıkış", L"Kamil'i kapatır (kısayol devre dışı kalır)"},
};

int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool write_if_changed(const std::filesystem::path& path, const std::string& content) {
    std::ifstream in(path, std::ios::binary);
    if (in) {
        std::ostringstream ss;
        ss << in.rdbuf();
        if (ss.str() == content) return true;
    }
    in.close();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(out);
}

std::wstring hotkey_display(const std::string& text) { return widen(text); }

}  // namespace

App::App() : store_(builtin_schema()) {}

App::~App() = default;

// ---------------------------------------------------------------------------------------------
// Startup

int App::run(HINSTANCE instance, bool autostart) {
    instance_ = instance;
    paths_ = resolve_paths();

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    if (!create_window()) return 1;
    if (FAILED(LoadIconMetric(instance_, MAKEINTRESOURCEW(IDI_KAMIL), LIM_SMALL, &icon_)))
        icon_ = LoadIconW(nullptr, IDI_APPLICATION);
    tray_.create(hwnd_, icon_, L"Kamil");

    executor_ = std::make_unique<Executor>();
    icon_loader_ = std::make_unique<IconLoader>(hwnd_);

    LauncherWindow::Callbacks cb;
    cb.query_changed = [this](const std::wstring& q) { run_query(q); };
    cb.activate = [this](const Item& item, LaunchMode mode) { on_activate(item, mode); };
    cb.need_icon = [this](const Item& item, uint32_t size) {
        const std::wstring source = item.kind == ItemKind::Command ? paths_.exe.wstring() : item.target;
        icon_loader_->request(item.key, source, size);
    };
    cb.copy_item = [this](const Item& item) {
        copy_to_clipboard(hwnd_, item.path.empty() ? item.target : item.path);
    };
    if (!launcher_.create(instance_, std::move(cb))) {
        MessageBoxW(nullptr, L"Arama penceresi oluşturulamadı (Direct2D/DirectComposition).", L"Kamil", MB_ICONERROR);
        return 1;
    }

    usage_.load(paths_.usage_file());
    apps_ = AppsProvider::load_cache(paths_.apps_cache_file());

    ensure_config_files();
    reload_settings(true);

    watcher_.start(paths_.config_dir, {L"settings.yaml"}, hwnd_);
    apps_provider_.scan_async(hwnd_);
    SetTimer(hwnd_, kTimerRescanApps, kRescanIntervalMs, nullptr);

    if (!autostart && hotkey_ok_)
        notify(L"Kamil çalışıyor", L"Açmak için " + hotkey_display(hotkey_text_) + L" tuşlarına basın.", Tray::Balloon::Info);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (usage_.dirty()) usage_.save(paths_.usage_file());
    UnregisterHotKey(hwnd_, kHotkeyId);
    watcher_.stop();
    tray_.destroy();
    icon_loader_.reset();
    executor_.reset();
    launcher_.destroy();
    return 0;
}

bool App::create_window() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &App::wndproc;
    wc.hInstance = instance_;
    wc.lpszClassName = kAppWindowClass;
    if (!RegisterClassExW(&wc)) return false;
    // A hidden top-level window (not message-only) so that it receives TaskbarCreated and
    // WM_SETTINGCHANGE broadcasts.
    hwnd_ = CreateWindowExW(0, kAppWindowClass, L"Kamil", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance_, this);
    return hwnd_ != nullptr;
}

LRESULT CALLBACK App::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        static_cast<App*>(cs->lpCreateParams)->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------
// Settings

void App::ensure_config_files() {
    const auto& schema = builtin_schema();
    std::error_code ec;
    if (!std::filesystem::exists(paths_.settings_file(), ec)) {
        std::ofstream out(paths_.settings_file(), std::ios::binary);
        const std::string yaml = generate_default_yaml(schema, "./schema/settings.json");
        out.write(yaml.data(), static_cast<std::streamsize>(yaml.size()));
    }
    // Always refresh the JSON Schema so editor completion matches this Kamil version.
    write_if_changed(paths_.schema_file(), generate_json_schema(schema));
}

void App::reload_settings(bool initial) {
    const auto before = store_.current();
    LoadResult r = store_.reload(paths_.settings_file());
    diagnostics_ = r.diagnostics;
    const auto now = store_.current();
    std::vector<std::string> changed = initial ? std::vector<std::string>{} : now->diff(*before);
    if (initial || !changed.empty()) apply_settings(*now, changed, initial);
    report(r);
}

void App::report(const LoadResult& r) {
    if (r.diagnostics.empty()) return;
    size_t errors = 0;
    for (const auto& d : r.diagnostics) errors += d.severity == Diagnostic::Severity::Error;
    const Diagnostic& first = r.diagnostics.front();
    std::wstring text = widen(first.to_string());
    if (r.diagnostics.size() > 1) text += L"\n(+" + std::to_wstring(r.diagnostics.size() - 1) + L" sorun daha)";
    if (r.parse_failed) {
        notify(L"settings.yaml okunamadı", text + L"\nÖnceki ayarlar kullanılıyor.", Tray::Balloon::Error);
    } else if (errors) {
        notify(L"Ayar hatası", text + L"\nHatalı değerler varsayılana döndü.", Tray::Balloon::Warning);
    } else {
        notify(L"Ayar uyarısı", text, Tray::Balloon::Info);
    }
}

void App::apply_settings(const Settings& s, const std::vector<std::string>& changed, bool initial) {
    auto touched = [&](std::string_view prefix) {
        if (initial) return true;
        for (const auto& k : changed)
            if (k.compare(0, prefix.size(), prefix) == 0) return true;
        return false;
    };

    if (touched(keys::kHotkey)) register_hotkey(s.get_string(keys::kHotkey));
    if (touched(keys::kStartWithWindows) && !paths_.portable) set_autostart(s.get_bool(keys::kStartWithWindows), paths_.exe);

    LauncherStyle style;
    style.width = static_cast<int>(s.get_int(keys::kWidth));
    style.max_rows = static_cast<int>(s.get_int(keys::kMaxRows));
    style.font_size = static_cast<int>(s.get_int(keys::kFontSize));
    style.position = static_cast<int>(s.get_int(keys::kPosition));
    style.animations = s.get_bool(keys::kAnimations);
    style.remember_query = s.get_bool(keys::kRememberQuery);
    style.hide_on_focus_loss = s.get_bool(keys::kHideOnFocusLoss);
    style.theme = s.get_string(keys::kTheme);
    style.accent = s.get_string(keys::kAccent);
    launcher_.apply_style(style);

    search_options_.limit = static_cast<size_t>(s.get_int(keys::kMaxResults));
    search_options_.frequent_when_empty = s.get_bool(keys::kShowFrequent);
    search_options_.learning = s.get_bool(keys::kLearning);
    search_options_.aliases.clear();
    for (const auto& obj : s.get_objects(keys::kAliases)) {
        const Value* a = obj.find("alias");
        const Value* t = obj.find("target");
        if (a && t) search_options_.aliases.push_back({fold(trim(std::wstring_view(widen(a->as_string())))),
                                                       fold(trim(std::wstring_view(widen(t->as_string()))))});
    }
    usage_.set_half_life_days(static_cast<double>(s.get_int(keys::kLearningHalfLife)) / 86'400'000.0);

    exclude_patterns_.clear();
    for (const auto& g : s.get_list(keys::kExcludeApps)) exclude_patterns_.push_back(fold(widen(g)));
    rebuild_items();
}

void App::register_hotkey(const std::string& text) {
    UnregisterHotKey(hwnd_, kHotkeyId);
    hotkey_ok_ = false;
    hotkey_text_ = text;
    const auto hk = parse_hotkey(text);
    if (!hk) return;  // already reported by validation
    if (RegisterHotKey(hwnd_, kHotkeyId, hk->mods | MOD_NOREPEAT, hk->vk)) {
        hotkey_ok_ = true;
        tray_.set_tooltip(L"Kamil — " + hotkey_display(text));
        return;
    }
    tray_.set_tooltip(L"Kamil — kısayol kullanılamıyor");
    notify(L"Kısayol kaydedilemedi",
           hotkey_display(text) + L" başka bir uygulama tarafından kullanılıyor (ör. PowerToys Run).\n"
                                  L"Tray menüsünden Ayarları düzenle → general.hotkey ile değiştirin.",
           Tray::Balloon::Error);
}

// ---------------------------------------------------------------------------------------------
// Items and search

void App::set_apps(std::vector<Item> apps) {
    apps_ = std::move(apps);
    AppsProvider::save_cache(paths_.apps_cache_file(), apps_);
    rebuild_items();
}

void App::rebuild_items() {
    std::vector<Item> items;
    items.reserve(apps_.size() + std::size(kCommands));
    for (const auto& c : kCommands) {
        Item it;
        it.kind = ItemKind::Command;
        it.key = std::wstring(L"kamil:") + c.id;
        it.target = c.id;
        it.title = c.title;
        it.subtitle = c.subtitle;
        it.prepare();
        items.push_back(std::move(it));
    }
    for (const auto& app : apps_) {
        bool excluded = false;
        for (const auto& pattern : exclude_patterns_) {
            if (glob_match(pattern, app.title_folded)) {
                excluded = true;
                break;
            }
        }
        if (!excluded) items.push_back(app);
    }
    items_ = std::move(items);
    run_query(launcher_.query());  // the launcher holds pointers into items_: refresh them now
}

void App::run_query(const std::wstring& query) {
    search_options_.now_unix = now_unix();
    launcher_.set_results(search(items_, query, usage_, search_options_));
}

// ---------------------------------------------------------------------------------------------
// Actions

void App::on_activate(const Item& item, LaunchMode mode) {
    if (search_options_.learning) {
        usage_.record(item.key, fold(trim(std::wstring_view(launcher_.query()))), now_unix());
        schedule_usage_save();
    }
    if (item.kind == ItemKind::Command) {
        launcher_.hide();
        run_command(item.target);
        return;
    }
    AllowSetForegroundWindow(ASFW_ANY);  // let the launched program take the foreground
    launcher_.hide();
    const HWND target = hwnd_;
    executor_->post([item, mode, target] {
        const std::wstring err = launch_item(item, mode);
        if (!err.empty())
            post_owned(target, WM_KAMIL_NOTIFY, new Notification{Notification::Level::Error, item.title + L" açılamadı", err});
    });
}

void App::run_command(const std::wstring& id) {
    if (id == L"settings") {
        ensure_config_files();
        const auto file = paths_.settings_file();
        executor_->post([file] { open_in_editor(file); });
    } else if (id == L"config-dir") {
        const auto dir = paths_.config_dir;
        executor_->post([dir] { open_folder(dir); });
    } else if (id == L"reload") {
        reload_settings(false);
        if (diagnostics_.empty()) notify(L"Ayarlar yüklendi", L"settings.yaml sorunsuz okundu.", Tray::Balloon::Info);
    } else if (id == L"rescan") {
        apps_provider_.scan_async(hwnd_);
    } else if (id == L"forget") {
        if (MessageBoxW(hwnd_, L"Öğrenilen tüm sık kullanılanlar ve arama tercihleri silinsin mi?", L"Kamil",
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
            usage_.clear();
            usage_.save(paths_.usage_file());
        }
    } else if (id == L"quit") {
        DestroyWindow(hwnd_);
    }
}

void App::toggle_from_tray() {
    // Clicking the tray icon first deactivates the launcher (which hides it); do not reopen it.
    if (!launcher_.visible() && GetTickCount64() - launcher_.hidden_at() < 300) return;
    launcher_.toggle();
}

void App::show_tray_menu(POINT at) {
    HMENU menu = CreatePopupMenu();
    const std::wstring open = L"Kamil'i aç\t" + hotkey_display(hotkey_text_);
    AppendMenuW(menu, MF_STRING, IDM_SHOW, open.c_str());
    SetMenuDefaultItem(menu, IDM_SHOW, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Ayarları düzenle (settings.yaml)");
    AppendMenuW(menu, MF_STRING, IDM_CONFIG_DIR, L"Ayar klasörünü aç");
    AppendMenuW(menu, MF_STRING, IDM_RELOAD, L"Ayarları yeniden yükle");
    std::wstring issues;
    if (!diagnostics_.empty()) {
        issues = L"Ayar sorunları: " + std::to_wstring(diagnostics_.size()) + L" (ilki: satır " +
                 std::to_wstring(diagnostics_.front().line) + L")";
        AppendMenuW(menu, MF_STRING, IDM_ISSUES, issues.c_str());
    }
    AppendMenuW(menu, MF_STRING, IDM_RESCAN, L"Uygulamaları yeniden tara");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Çıkış");

    SetForegroundWindow(hwnd_);  // required for the menu to close when clicking elsewhere
    const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, at.x, at.y, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (cmd) {
        case IDM_SHOW: launcher_.show(); break;
        case IDM_SETTINGS:
        case IDM_ISSUES: run_command(L"settings"); break;
        case IDM_CONFIG_DIR: run_command(L"config-dir"); break;
        case IDM_RELOAD: run_command(L"reload"); break;
        case IDM_RESCAN: run_command(L"rescan"); break;
        case IDM_EXIT: run_command(L"quit"); break;
        default: break;
    }
}

void App::notify(const std::wstring& title, const std::wstring& text, Tray::Balloon kind) { tray_.balloon(title, text, kind); }

void App::schedule_usage_save() { SetTimer(hwnd_, kTimerSaveUsage, 3000, nullptr); }

// ---------------------------------------------------------------------------------------------
// Messages

LRESULT App::handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == Tray::taskbar_created_message() && msg != 0) {
        tray_.recreate();
        return 0;
    }
    switch (msg) {
        case WM_HOTKEY:
            if (wp == kHotkeyId) launcher_.toggle();
            return 0;
        case WM_KAMIL_SHOW:
            launcher_.show();
            return 0;
        case WM_KAMIL_TRAY:
            switch (LOWORD(lp)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    toggle_from_tray();
                    break;
                case WM_CONTEXTMENU:
                    show_tray_menu(POINT{GET_X_LPARAM(wp), GET_Y_LPARAM(wp)});
                    break;
                default:
                    break;
            }
            return 0;
        case WM_KAMIL_APPS_READY: {
            std::unique_ptr<std::vector<Item>> apps(reinterpret_cast<std::vector<Item>*>(lp));
            // An empty result usually means the shell was not ready yet (early autostart): keep the cache.
            if (!apps->empty() || apps_.empty()) set_apps(std::move(*apps));
            return 0;
        }
        case WM_KAMIL_ICON_READY: {
            std::unique_ptr<IconResult> icon(reinterpret_cast<IconResult*>(lp));
            launcher_.on_icon(*icon);
            return 0;
        }
        case WM_KAMIL_FILE_CHANGED:
            SetTimer(hwnd_, kTimerReloadSettings, 200, nullptr);  // editors write in several steps
            return 0;
        case WM_KAMIL_NOTIFY: {
            std::unique_ptr<Notification> n(reinterpret_cast<Notification*>(lp));
            notify(n->title, n->text,
                   n->level == Notification::Level::Error     ? Tray::Balloon::Error
                   : n->level == Notification::Level::Warning ? Tray::Balloon::Warning
                                                              : Tray::Balloon::Info);
            return 0;
        }
        case WM_TIMER:
            if (wp == kTimerReloadSettings) {
                KillTimer(hwnd_, kTimerReloadSettings);
                reload_settings(false);
            } else if (wp == kTimerSaveUsage) {
                KillTimer(hwnd_, kTimerSaveUsage);
                usage_.save(paths_.usage_file());
            } else if (wp == kTimerRescanApps) {
                apps_provider_.scan_async(hwnd_);
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && std::wstring_view(reinterpret_cast<const wchar_t*>(lp)) == L"ImmersiveColorSet")
                launcher_.on_system_theme_changed();
            return 0;
        case WM_DWMCOLORIZATIONCOLORCHANGED:  // accent color changed
            launcher_.on_system_theme_changed();
            return 0;
        case WM_ENDSESSION:
            if (wp && usage_.dirty()) usage_.save(paths_.usage_file());
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace kamil
