#include "app/app.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <sstream>

#include "../res/resource.h"
#include "app/action_item.h"
#include "core/hotkey.h"
#include "core/i18n.h"
#include "core/log.h"
#include "core/settings_schema.h"
#include "core/text.h"
#include "platform/process.h"
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
    IDM_JOBS,
    IDM_CANCEL_JOBS,
    IDM_LOG,
    IDM_EXIT,
};

// Built-in commands appear in the result list like any other item. Both languages' titles are
// searchable keywords, so "settings" and "ayarlar" find the same command.
struct BuiltinCommand {
    const wchar_t* id;
    const wchar_t* title_en;
    const wchar_t* title_tr;
    const wchar_t* sub_en;
    const wchar_t* sub_tr;
};

constexpr BuiltinCommand kCommands[] = {
    {L"settings", L"Kamil: Edit settings", L"Kamil: Ayarları düzenle", L"Opens settings.yaml in the editor", L"settings.yaml dosyasını düzenleyicide açar"},
    {L"config-dir", L"Kamil: Open settings folder", L"Kamil: Ayar klasörünü aç", L"Settings, learning data and cache files",
     L"Ayar, öğrenme ve önbellek dosyalarının bulunduğu klasör"},
    {L"reload", L"Kamil: Reload settings", L"Kamil: Ayarları yeniden yükle", L"Reads settings.yaml again", L"settings.yaml dosyasını yeniden okur"},
    {L"rescan", L"Kamil: Rescan", L"Kamil: Yeniden tara", L"Apps, git repositories and files in the search folders",
     L"Uygulamalar, git depoları ve aranacak klasörlerdeki dosyalar"},
    {L"jobs", L"Kamil: VS jobs", L"Kamil: VS işleri", L"Running and queued Visual Studio jobs; cancel them here",
     L"Çalışan ve sırada bekleyen Visual Studio işleri; buradan iptal edilir"},
    {L"forget", L"Kamil: Forget what was learned", L"Kamil: Öğrenilenleri sıfırla", L"Clears frequently used items and search preferences",
     L"Sık kullanılanlar ve arama tercihleri silinir"},
    {L"vs-diagnose", L"Kamil: Test VS connection", L"Kamil: VS bağlantısını test et",
     L"Report: Visual Studio 2022/2026 instances, Output panes, CMake command names, administrator rights",
     L"Rapor: açık VS 2022/2026 örnekleri, Output bölmeleri, CMake komut adları, yönetici hakları"},
    {L"log", L"Kamil: Open log", L"Kamil: Günlüğü aç", L"kamil.log: what Kamil did, especially with Visual Studio",
     L"kamil.log: Kamil'in yaptıkları, özellikle Visual Studio ile"},
    {L"quit", L"Kamil: Quit", L"Kamil: Çıkış", L"Closes Kamil (the hotkey stops working)", L"Kamil'i kapatır (kısayol devre dışı kalır)"},
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

std::wstring vs_title(const std::string& which) { return which == "vs2022" ? L"Visual Studio 2022" : L"Visual Studio 2026"; }

}  // namespace

App::App() : store_(builtin_schema()) {}

App::~App() = default;

// ---------------------------------------------------------------------------------------------
// Startup

int App::run(HINSTANCE instance, bool autostart) {
    instance_ = instance;
    paths_ = resolve_paths();
    set_language(system_language());  // until settings.yaml is read; also the language of a new settings.yaml
    log_open(paths_.cache_dir / "kamil.log");
    log_line("---- Kamil started");

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    if (!create_window()) return 1;
    if (FAILED(LoadIconMetric(instance_, MAKEINTRESOURCEW(IDI_KAMIL), LIM_SMALL, &icon_)))
        icon_ = LoadIconW(nullptr, IDI_APPLICATION);
    tray_.create(hwnd_, icon_, L"Kamil");

    executor_ = std::make_unique<Executor>();
    icon_loader_ = std::make_unique<IconLoader>(hwnd_);
    git_ = std::make_unique<GitService>(hwnd_);
    vs_ = std::make_unique<VsBridge>(hwnd_);
    project_state_.load(paths_.projects_file());
    {
        const HWND target = hwnd_;
        executor_->post([target] { post_owned(target, WM_KAMIL_TOOLS_READY, new Tools(discover_tools())); });
    }

    LauncherWindow::Callbacks cb;
    cb.query_changed = [this](const std::wstring& q) { run_query(q); };
    cb.activate = [this](const Item& item, LaunchMode mode) { on_activate(item, mode); };
    cb.need_icon = [this](const Item& item, uint32_t size) {
        std::wstring source = item.icon_source;
        if (source.empty()) source = item.kind == ItemKind::Command ? paths_.exe.wstring() : item.target;
        icon_loader_->request(item.icon_cache_key(), source, size);
    };
    cb.copy_item = [this](const Item& item) {
        copy_to_clipboard(hwnd_, item.path.empty() ? item.target : item.path);
    };
    cb.actions_for = [this](const Item& item) { return actions_for(item); };
    cb.run_action = [this](const Item& parent, const Item& action) { run_action(parent, action.target); };
    cb.footer = [this](const Item* selected, const Item* parent) { return footer_for(selected, parent); };
    cb.quick_action = [this](const Item& item, wchar_t key) { return quick_action(item, key); };
    if (!launcher_.create(instance_, std::move(cb))) {
        MessageBoxW(nullptr, loc(L"The search window could not be created (Direct2D/DirectComposition).",
                                 L"Arama penceresi oluşturulamadı (Direct2D/DirectComposition)."),
                    L"Kamil", MB_ICONERROR);
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
        notify(loc(L"Kamil is running", L"Kamil çalışıyor"),
               fmt(loc(L"Press {} to open it.", L"Açmak için {} tuşlarına basın."), hotkey_display(hotkey_text_)), Tray::Balloon::Info);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (usage_.dirty()) usage_.save(paths_.usage_file());
    UnregisterHotKey(hwnd_, kHotkeyId);
    watcher_.stop();
    tray_.destroy();
    if (file_scan_.joinable()) {
        file_scan_cancel_ = true;
        file_scan_.join();
    }
    vs_.reset();
    git_.reset();
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
    if (r.diagnostics.size() > 1) text += fmt(loc(L"\n(+{} more)", L"\n(+{} sorun daha)"), std::to_wstring(r.diagnostics.size() - 1));
    for (const auto& d : r.diagnostics) log_line("[settings] " + d.to_string());
    if (r.parse_failed) {
        notify(loc(L"settings.yaml could not be read", L"settings.yaml okunamadı"),
               text + loc(L"\nThe previous settings stay in use.", L"\nÖnceki ayarlar kullanılıyor."), Tray::Balloon::Error);
    } else if (errors) {
        notify(loc(L"Settings error", L"Ayar hatası"), text + loc(L"\nInvalid values were reset to their defaults.", L"\nHatalı değerler varsayılana döndü."),
               Tray::Balloon::Warning);
    } else {
        notify(loc(L"Settings warning", L"Ayar uyarısı"), text, Tray::Balloon::Info);
    }
}

void App::apply_settings(const Settings& s, const std::vector<std::string>& changed, bool initial) {
    auto touched = [&](std::string_view prefix) {
        if (initial) return true;
        for (const auto& k : changed)
            if (k.compare(0, prefix.size(), prefix) == 0) return true;
        return false;
    };

    const Lang lang = parse_language(s.get_string(keys::kLanguage));
    const bool language_changed = lang != language();
    set_language(lang);
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
    style.footer = s.get_bool(keys::kFooter);
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
    search_options_.folder_boosts.clear();
    for (const auto& obj : s.get_objects(keys::kFolderPriority)) {
        const Value* p = obj.find("path");
        const Value* prio = obj.find("priority");
        if (!p || !prio) continue;
        const std::wstring raw = widen(p->as_string());
        std::wstring expanded(32768, L'\0');
        const DWORD n = ExpandEnvironmentStringsW(raw.c_str(), expanded.data(), static_cast<DWORD>(expanded.size()));
        expanded.resize(n > 0 && n <= expanded.size() ? n - 1 : 0);
        search_options_.folder_boosts.push_back({normalize_folder(expanded.empty() ? raw : expanded), static_cast<int>(prio->as_int())});
    }
    for (const auto& obj : s.get_objects(keys::kSearchFolders)) {
        const Value* p = obj.find("path");
        const Value* prio = obj.find("priority");
        if (p && prio && prio->as_int() != 0)
            search_options_.folder_boosts.push_back({normalize_folder(expand_env(widen(p->as_string()))), static_cast<int>(prio->as_int())});
    }
    if (initial || touched(keys::kSearchFolders) || touched(keys::kFolderPriority) || touched(keys::kExcludeDirs) || touched(keys::kMaxFiles))
        scan_files();
    usage_.set_half_life_days(static_cast<double>(s.get_int(keys::kLearningHalfLife)) / 86'400'000.0);

    exclude_patterns_.clear();
    for (const auto& g : s.get_list(keys::kExcludeApps)) exclude_patterns_.push_back(fold(widen(g)));
    if (git_) git_->configure(tools_.git, s.get_bool(keys::kGitStatus));
    if (touched(keys::kProjectRoots) || touched(keys::kScanDepth) || touched(keys::kScanExclude)) scan_repos();
    if (!initial && touched(keys::kDevenvPath)) {
        const HWND target = hwnd_;
        const std::wstring devenv = expand_env(widen(s.get_string(keys::kDevenvPath)));
        executor_->post([target, devenv] { post_owned(target, WM_KAMIL_TOOLS_READY, new Tools(discover_tools(devenv))); });
    }
    if (language_changed) {
        invalidate_project({});
        if (hotkey_ok_) tray_.set_tooltip(L"Kamil — " + hotkey_display(hotkey_text_));
        else register_hotkey(s.get_string(keys::kHotkey));  // re-sends its message in the new language
    }
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
    log_line("[app] hotkey " + text + " is taken");
    tray_.set_tooltip(loc(L"Kamil — hotkey unavailable", L"Kamil — kısayol kullanılamıyor"));
    notify(loc(L"Hotkey could not be registered", L"Kısayol kaydedilemedi"),
           fmt(loc(L"{} is used by another application (e.g. PowerToys Run).\nChange it with Edit settings → general.hotkey in the tray menu.",
                   L"{} başka bir uygulama tarafından kullanılıyor (ör. PowerToys Run).\nTray menüsünden Ayarları düzenle → general.hotkey ile değiştirin."),
               hotkey_display(text)),
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
        const bool tr = language() == Lang::Tr;
        it.title = tr ? c.title_tr : c.title_en;
        it.subtitle = tr ? c.sub_tr : c.sub_en;
        // The other language's title as a search keyword ("ayarlar" also finds "Edit settings").
        it.keywords = tr ? c.title_en : c.title_tr;
        if (it.target == L"jobs") it.keywords += L" queue cancel kuyruk sıra iptal";
        it.prepare();
        items.push_back(std::move(it));
    }
    for (const auto& repo : repos_) {
        Item it;
        it.kind = ItemKind::Repo;
        it.key = L"repo:" + repo.path;
        it.title = repo.name;
        it.subtitle = (repo.branch.empty() ? std::wstring() : L"⎇ " + widen(repo.branch) + L"   ") + (repo.cmake ? L"CMake   " : L"") + repo.path;
        it.target = repo.path;
        it.path = repo.path;
        it.icon_source = repo.path;  // the folder's own shell icon
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
    std::vector<Hit> hits = search(items_, query, usage_, search_options_);
    add_file_hits(query, hits);
    launcher_.set_results(std::move(hits));
}

// ---------------------------------------------------------------------------------------------
// Actions

void App::on_activate(const Item& item, LaunchMode mode) {
    if (search_options_.learning) {
        usage_.record(item.key, fold(trim(std::wstring_view(launcher_.query()))), now_unix());
        schedule_usage_save();
    }
    if (item.kind == ItemKind::Command) {
        if (item.target == L"jobs") {  // a list inside the launcher: stay open
            show_jobs();
            return;
        }
        launcher_.hide();
        run_command(item.target);
        return;
    }
    if (item.kind == ItemKind::Repo) {
        const std::wstring action = mode == LaunchMode::OpenLocation ? L"explorer" : default_repo_action();
        run_action(item, action);
        return;
    }
    if (item.kind == ItemKind::File || item.kind == ItemKind::Folder) {
        open_file(item, mode);
        return;
    }
    AllowSetForegroundWindow(ASFW_ANY);  // let the launched program take the foreground
    launcher_.hide();
    const HWND target = hwnd_;
    executor_->post([item, mode, target] {
        const std::wstring err = launch_item(item, mode);
        if (!err.empty())
            post_owned(target, WM_KAMIL_NOTIFY,
                       new Notification{Notification::Level::Error, fmt(loc(L"{} could not be opened", L"{} açılamadı"), item.title), err});
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
        if (diagnostics_.empty())
            notify(loc(L"Settings loaded", L"Ayarlar yüklendi"), loc(L"settings.yaml was read without problems.", L"settings.yaml sorunsuz okundu."),
                   Tray::Balloon::Info);
    } else if (id == L"rescan") {
        apps_provider_.scan_async(hwnd_);
        scan_repos();
        scan_files();
    } else if (id == L"forget") {
        if (MessageBoxW(hwnd_,
                        loc(L"Forget all learned favourites and search preferences?", L"Öğrenilen tüm sık kullanılanlar ve arama tercihleri silinsin mi?"),
                        L"Kamil",
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
            usage_.clear();
            usage_.save(paths_.usage_file());
        }
    } else if (id == L"vs-diagnose") {
        VsJob job;
        job.kind = VsJob::Kind::Diagnose;
        job.report_file = paths_.vs_report_file();
        job.tools_report = tools_ready_ ? tools_.report : std::wstring(L"(tools are still being discovered)\n");
        vs_->submit(std::move(job));
        notify(loc(L"Testing the VS connection", L"VS bağlantısı test ediliyor"),
               loc(L"The report opens in a few seconds.", L"Rapor birkaç saniye içinde açılacak."), Tray::Balloon::Info);
    } else if (id == L"jobs") {
        show_jobs();
    } else if (id == L"log") {
        const auto file = log_file();
        executor_->post([file] { open_in_editor(file); });
    } else if (id == L"quit") {
        DestroyWindow(hwnd_);
    }
}

// ---------------------------------------------------------------------------------------------
// Repositories, actions, footer

void App::scan_repos() {
    const auto s = store_.current();
    std::vector<std::filesystem::path> roots;
    for (const auto& r : s->get_list(keys::kProjectRoots)) {
        const std::wstring raw = widen(r);
        std::wstring expanded(32768, L'\0');
        const DWORD n = ExpandEnvironmentStringsW(raw.c_str(), expanded.data(), static_cast<DWORD>(expanded.size()));
        expanded.resize(n > 0 && n <= expanded.size() ? n - 1 : 0);
        roots.emplace_back(expanded.empty() ? raw : expanded);
    }
    RepoScanOptions opt;
    opt.max_depth = static_cast<size_t>(s->get_int(keys::kScanDepth));
    opt.include_cmake_roots = true;
    for (const auto& g : s->get_list(keys::kScanExclude)) opt.exclude.push_back(fold(widen(g)));
    if (roots.empty()) {
        if (!repos_.empty()) {
            repos_.clear();
            rebuild_items();
        }
        return;
    }
    repo_provider_.scan_async(std::move(roots), std::move(opt), hwnd_);
}

std::wstring App::default_repo_action() const {
    const std::string a = store_.current()->get_string(keys::kRepoAction);
    if (a == "code") return L"code";
    if (a == "explorer") return L"explorer";
    if (a == "terminal") return L"terminal";
    return L"vs";
}

std::vector<Item> App::actions_for(const Item& item) const {
    std::vector<Item> actions;
    const std::string vs = store_.current()->get_string(keys::kDefaultVs);
    switch (item.kind) {
        case ItemKind::Repo: {
            const bool project = is_project(item.path);
            std::vector<Item> project_list;
            if (project) project_list = const_cast<App*>(this)->project_actions(item);
            const std::wstring& devenv = tools_.devenv(vs);
            const std::wstring other_vs = vs == "vs2022" ? tools_.devenv_2026 : tools_.devenv_2022;
            std::vector<Item> list;
            // Always offered: when Visual Studio was not found, the subtitle says why.
            list.push_back(make_action(L"vs", fmt(loc(L"Open in {}", L"{}'da aç"), vs_title(!devenv.empty() && devenv == tools_.devenv_2022 ? "vs2022" : (devenv.empty() ? vs : "vs2026"))),
                                       devenv, kGlyphFolder,
                                       devenv.empty() ? std::wstring(loc(L"⚠ Visual Studio not found: set dev.devenv_path, or see Kamil: Test VS connection",
                                                                         L"⚠ Visual Studio bulunamadı: dev.devenv_path ayarlayın ya da Kamil: VS bağlantısını test et"))
                                                      : L"Open Folder (CMake)"));
            if (!other_vs.empty() && other_vs != devenv)
                list.push_back(make_action(L"vs-other", fmt(loc(L"Open in {}", L"{}'da aç"), vs_title(other_vs == tools_.devenv_2022 ? "vs2022" : "vs2026")),
                                           other_vs, 0, L"Open Folder (CMake)"));
            if (!tools_.code.empty()) list.push_back(make_action(L"code", loc(L"Open in VS Code", L"VS Code'da aç"), tools_.code, 0));
            list.push_back(make_action(L"explorer", loc(L"Open in Explorer", L"Gezgin'de aç"), tools_.explorer, 0));
            const std::string term = store_.current()->get_string(keys::kTerminal);
            const bool use_wt = term == "wt" && !tools_.wt.empty();
            const bool use_bash = term == "git-bash" && !tools_.git_bash.empty();
            const std::wstring term_exe = use_wt     ? tools_.wt
                                          : use_bash ? tools_.git_bash
                                          : term == "powershell" ? tools_.powershell
                                                                 : tools_.cmd;
            const wchar_t* term_name = use_wt ? L"Windows Terminal" : use_bash ? L"Git Bash" : term == "powershell" ? L"PowerShell" : loc(L"Command Prompt", L"Komut İstemi");
            list.push_back(make_action(L"terminal", loc(L"Open in terminal", L"Terminalde aç"), term_exe, kGlyphTerminal, term_name));
            if (!tools_.git_bash.empty()) list.push_back(make_action(L"git-bash", loc(L"Open in Git Bash", L"Git Bash'te aç"), tools_.git_bash, 0));
            if (!tools_.git_gui.empty()) list.push_back(make_action(L"git-gui", L"Git GUI", tools_.git_gui, 0));
            list.push_back(make_action(L"copy-path", loc(L"Copy path", L"Yolu kopyala"), {}, kGlyphCopy, item.path));
            list.push_back(make_action(L"copy-branch", loc(L"Copy branch name", L"Dal adını kopyala"), {}, kGlyphBranch));
            // The default Enter action first.
            const std::wstring def = default_repo_action();
            std::vector<Item> sorted;
            for (auto& a : list)
                if (a.target == def) sorted.push_back(a);
            for (auto& a : list)
                if (a.target != def) sorted.push_back(std::move(a));
            if (project) {
                // Jobs to cancel, Debug, Build, Run, then the editors (VS / VS Code), then the rest.
                size_t head = 0;
                while (head < project_list.size() && project_list[head].target.rfind(L"cancel-job:", 0) == 0) ++head;
                head = std::min(project_list.size(), head + 3);
                for (size_t i = 0; i < head; ++i) actions.push_back(std::move(project_list[i]));
                for (auto& a : sorted)
                    if (a.target == L"vs" || a.target == L"code") actions.push_back(a);
                for (size_t i = head; i < project_list.size(); ++i) actions.push_back(std::move(project_list[i]));
                for (auto& a : sorted)
                    if (a.target != L"vs" && a.target != L"code") actions.push_back(std::move(a));
            } else {
                for (auto& a : sorted) actions.push_back(std::move(a));
            }
            break;
        }
        case ItemKind::Command:
            actions.push_back(make_action(L"run", loc(L"Run", L"Çalıştır"), {}, kGlyphPlay, item.subtitle));
            break;
        case ItemKind::File:
        case ItemKind::Folder:
            actions = file_actions(item);
            break;
        default: {
            Item open = make_action(L"open", loc(L"Open", L"Aç"), item.target, 0);
            open.key = item.key;  // reuse the item's own icon
            actions.push_back(std::move(open));
            actions.push_back(make_action(L"admin", loc(L"Run as administrator", L"Yönetici olarak çalıştır"), {}, kGlyphAdmin));
            if (!item.path.empty()) {
                actions.push_back(make_action(L"location", loc(L"Show file location", L"Dosya konumunu göster"), {}, kGlyphFolder, item.path));
                actions.push_back(make_action(L"copy-path", loc(L"Copy path", L"Yolu kopyala"), {}, kGlyphCopy, item.path));
            }
            break;
        }
    }
    return actions;
}

void App::run_action(const Item& parent, const std::wstring& action) {
    if (search_options_.learning) {
        usage_.record(parent.key, {}, now_unix());
        schedule_usage_save();
    }
    if (run_job_action(parent, action)) return;
    if (parent.kind == ItemKind::Command) {
        launcher_.hide();
        run_command(parent.target);
        return;
    }
    if (run_file_action(parent, action)) return;
    if (parent.kind == ItemKind::Repo && run_project_action(parent, action)) return;
    if (action == L"copy-path") {
        copy_to_clipboard(hwnd_, parent.path.empty() ? parent.target : parent.path);
        launcher_.hide();
        return;
    }
    if (action == L"copy-branch") {
        auto it = repo_states_.find(parent.path);
        std::string branch = it != repo_states_.end() ? it->second.head.display() : std::string();
        if (branch.empty())
            for (const auto& r : repos_)
                if (r.path == parent.path) branch = r.branch;
        copy_to_clipboard(hwnd_, widen(branch));
        launcher_.hide();
        return;
    }
    if (parent.kind != ItemKind::Repo) {
        const LaunchMode mode = action == L"admin" ? LaunchMode::Admin : action == L"location" ? LaunchMode::OpenLocation : LaunchMode::Normal;
        on_activate(parent, mode);
        return;
    }

    // Repository actions: build the program + arguments, then start it off the UI thread.
    const std::wstring& dir = parent.path;
    const std::string vs = store_.current()->get_string(keys::kDefaultVs);
    std::wstring exe, args;
    if (action == L"vs" || action == L"vs-other") {
        exe = action == L"vs" ? tools_.devenv(vs) : (vs == "vs2022" ? tools_.devenv_2026 : tools_.devenv_2022);
        args = quote_arg(dir);
        if (exe.empty()) {
            notify(loc(L"Visual Studio not found", L"Visual Studio bulunamadı"),
                   loc(L"Neither vswhere nor the standard install folders have Visual Studio 2022/2026. Set dev.devenv_path in settings.yaml.",
                       L"vswhere ve standart kurulum klasörlerinde Visual Studio 2022/2026 yok. settings.yaml → dev.devenv_path ile yolu verin."),
                   Tray::Balloon::Warning);
            return;
        }
    } else if (action == L"code") {
        exe = tools_.code;
        args = quote_arg(dir);
    } else if (action == L"explorer") {
        exe = tools_.explorer;
        args = quote_arg(dir);
    } else if (action == L"git-bash") {
        exe = tools_.git_bash;
        args = L"--cd=" + quote_arg(dir);
    } else if (action == L"git-gui") {
        exe = tools_.git_gui;
    } else if (action == L"terminal") {
        const std::string term = store_.current()->get_string(keys::kTerminal);
        if (term == "wt" && !tools_.wt.empty()) {
            exe = tools_.wt;
            args = L"-d " + quote_arg(dir);
        } else if (term == "git-bash" && !tools_.git_bash.empty()) {
            exe = tools_.git_bash;
            args = L"--cd=" + quote_arg(dir);
        } else if (term == "powershell") {
            exe = tools_.powershell;
            args = L"-NoExit";
        } else {
            exe = tools_.cmd;
            args = L"/K";
        }
    }
    if (exe.empty()) {
        notify(loc(L"Program not found", L"Program bulunamadı"),
               loc(L"The program this action needs was not found on this computer.", L"Bu eylem için gereken program bu bilgisayarda bulunamadı."),
               Tray::Balloon::Warning);
        return;
    }
    AllowSetForegroundWindow(ASFW_ANY);
    launcher_.hide();
    const HWND target = hwnd_;
    const std::wstring title = parent.title;
    executor_->post([exe, args, dir, title, target] {
        const std::wstring err = run_program(exe, args, dir);
        if (!err.empty())
            post_owned(target, WM_KAMIL_NOTIFY, new Notification{Notification::Level::Error, fmt(loc(L"{} could not be opened", L"{} açılamadı"), title), err});
    });
}

std::wstring App::repo_context(const std::wstring& path) {
    const bool git = std::any_of(repos_.begin(), repos_.end(), [&](const RepoInfo& r) { return r.path == path && r.git; });
    if (!git) return {};
    auto it = repo_states_.find(path);
    const bool fresh = it != repo_states_.end() && GetTickCount64() - it->second.tick < 5000;
    if (!fresh && git_) git_->request(path);
    std::string branch;
    if (it != repo_states_.end()) branch = it->second.head.display();
    if (branch.empty())
        for (const auto& r : repos_)
            if (r.path == path) branch = r.branch;
    std::wstring out = branch.empty() ? std::wstring() : L"⎇ " + widen(branch);
    if (it == repo_states_.end() || !it->second.status.valid) return out + (fresh ? L"" : L"   …");
    const GitStatus& st = it->second.status;
    if (st.conflicts) out += fmt(loc(L"   ⚠ {} conflicts", L"   ⚠ {} çakışma"), std::to_wstring(st.conflicts));
    if (st.changes() - st.conflicts > 0) out += fmt(loc(L"   ● {} changes", L"   ● {} değişiklik"), std::to_wstring(st.changes() - st.conflicts));
    if (st.changes() == 0) out += loc(L"   ✓ clean", L"   ✓ temiz");
    if (st.ahead) out += L"   ↑" + std::to_wstring(st.ahead);
    if (st.behind) out += L"   ↓" + std::to_wstring(st.behind);
    return out;
}

Footer App::footer_for(const Item* selected, const Item* parent) {
    Footer f;
    // A running (or just finished) VS job is always shown first: "⚒ Build sürüyor… 0:12".
    const std::wstring job = job_status();
    auto with_job = [&](std::wstring context) {
        if (job.empty()) return context;
        return context.empty() ? job : job + L"     " + context;
    };
    auto repo_line = [&](const Item& repo) {
        std::wstring line = repo_context(repo.path);
        if (is_project(repo.path)) {
            const std::wstring project = project_context(repo.path);
            if (!project.empty()) line += (line.empty() ? L"" : L"   ") + project;
        }
        return line;
    };
    if (parent) {
        f.left = with_job(parent->kind == ItemKind::Repo ? parent->title + L"   " + repo_line(*parent) : parent->title);
        f.right = loc(L"Enter select · Tab complete · Esc back", L"Enter seç · Tab tamamla · Esc geri");
        return f;
    }
    if (!selected) {
        f.left = with_job({});
        f.right = loc(L"Esc close", L"Esc kapat");
        return f;
    }
    switch (selected->kind) {
        case ItemKind::Repo: {
            f.left = with_job(repo_line(*selected));
            const std::wstring def = default_repo_action();
            const std::wstring what = def == L"code" ? L"VS Code" : def == L"explorer" ? loc(L"Explorer", L"Gezgin") : def == L"terminal" ? L"Terminal"
                                                                                                                                    : L"Visual Studio";
            f.right = is_project(selected->path) ? std::wstring(loc(L"Alt+D debug · Alt+B build · Alt+R run · Ctrl+K", L"Alt+D debug · Alt+B build · Alt+R çalıştır · Ctrl+K"))
                                                 : fmt(loc(L"Enter {} · Ctrl+K actions · Tab", L"Enter {} · Ctrl+K eylemler · Tab"), what);
            break;
        }
        case ItemKind::Command:
            f.left = with_job(selected->subtitle);
            f.right = loc(L"Enter run", L"Enter çalıştır");
            break;
        case ItemKind::File: {
            f.left = with_job(selected->subtitle);
            std::wstring ext = std::filesystem::path(selected->path).extension().wstring();
            for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
            if (is_script_extension(ext))
                f.right = store_.current()->get_string(keys::kScriptAction) == "edit"
                              ? loc(L"Enter edit · Alt+R run · Ctrl+K", L"Enter düzenle · Alt+R çalıştır · Ctrl+K")
                              : loc(L"Enter run · Alt+E edit · Ctrl+K", L"Enter çalıştır · Alt+E düzenle · Ctrl+K");
            else
                f.right = loc(L"Enter open · Alt+E edit · Ctrl+K", L"Enter aç · Alt+E düzenle · Ctrl+K");
            break;
        }
        case ItemKind::Folder:
            f.left = with_job(selected->subtitle);
            f.right = loc(L"Enter Explorer · Ctrl+K actions", L"Enter Gezgin · Ctrl+K eylemler");
            break;
        default:
            f.left = with_job(selected->path);
            f.right = loc(L"Enter open · Ctrl+K actions · Tab", L"Enter aç · Ctrl+K eylemler · Tab");
            break;
    }
    return f;
}

void App::toggle_from_tray() {
    // Clicking the tray icon first deactivates the launcher (which hides it); do not reopen it.
    if (!launcher_.visible() && GetTickCount64() - launcher_.hidden_at() < 300) return;
    launcher_.toggle();
}

void App::show_tray_menu(POINT at) {
    HMENU menu = CreatePopupMenu();
    const std::wstring open = std::wstring(loc(L"Open Kamil\t", L"Kamil'i aç\t")) + hotkey_display(hotkey_text_);
    AppendMenuW(menu, MF_STRING, IDM_SHOW, open.c_str());
    SetMenuDefaultItem(menu, IDM_SHOW, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    std::wstring jobs;
    if (!jobs_.empty()) {
        jobs = fmt(loc(L"VS jobs: {} (show / cancel)…", L"VS işleri: {} (göster / iptal)…"), std::to_wstring(jobs_.size()));
        AppendMenuW(menu, MF_STRING, IDM_JOBS, jobs.c_str());
        AppendMenuW(menu, MF_STRING, IDM_CANCEL_JOBS, loc(L"Cancel all VS jobs", L"Tüm VS işlerini iptal et"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, loc(L"Edit settings (settings.yaml)", L"Ayarları düzenle (settings.yaml)"));
    AppendMenuW(menu, MF_STRING, IDM_CONFIG_DIR, loc(L"Open settings folder", L"Ayar klasörünü aç"));
    AppendMenuW(menu, MF_STRING, IDM_RELOAD, loc(L"Reload settings", L"Ayarları yeniden yükle"));
    std::wstring issues;
    if (!diagnostics_.empty()) {
        issues = fmt(loc(L"Settings problems: {} (first: line {})", L"Ayar sorunları: {} (ilki: satır {})"), std::to_wstring(diagnostics_.size()),
                     std::to_wstring(diagnostics_.front().line));
        AppendMenuW(menu, MF_STRING, IDM_ISSUES, issues.c_str());
    }
    AppendMenuW(menu, MF_STRING, IDM_RESCAN, loc(L"Rescan apps and files", L"Uygulamaları ve dosyaları yeniden tara"));
    AppendMenuW(menu, MF_STRING, IDM_LOG, loc(L"Open log", L"Günlüğü aç"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, loc(L"Quit", L"Çıkış"));

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
        case IDM_JOBS: show_jobs(); break;
        case IDM_CANCEL_JOBS: {
            Item none;
            run_job_action(none, L"cancel-all-jobs");
            break;
        }
        case IDM_LOG: run_command(L"log"); break;
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
            if (wp == kHotkeyId) {
                // New scripts appear without waiting for the periodic rescan.
                if (!launcher_.visible() && GetTickCount64() - files_scanned_at_ > 120'000) scan_files();
                launcher_.toggle();
            }
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
        case WM_KAMIL_TOOLS_READY: {
            std::unique_ptr<Tools> tools(reinterpret_cast<Tools*>(lp));
            const std::wstring override_path = expand_env(widen(store_.current()->get_string(keys::kDevenvPath)));
            if (!override_path.empty() && tools->report.find(L"dev.devenv_path") == std::wstring::npos) {
                // The startup discovery ran before the settings were read: redo it with the override.
                const HWND target = hwnd_;
                executor_->post([target, override_path] { post_owned(target, WM_KAMIL_TOOLS_READY, new Tools(discover_tools(override_path))); });
            }
            tools_ = std::move(*tools);
            tools_ready_ = true;
            log_line(L"[tools]\n" + tools_.report);
            invalidate_project({});
            if (git_) git_->configure(tools_.git, store_.current()->get_bool(keys::kGitStatus));
            return 0;
        }
        case WM_KAMIL_REPOS_READY: {
            std::unique_ptr<std::vector<RepoInfo>> repos(reinterpret_cast<std::vector<RepoInfo>*>(lp));
            repos_ = std::move(*repos);
            rebuild_items();  // safe in the action panel too: it owns copies of what it shows
            return 0;
        }
        case WM_KAMIL_GIT_STATUS: {
            std::unique_ptr<RepoState> state(reinterpret_cast<RepoState*>(lp));
            std::wstring path = state->path;
            repo_states_[path] = std::move(*state);
            launcher_.refresh_footer();
            return 0;
        }
        case WM_KAMIL_FILES_READY:
            on_files_ready(std::unique_ptr<FileIndex>(reinterpret_cast<FileIndex*>(lp)));
            return 0;
        case WM_KAMIL_VS_EVENT: {
            std::unique_ptr<VsEvent> e(reinterpret_cast<VsEvent*>(lp));
            on_vs_event(*e);
            return 0;
        }
        case WM_DEVICECHANGE:
            com_ports_stale_ = true;  // a serial adapter may have been plugged in or out
            invalidate_project({});
            return TRUE;
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
            } else if (wp == 4) {  // job clock in the footer
                launcher_.refresh_footer();
            } else if (wp == kTimerRescanApps) {
                apps_provider_.scan_async(hwnd_);
                scan_repos();
                scan_files();
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
