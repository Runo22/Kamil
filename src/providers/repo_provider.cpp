#include "providers/repo_provider.h"

#include "core/text.h"
#include "platform/process.h"

namespace kamil {

RepoProvider::~RepoProvider() {
    if (worker_.joinable()) worker_.join();
}

void RepoProvider::scan_async(std::vector<std::filesystem::path> roots, RepoScanOptions options, HWND target) {
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([roots = std::move(roots), options = std::move(options), target] {
        auto* repos = new std::vector<RepoInfo>();
        for (const auto& path : find_repos(roots, options)) {
            RepoInfo info;
            info.path = path.wstring();
            info.name = path.filename().wstring();
            if (auto git_dir = resolve_git_dir(path)) {
                info.git = true;
                info.branch = read_head(*git_dir).display();
            }
            std::error_code ec;
            info.cmake = std::filesystem::exists(path / "CMakeLists.txt", ec);
            repos->push_back(std::move(info));
        }
        post_owned(target, WM_KAMIL_REPOS_READY, repos);
    });
}

GitService::GitService(HWND target) : target_(target), thread_([this] { run(); }) {}

GitService::~GitService() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_one();
    thread_.join();
}

void GitService::configure(std::wstring git_exe, bool run_status) {
    std::lock_guard lock(mutex_);
    git_exe_ = std::move(git_exe);
    run_status_ = run_status;
}

void GitService::request(const std::wstring& repo_path) {
    {
        std::lock_guard lock(mutex_);
        if (!queued_.insert(repo_path).second) return;
        queue_.push_back(repo_path);
    }
    cv_.notify_one();
}

void GitService::run() {
    for (;;) {
        std::wstring path, git;
        bool status = false;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            path = std::move(queue_.back());  // newest first: the row the user is looking at
            queue_.pop_back();
            queued_.erase(path);
            git = git_exe_;
            status = run_status_;
        }
        auto* state = new RepoState{path, {}, {}, 0};
        if (auto git_dir = resolve_git_dir(path)) state->head = read_head(*git_dir);
        if (status && !git.empty()) {
            // Optional locks off: never compete with a running VS / git GUI for index.lock.
            const std::wstring cmd = quote_arg(git) + L" --no-optional-locks -C " + quote_arg(path) +
                                     L" status --porcelain=v1 -b --untracked-files=normal";
            if (auto r = run_capture(cmd, path, 3000); r && r->exit_code == 0 && !r->timed_out)
                state->status = parse_porcelain_v1(r->output);
        }
        state->tick = GetTickCount64();
        post_owned(target_, WM_KAMIL_GIT_STATUS, state);
    }
}

}  // namespace kamil
