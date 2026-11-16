#pragma once

// workspaces: one name opens a set of projects, folders, files and apps together, and checks that
// git repositories are on the branches the workspace expects.
//
//   workspaces:
//     - name: Sensor
//       keyword: sen
//       open:
//         - { path: 'D:\src\sensor', with: vs, branch: develop }
//         - { path: 'D:\src\sensor-lib', with: code }
//         - { item: Tera Term }

#include <optional>
#include <string>
#include <vector>

#include "core/path_input.h"
#include "core/settings.h"

namespace kamil {

struct WorkspaceEntry {
    std::wstring path;          // expanded (%VARS%, ~); empty for `item`
    std::wstring item;          // a search result by its exact name: app, command, bookmark ...
    std::string with = "default";  // default | vs | code | explorer | terminal
    std::wstring branch;        // git branch the repository at `path` should be on; empty: any

    // What the entry is called in lists: the folder / file name, or the item name.
    std::wstring label() const;
};

struct Workspace {
    std::wstring name;
    std::wstring keyword_folded;
    std::vector<WorkspaceEntry> entries;
};

// Reads the workspaces objects. Entries with neither path nor item are dropped and described in
// `problems` ("Sensor: entry 2 has no path or item").
std::vector<Workspace> parse_workspaces(const Value::Objects& objects, const std::wstring& home, const EnvExpander& expand,
                                        std::vector<std::wstring>* problems = nullptr);

// The branch of the git work tree at `path`, read from HEAD (no git.exe); a detached HEAD gives
// its short commit id. nullopt when `path` is not a git work tree.
std::optional<std::wstring> current_branch(const std::wstring& path);

struct BranchProblem {
    size_t entry = 0;      // index into Workspace::entries
    std::wstring expected;
    std::wstring actual;   // empty: not a git repository
};

// Entries whose repository is not on the expected branch. `branch_of` reads a branch (normally
// current_branch); it is called only for entries that name a branch.
std::vector<BranchProblem> check_branches(const Workspace& w,
                                          const std::function<std::optional<std::wstring>(const std::wstring&)>& branch_of);

}  // namespace kamil
