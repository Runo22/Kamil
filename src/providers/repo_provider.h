#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "core/git.h"
#include "platform/win.h"

namespace kamil {

struct RepoInfo {
    std::wstring path;
    std::wstring name;
    std::string branch;  // from HEAD, no git.exe needed
};

struct RepoState {
    std::wstring path;
    GitHead head;
    GitStatus status;  // valid only when git.exe ran successfully
    uint64_t tick = 0; // GetTickCount64() when measured
};

// Finds git repositories below the project roots on a background thread.
class RepoProvider {
public:
    ~RepoProvider();
    // Posts WM_KAMIL_REPOS_READY with a heap-allocated std::vector<RepoInfo>.
    void scan_async(std::vector<std::filesystem::path> roots, RepoScanOptions options, HWND target);

private:
    std::thread worker_;
};

// Reads HEAD and runs `git status` for repositories on demand (the selected one), one at a time.
class GitService {
public:
    explicit GitService(HWND target);
    ~GitService();
    GitService(const GitService&) = delete;
    GitService& operator=(const GitService&) = delete;

    void configure(std::wstring git_exe, bool run_status);
    void request(const std::wstring& repo_path);  // posts WM_KAMIL_GIT_STATUS with a RepoState*

private:
    void run();

    HWND target_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::wstring> queue_;
    std::unordered_set<std::wstring> queued_;
    std::wstring git_exe_;
    bool run_status_ = true;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace kamil
