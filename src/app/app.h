#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/paths.h"
#include "core/item.h"
#include "core/search.h"
#include "core/settings.h"
#include "core/usage.h"
#include "platform/file_watcher.h"
#include "platform/icon_loader.h"
#include "platform/tools.h"
#include "platform/shell.h"
#include "platform/tray.h"
#include "providers/apps_provider.h"
#include "providers/repo_provider.h"
#include "ui/launcher_window.h"

namespace kamil {

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
