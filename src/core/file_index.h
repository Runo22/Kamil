#pragma once

// Compact in-memory index of files and folders under the configured search folders.
//
// Items for apps and repos are full `Item` objects, but there can be hundreds of thousands of
// files, so they live here as small fixed records plus shared string pools (~30 bytes + name
// per entry). Searching returns entry ids; only the few visible results become Items.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/fuzzy.h"

namespace kamil {

struct IndexRoot {
    std::filesystem::path path;
    int bonus = 0;                      // folder priority (-100..100)
    size_t max_depth = 6;
    std::vector<std::wstring> include;  // folded globs for file names; empty = every file
};

struct IndexOptions {
    std::vector<std::wstring> exclude_dirs;  // folded globs matched against directory names
    size_t max_entries = 300'000;
};

class FileIndex {
public:
    // Walks the roots. `cancel` may abort a long scan (the partial index is returned).
    static FileIndex build(const std::vector<IndexRoot>& roots, const IndexOptions& options,
                           const std::atomic<bool>* cancel = nullptr);

    size_t size() const { return entries_.size(); }
    bool truncated() const { return truncated_; }

    struct Match {
        uint32_t entry;
        int score;
    };
    // Best `limit` entries for the query: fuzzy score on the name + folder priority + a small
    // bonus for runnable scripts. Sorted best first.
    std::vector<Match> search(const FuzzyMatcher& matcher, size_t limit) const;

    std::wstring_view name(uint32_t e) const;
    std::wstring_view folded_name(uint32_t e) const;
    const std::wstring& dir(uint32_t e) const { return dirs_[entries_[e].dir]; }
    std::wstring path(uint32_t e) const;
    bool is_dir(uint32_t e) const { return entries_[e].flags & kDir; }
    bool is_script(uint32_t e) const { return entries_[e].flags & kScript; }
    int bonus(uint32_t e) const { return entries_[e].bonus; }

private:
    enum : uint8_t { kDir = 1, kScript = 2 };
    struct Entry {
        uint32_t name_off;
        uint16_t name_len;
        uint8_t flags;
        int8_t bonus;
        uint32_t dir;
        uint64_t mask;
    };

    void add(uint32_t dir, std::wstring_view name, bool is_dir, int bonus);

    std::wstring names_;   // original names, back to back
    std::wstring folded_;  // folded names at the same offsets
    std::vector<std::wstring> dirs_;
    std::vector<Entry> entries_;
    bool truncated_ = false;
};

// Extensions Kamil runs directly (Enter = run, Alt+E = edit).
bool is_script_extension(std::wstring_view ext_folded);

}  // namespace kamil
