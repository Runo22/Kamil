#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/paths.h"
#include "core/cmake.h"
#include "core/file_index.h"
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

// Expands %VARIABLES% in a path.
std::wstring expand_env(const std::wstring& raw);

class App {
public:
    App();
    ~App();

    int run(HINSTANCE instance, bool autostart);

private:
    struct Job;
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

    // Files and scripts (app_files.cpp)
    void scan_files();
    void on_files_ready(std::unique_ptr<FileIndex> index);
    Item make_file_item(const std::wstring& path, bool is_dir) const;
    void add_file_hits(const std::wstring& query, std::vector<Hit>& hits);
    void run_detached(std::wstring exe, std::wstring args, std::wstring dir, bool admin, std::wstring title, const wchar_t* verb = nullptr);
    std::wstring python_for(bool windowed) const;
    void run_script(const Item& item, bool admin);
    void edit_file(const Item& item);
    void open_file(const Item& item, LaunchMode mode);
    std::vector<Item> file_actions(const Item& item) const;
    bool run_file_action(const Item& item, const std::wstring& action);

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
    std::wstring job_title(const Job& job) const;
    void show_jobs();
    void cancel_job(uint64_t id);
    std::vector<Item> job_actions(const std::wstring& root) const;
    bool run_job_action(const Item& parent, const std::wstring& action);
    void update_job_timer();
    void warm_projects();
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
    // Visual Studio jobs: queued and running, in submit order (the bridge runs them one by one).
    struct Job {
        uint64_t id = 0;
        VsJob::Kind kind = VsJob::Kind::Build;
        std::wstring root;
        std::wstring label;   // preset
        std::wstring text;    // latest progress line
        bool running = false;
        bool cancelling = false;
        ULONGLONG queued = 0;
        ULONGLONG started = 0;
    };
    std::vector<Job> jobs_;
    struct LastJob {
        std::wstring text;
        ULONGLONG finished = 0;
        bool ok = false;
    } last_job_;
    std::unordered_map<std::wstring, RepoState> repo_states_;
    Tools tools_;
    bool tools_ready_ = false;
    std::vector<Item> items_;  // commands + filtered apps; LauncherWindow keeps pointers into it
    std::unique_ptr<FileIndex> file_index_;
    std::vector<Item> query_items_;  // file results of the current query (the launcher points into it)
    std::thread file_scan_;
    std::atomic<bool> file_scan_cancel_{false};
    ULONGLONG files_scanned_at_ = 0;
    std::thread warm_;  // pre-reads CMake presets and File API replies so Ctrl+K opens instantly
    std::atomic<bool> warm_cancel_{false};

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
