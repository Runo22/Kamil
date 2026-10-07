#pragma once

// Platform independent git helpers. Reading HEAD needs no git.exe, so the branch of every repo
// is known instantly; the working tree status comes from `git status --porcelain=v1 -b`.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kamil {

struct GitHead {
    std::string branch;    // "main"; empty when detached
    std::string detached;  // short commit id when detached
    bool valid = false;

    std::string display() const { return !branch.empty() ? branch : detached.empty() ? std::string() : detached; }
};

struct GitStatus {
    bool valid = false;
    std::string branch;
    std::string upstream;
    int ahead = 0;
    int behind = 0;
    int staged = 0;
    int modified = 0;
    int untracked = 0;
    int conflicts = 0;

    int changes() const { return staged + modified + untracked + conflicts; }
};

// Accepts a work tree; follows a ".git" file ("gitdir: ...") used by worktrees and submodules.
std::optional<std::filesystem::path> resolve_git_dir(const std::filesystem::path& worktree);
GitHead parse_head(std::string_view head_file_content);
GitHead read_head(const std::filesystem::path& git_dir);

// Parses `git status --porcelain=v1 -b` output.
GitStatus parse_porcelain_v1(std::string_view output);

struct RepoScanOptions {
    size_t max_depth = 4;
    std::vector<std::wstring> exclude;  // folded glob patterns matched against directory names
    size_t max_repos = 2000;
    bool include_cmake_roots = false;  // also stop at (and return) folders with a CMakeLists.txt
};

// Finds git work trees (and optionally CMake project roots) below the roots. Does not descend into a found repository (nested
// repositories and submodules are not listed separately).
std::vector<std::filesystem::path> find_repos(const std::vector<std::filesystem::path>& roots, const RepoScanOptions& opt);

}  // namespace kamil
