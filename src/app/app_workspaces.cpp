// Conan cache packages and workspaces as search results.

#include <algorithm>
#include <filesystem>
#include <thread>

#include "app/action_item.h"
#include "app/app.h"
#include "core/i18n.h"
#include "core/log.h"
#include "core/settings_schema.h"
#include "core/text.h"
#include "platform/process.h"

namespace kamil {

namespace {

// An environment variable, or empty when it is not set.
std::wstring env_var(const wchar_t* name) {
    wchar_t buf[32768];
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    return n > 0 && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
}

// What makes two scans different for the result list (no rebuild while typing when nothing changed).
std::wstring conan_signature(const std::vector<ConanPackage>& packages) {
    std::wstring s;
    for (const auto& p : packages) {
        s += p.name + L'|' + conan_open_folder(p) + L'|' + conan_summary(p) + L'\n';
    }
    return s;
}

constexpr wchar_t kWorkspacePrefix[] = L"ws:";

}  // namespace

// ---------------------------------------------------------------------------------------------
// Conan

void App::scan_conan() {
    const auto s = store_.current();
    if (!s->get_bool(keys::kConan)) {
        if (!conan_.empty()) {
            conan_.clear();
            rebuild_items();
        }
        return;
    }
    if (conan_scanning_) return;
    conan_scanning_ = true;
    conan_scanned_at_ = GetTickCount64();
    const std::wstring home = conan_home(expand_env(widen(s->get_string(keys::kConanHome))), env_var(L"CONAN_USER_HOME"), home_dir());
    const HWND target = hwnd_;
    // Its own thread: CONAN_USER_HOME may be on a network drive.
    std::thread([home, target] {
        auto* scan = new ConanScan;
        scan->data_dir = conan_data_dir(home);
        scan->packages = scan_conan_data(scan->data_dir);
        post_owned(target, WM_KAMIL_CONAN_READY, scan);
    }).detach();
}

void App::on_conan_scanned(std::unique_ptr<ConanScan> scan) {
    conan_scanning_ = false;
    if (!store_.current()->get_bool(keys::kConan)) return;
    if (scan->data_dir != conan_data_)
        log_line(scan->data_dir.empty() ? std::wstring(L"[conan] no Conan cache found")
                                        : L"[conan] " + scan->data_dir + L": " + std::to_wstring(scan->packages.size()) + L" packages");
    const bool changed = scan->data_dir != conan_data_ || conan_signature(scan->packages) != conan_signature(conan_);
    conan_data_ = std::move(scan->data_dir);
    conan_ = std::move(scan->packages);
    if (changed) rebuild_items();
}

void App::add_conan_items(std::vector<Item>& items) const {
    for (const auto& p : conan_) {
        const std::wstring folder = conan_open_folder(p);
        Item it = make_file_item(folder, true, false);  // a folder: Explorer, VS Code, terminal ... in Ctrl+K
        it.kind = ItemKind::Folder;
        it.key = L"conan:" + fold(p.name);  // stays the same when versions come and go
        it.title = p.name;
        const std::wstring refs = conan_summary(p);
        it.subtitle = L"Conan   " + (refs.empty() ? folder : refs);
        it.keywords = L"conan";
        it.icon_key = L"icon:folder";
        it.prepare();
        items.push_back(std::move(it));
    }
}

void App::before_show() {
    // conan install / remove while the window was closed: a quick rescan (folders only).
    if (store_.current()->get_bool(keys::kConan) && GetTickCount64() - conan_scanned_at_ > 60'000) scan_conan();
}

// ---------------------------------------------------------------------------------------------
// Workspaces

void App::load_workspaces(const Settings& s, bool report) {
    std::vector<std::wstring> problems;
    workspaces_ = parse_workspaces(s.get_objects(keys::kWorkspaces), home_dir(), expand_env, &problems);
    for (const auto& p : problems) log_line(L"[workspaces] " + p);
    if (report && !problems.empty())
        notify(loc(L"Workspace entry skipped", L"Çalışma alanı girdisi atlandı"), problems.front(), Tray::Balloon::Warning);
}

void App::add_workspace_items(std::vector<Item>& items) const {
    for (size_t i = 0; i < workspaces_.size(); ++i) {
        const Workspace& w = workspaces_[i];
        Item it;
        it.kind = ItemKind::Command;
        it.key = L"ws:" + fold(w.name);
        it.target = kWorkspacePrefix + std::to_wstring(i);
        it.title = w.name;
        std::wstring what;
        for (const auto& e : w.entries) {
            if (!what.empty()) what += L" · ";
            what += e.label();
            if (!e.branch.empty()) what += L" ⎇ " + e.branch;
        }
        it.subtitle = loc(L"Workspace", L"Çalışma alanı") + std::wstring(L"   ") + what;
        it.keywords = loc(L"workspace", L"çalışma alanı workspace");
        it.glyph = kGlyphFolder;
        it.prepare();
        items.push_back(std::move(it));
    }
}

const Workspace* App::workspace_of(const Item& item) const {
    if (item.kind != ItemKind::Command || item.target.rfind(kWorkspacePrefix, 0) != 0) return nullptr;
    const size_t i = std::wcstoul(item.target.c_str() + std::size(kWorkspacePrefix) - 1, nullptr, 10);
    // The settings may have changed since the item was made: the name must still match.
    if (i >= workspaces_.size() || workspaces_[i].name != item.title) return nullptr;
    return &workspaces_[i];
}

void App::open_workspace(const Item& item, bool check) {
    const Workspace* w = workspace_of(item);
    if (!w) return;
    if (check) {
        // Network paths are not read here (an unreachable server would block the UI).
        auto branch_of = [this](const std::wstring& path) -> std::optional<std::wstring> {
            if (may_block(path)) return std::nullopt;
            return current_branch(path);
        };
        Workspace local = *w;
        for (auto& e : local.entries)
            if (may_block(e.path)) e.branch.clear();
        const auto problems = check_branches(local, branch_of);
        if (!problems.empty()) {
            // Ask: switch the branch (git switch, in Kamil Console), open anyway, or Esc.
            std::vector<Item> rows;
            for (const auto& p : problems) {
                const WorkspaceEntry& e = w->entries[p.entry];
                if (p.actual.empty()) {
                    rows.push_back(make_action(L"ws-none", fmt(loc(L"{} is not a git repository", L"{} bir git deposu değil"), e.label()), {}, kGlyphInfo,
                                               fmt(loc(L"Expected branch {}: {}", L"Beklenen dal {}: {}"), e.branch, e.path)));
                } else if (!tools_.git.empty()) {
                    rows.push_back(make_action(L"ws-switch:" + std::to_wstring(p.entry),
                                               fmt(loc(L"Switch {} to {}", L"{} deposunu {} dalına geçir"), e.label(), e.branch), {}, kGlyphBranch,
                                               fmt(loc(L"Now on {} · git switch {} (output in Kamil Console)", L"Şu an {} dalında · git switch {} (çıktı Kamil Console'da)"),
                                                   p.actual, e.branch)));
                } else {
                    rows.push_back(make_action(L"ws-none", fmt(loc(L"{} is on {}, not {}", L"{} {} dalında, {} değil"), e.label(), p.actual, e.branch), {},
                                               kGlyphInfo, loc(L"git.exe was not found: switch it yourself", L"git.exe bulunamadı: dalı kendin değiştir")));
                }
            }
            rows.push_back(make_action(L"ws-open", loc(L"Open anyway", L"Yine de aç"), {}, kGlyphPlay,
                                       loc(L"Opens everything on the branches they are on now", L"Hepsini şu anki dallarıyla açar")));
            log_line(L"[workspaces] " + w->name + L": " + std::to_wstring(problems.size()) + L" repositories on another branch");
            launcher_.show_list(item, std::move(rows), fmt(loc(L"{}: not on the expected branch", L"{}: beklenen dalda değil"), w->name));
            return;
        }
    }
    launcher_.hide();
    const Workspace copy = *w;  // opening may rebuild the item list
    std::vector<std::wstring> missing;
    for (const auto& e : copy.entries) open_workspace_entry(e, missing);
    if (!missing.empty()) {
        std::wstring text;
        for (const auto& m : missing) text += (text.empty() ? L"" : L"\n") + m;
        notify(fmt(loc(L"{}: not everything was opened", L"{}: hepsi açılamadı"), copy.name), text, Tray::Balloon::Warning);
    }
}

void App::open_workspace_entry(const WorkspaceEntry& e, std::vector<std::wstring>& missing) {
    if (!e.item.empty()) {
        // A search result by its name; apps first (a folder may have the same name).
        const std::wstring want = fold(e.item);
        const Item* found = nullptr;
        for (const auto& it : items_) {
            if (it.title_folded != want || workspace_of(it)) continue;  // a workspace does not open workspaces
            if (!found || (it.kind == ItemKind::App && found->kind != ItemKind::App)) found = &it;
        }
        if (!found) {
            missing.push_back(fmt(loc(L"{}: no such app or command", L"{}: böyle bir uygulama ya da komut yok"), e.item));
            return;
        }
        const Item copy = *found;
        on_activate(copy, LaunchMode::Normal);
        return;
    }
    const bool remote = may_block(e.path);
    bool is_dir = true;
    if (remote) {
        is_dir = std::filesystem::path(e.path).extension().empty();
    } else {
        std::error_code ec;
        const auto status = std::filesystem::status(e.path, ec);
        if (ec || !std::filesystem::exists(status)) {
            missing.push_back(fmt(loc(L"{}: not found", L"{}: bulunamadı"), e.path));
            return;
        }
        is_dir = std::filesystem::is_directory(status);
    }
    // A scanned project keeps its own item (preset, target ... are remembered per project).
    Item it;
    const std::wstring repo_key = L"repo:" + e.path;
    const auto repo = std::find_if(items_.begin(), items_.end(), [&](const Item& x) { return x.key == repo_key; });
    if (repo != items_.end()) it = *repo;
    else it = make_file_item(e.path, is_dir, !remote);

    const std::string& with = e.with;
    if (it.kind == ItemKind::Repo) {
        if (is_project(it.path)) set_last_project(it.path);
        run_action(it, with == "default" ? default_repo_action() : widen(with));
        return;
    }
    if (it.kind == ItemKind::Folder) {
        if (with == "vs") run_action(it, L"dir-vs");
        else if (with == "code") run_action(it, L"dir-code");
        else if (with == "terminal") run_action(it, L"file-terminal");
        else on_activate(it, LaunchMode::Normal);  // default, explorer
        return;
    }
    if (with == "code") open_in_code(it.path, 1);
    else if (with == "explorer") on_activate(it, LaunchMode::OpenLocation);
    else if (with == "terminal") run_action(it, L"file-terminal");
    else on_activate(it, LaunchMode::Normal);  // default, vs (.sln files open in Visual Studio anyway)
}

std::vector<Item> App::workspace_actions(const Item& item) const {
    std::vector<Item> list;
    const Workspace* w = workspace_of(item);
    if (!w) return list;
    list.push_back(make_action(L"ws-open-all", loc(L"Open all", L"Hepsini aç"), {}, kGlyphPlay,
                               loc(L"Checks the branches first", L"Önce dalları denetler")));
    for (size_t i = 0; i < w->entries.size(); ++i) {
        const WorkspaceEntry& e = w->entries[i];
        std::wstring sub = e.path.empty() ? loc(L"App or command", L"Uygulama ya da komut") : e.path;
        if (!e.path.empty() && !may_block(e.path)) {
            if (auto b = current_branch(e.path)) {
                sub += L"   ⎇ " + *b;
                if (!e.branch.empty() && *b != e.branch) sub += fmt(loc(L"  ⚠ expected {}", L"  ⚠ beklenen {}"), e.branch);
            }
        }
        Item a = make_action(L"ws-entry:" + std::to_wstring(i), fmt(loc(L"Open {}", L"{} aç"), e.label()), e.path, e.path.empty() ? kGlyphPlay : 0, sub);
        if (!e.path.empty()) a.icon_key = L"icon:folder";
        list.push_back(std::move(a));
    }
    return list;
}

bool App::run_workspace_action(const Item& item, const std::wstring& action) {
    if (action.rfind(L"ws-", 0) != 0) return false;
    const Workspace* w = workspace_of(item);
    if (!w) return true;
    if (action == L"ws-none") return true;  // an information row
    if (action == L"ws-open") {
        open_workspace(item, false);
        return true;
    }
    if (action == L"ws-open-all") {
        open_workspace(item, true);
        return true;
    }
    if (action.rfind(L"ws-entry:", 0) == 0) {
        const size_t i = std::wcstoul(action.c_str() + 9, nullptr, 10);
        if (i >= w->entries.size()) return true;
        launcher_.hide();
        const WorkspaceEntry e = w->entries[i];
        std::vector<std::wstring> missing;
        open_workspace_entry(e, missing);
        if (!missing.empty()) notify(w->name, missing.front(), Tray::Balloon::Warning);
        return true;
    }
    if (action.rfind(L"ws-switch:", 0) == 0) {
        const size_t i = std::wcstoul(action.c_str() + 10, nullptr, 10);
        if (i >= w->entries.size() || tools_.git.empty()) return true;
        const WorkspaceEntry e = w->entries[i];
        // git refuses to switch when local changes would be overwritten; the output says why.
        run_git(e.path, L"switch " + quote_arg(e.branch), false);
        return true;
    }
    return true;
}

}  // namespace kamil
