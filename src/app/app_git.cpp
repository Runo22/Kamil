// Git actions in Ctrl+K: status, log, fetch, pull, push and switching branches. Each runs git.exe
// in a Kamil Console tab (status and log show it; the others report with a notification).

#include <filesystem>

#include "app/action_item.h"
#include "app/app.h"
#include "core/git.h"
#include "core/i18n.h"
#include "core/log.h"
#include "core/text.h"
#include "platform/process.h"

namespace kamil {

namespace {

std::wstring repo_name(const std::wstring& path) {
    const std::wstring part = last_path_part(path);
    return part.empty() ? path : part;
}

}  // namespace

void App::add_git_actions(const std::wstring& path, std::vector<Item>& list) const {
    if (tools_.git.empty() || path.empty() || may_block(path)) return;
    const auto git_dir = resolve_git_dir(std::filesystem::path(path));
    if (!git_dir) return;
    const std::wstring branch = widen(read_head(*git_dir).display());

    // What is known from the last `git status` of the footer: changes, ahead / behind.
    std::wstring changes, ahead, behind;
    if (auto it = repo_states_.find(path); it != repo_states_.end() && it->second.status.valid) {
        const GitStatus& s = it->second.status;
        changes = s.changes() ? fmt(loc(L"{} changes", L"{} değişiklik"), std::to_wstring(s.changes())) : loc(L"clean", L"temiz");
        if (s.ahead) ahead = L"   ↑" + std::to_wstring(s.ahead);
        if (s.behind) behind = L"   ↓" + std::to_wstring(s.behind);
    }
    const std::wstring on = branch.empty() ? std::wstring() : L"⎇ " + branch + L"   ";
    list.push_back(make_action(L"git-status", loc(L"Git status", L"Git durumu"), {}, kGlyphList,
                               on + (changes.empty() ? L"git status" : changes) + ahead + behind));
    list.push_back(make_action(L"git-switch-pick", loc(L"Git: switch branch…", L"Git: dal değiştir…"), {}, kGlyphBranch,
                               branch.empty() ? std::wstring(L"git switch") : fmt(loc(L"Now on {}", L"Şu an {}"), branch)));
    list.push_back(make_action(L"git-fetch", loc(L"Git fetch", L"Git fetch"), {}, kGlyphSync, L"git fetch --all --prune"));
    list.push_back(make_action(L"git-pull", loc(L"Git pull", L"Git pull"), {}, kGlyphDownload,
                               L"git pull --ff-only" + behind));
    list.push_back(make_action(L"git-push", loc(L"Git push", L"Git push"), {}, kGlyphUpload, L"git push" + ahead));
    list.push_back(make_action(L"git-log", loc(L"Git log", L"Git geçmişi"), {}, kGlyphHistory,
                               loc(L"Last 30 commits (git log --oneline --graph)", L"Son 30 commit (git log --oneline --graph)")));
}

bool App::run_git_action(const Item& parent, const std::wstring& action) {
    if (action.rfind(L"git-", 0) != 0 || action == L"git-bash" || action == L"git-gui") return false;
    const std::wstring repo = parent.path;
    if (repo.empty() || tools_.git.empty()) return true;

    if (action == L"git-status") run_git(repo, L"status", true);
    else if (action == L"git-log") run_git(repo, L"log --oneline --graph --decorate -30", true);
    else if (action == L"git-fetch") run_git(repo, L"fetch --all --prune", false);
    // --ff-only: never a merge commit by surprise; when the branches diverged git says so.
    else if (action == L"git-pull") run_git(repo, L"pull --ff-only", false);
    else if (action == L"git-push") run_git(repo, L"push", false);
    else if (action == L"git-switch-pick") {
        const auto git_dir = resolve_git_dir(std::filesystem::path(repo));
        if (!git_dir) return true;
        const std::string current = read_head(*git_dir).branch;
        const GitBranches branches = list_branches(*git_dir);
        std::vector<Item> rows;
        for (const auto& b : branches.local) {
            const bool now = b == current;
            Item row = make_action(L"git-switch:" + widen(b), widen(b), {}, now ? kGlyphCheck : kGlyphBranch,
                                   now ? loc(L"Current branch", L"Şu anki dal") : std::wstring());
            if (now) rows.insert(rows.begin(), std::move(row));
            else rows.push_back(std::move(row));
        }
        for (const auto& r : remote_only_branches(branches)) {
            const std::string name = r.substr(r.find('/') + 1);
            rows.push_back(make_action(L"git-switch:" + widen(name), widen(r), {}, kGlyphBranch,
                                       fmt(loc(L"Creates the local branch {} tracking it", L"Onu izleyen yerel {} dalını oluşturur"), widen(name))));
        }
        if (rows.empty()) {
            notify(repo_name(repo), loc(L"No branches found.", L"Dal bulunamadı."), Tray::Balloon::Info);
            return true;
        }
        launcher_.show_list(parent, std::move(rows),
                            fmt(loc(L"Switch branch — {} (fetch first to see new remote branches)",
                                    L"Dal değiştir — {} (yeni uzak dallar için önce fetch)"),
                                repo_name(repo)));
    } else if (action.rfind(L"git-switch:", 0) == 0) {
        const std::wstring branch = action.substr(11);
        const auto git_dir = resolve_git_dir(std::filesystem::path(repo));
        if (git_dir && widen(read_head(*git_dir).branch) == branch) {
            launcher_.hide();  // already there
            return true;
        }
        // git refuses when local changes would be overwritten; the output (Kamil Console) says why.
        run_git(repo, L"switch " + quote_arg(branch), false);
    }
    return true;
}

void App::run_git(const std::wstring& repo, const std::wstring& args, bool show_output) {
    if (tools_.git.empty()) return;
    launcher_.hide();
    const std::wstring command = L"git " + args;
    const std::wstring title = command.substr(0, command.find(L" -")) + L" — " + repo_name(repo);
    start_in_console(
        title, repo, repo, tools_.git, args, repo, [this, repo, args, show_output] { run_git(repo, args, show_output); },
        show_output ? std::wstring() : title,  // fetch, pull, push, switch: a notification says how it went
        [this, repo](bool) { after_git(repo); });
    if (show_output) show_console(true);
}

void App::after_git(const std::wstring& repo) {
    repo_states_.erase(repo);  // the footer asks git status again
    bool changed = false;
    if (const auto git_dir = resolve_git_dir(std::filesystem::path(repo))) {
        const std::string branch = read_head(*git_dir).display();
        for (auto& r : repos_)
            if (r.path == repo && r.branch != branch) {
                r.branch = branch;
                changed = true;
            }
    }
    if (changed) rebuild_items();  // "⎇ branch" in the result's second line
    launcher_.refresh_footer();
}

}  // namespace kamil
