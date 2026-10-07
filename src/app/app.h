#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/paths.h"
#include "core/cmake.h"
#include "core/item.h"
#include "core/project_state.h"
#include "core/search.h"
#include "core/settings.h"
#include "core/usage.h"
#include "platform/com_ports.h"
#include "platform/file_watcher.h"
#include "platform/icon_loader.h"
#include "platform/tools.h"
#include "platform/shell.h"
#include "platform/tray.h"
#include "platform/vs_bridge.h"
#include "providers/apps_provider.h"
#include "providers/repo_provider.h"
#include "ui/launcher_window.h"

namespace kamil {

// Everything Kamil knows about one CMake project at a given moment (cached briefly).
struct ProjectView {
    std::wstring root;
    std::wstring name;
    std::vector<ConfigurePreset> presets;
    size_t preset = 0;                 // index into presets (valid when !presets.empty())
    bool preset_guessed = false;       // no explicit choice: the most recently configured one
    CodeModel model;
    std::vector<CMakeTarget> executables;
    size_t target = 0;
    std::filesystem::path exe;
    ProjectChoice choice;
    std::wstring com;                  // resolved port, may be empty
    std::wstring com_label;
    std::string args_template;
    std::wstring args;                 // expanded
    std::wstring problem;              // why build/run/debug cannot proceed, empty when ready
    ULONGLONG built_at = 0;            // tick when computed

    const ConfigurePreset* active() const { return presets.empty() ? nullptr : &presets[preset]; }
    std::wstring preset_name() const;
    std::wstring target_name() const;
};

class App {
public:
    App();
    ~App();

    int run(HINSTANCE instance, bool autostart);

private:
    static LRESULT CALLBACK wndproc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    bool create_window();
    void ensure_config_files();
    void reload_settings(bool initial);
    void apply_settings(const Settings& s, const std::vector<std::string>& changed, bool initial);
    void report(const LoadResult& r);
    void register_hotkey(const std::string& text);

    void set_apps(std::vector<Item> apps);
    void rebuild_items();
    void run_query(const std::wstring& query);

    void on_activate(const Item& item, LaunchMode mode);
    std::vector<Item> actions_for(const Item& item) const;
    void run_action(const Item& parent, const std::wstring& action);
    Footer footer_for(const Item* selected, const Item* action_parent);
    std::wstring repo_context(const std::wstring& path);
    void scan_repos();
    std::wstring default_repo_action() const;

    // Projects (app_projects.cpp)
    const ProjectView& project_view(const std::wstring& root, bool fresh = false);
    void invalidate_project(const std::wstring& root);
    std::vector<Item> project_actions(const Item& repo);
    bool run_project_action(const Item& repo, const std::wstring& action);
    bool quick_action(const Item& item, wchar_t key);
    std::wstring project_context(const std::wstring& root);
    void submit_vs(VsJob::Kind kind, const Item& repo);
    void run_project_exe(const Item& repo, const std::wstring& preset_override);
    void on_vs_event(const VsEvent& e);
    const std::vector<ComPort>& com_ports();
    std::wstring job_status() const;
    bool is_project(const std::wstring& root) const;
    std::string vs_choice() const;
    void run_command(const std::wstring& id);
    void toggle_from_tray();
    void show_tray_menu(POINT at);
    void notify(const std::wstring& title, const std::wstring& text, Tray::Balloon kind);
    void schedule_usage_save();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HICON icon_ = nullptr;
    Paths paths_;

    SettingsStore store_;
    std::vector<Diagnostic> diagnostics_;
    std::string hotkey_text_;
    bool hotkey_ok_ = false;

    UsageStore usage_;
    SearchOptions search_options_;
    std::vector<std::wstring> exclude_patterns_;  // folded globs

    std::vector<Item> apps_;
    std::vector<RepoInfo> repos_;
    std::unordered_map<std::wstring, ProjectView> projects_;
    ProjectStateStore project_state_;
    std::vector<ComPort> com_ports_;
    bool com_ports_stale_ = true;
    std::unique_ptr<VsBridge> vs_;
    struct Job {
        bool running = false;
        VsJob::Kind kind = VsJob::Kind::Build;
        std::wstring label;
        std::wstring text;
        ULONGLONG started = 0;
        ULONGLONG finished = 0;
        bool ok = false;
        std::wstring root;
    } job_;
    std::unordered_map<std::wstring, RepoState> repo_states_;
    Tools tools_;
    bool tools_ready_ = false;
    std::vector<Item> items_;  // commands + filtered apps; LauncherWindow keeps pointers into it

    LauncherWindow launcher_;
    Tray tray_;
    FileWatcher watcher_;
    AppsProvider apps_provider_;
    RepoProvider repo_provider_;
    std::unique_ptr<GitService> git_;
    std::unique_ptr<IconLoader> icon_loader_;
    std::unique_ptr<Executor> executor_;
};

}  // namespace kamil
