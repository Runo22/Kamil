#pragma once

// Compact in-memory index of files and folders under the configured search folders.
//
// Items for apps and repos are full `Item` objects, but there can be hundreds of thousands of
// files, so they live here as small fixed records plus shared string pools (~30 bytes + name
// per entry). Searching returns entry ids; only the few visible results become Items.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

    // An index of the given paths without touching the disk (benchmarks and tests). Paths are
    // "dir\\name"; every distinct dir must be listed before its files.
    static FileIndex from_paths(const std::vector<std::wstring>& paths, const IndexRoot& root);

    size_t size() const { return entries_.size() - removed_; }  // live entries
    bool truncated() const { return truncated_; }
    const std::vector<IndexRoot>& roots() const { return roots_; }

    // Incremental updates from a directory watcher. A file is added only when its folder is part
    // of the index (so excluded folders and the depth limit keep working) and its name passes the
    // root's include globs. Returns whether the index changed.
    bool add_file(std::wstring_view path);
    bool remove(std::wstring_view path);
    // True when `path` is a folder of the index: changes to it need a rescan of that subtree.
    bool is_indexed_dir(std::wstring_view path) const;
    // True when `path` lies under one of the roots (changes there are relevant at all).
    bool under_root(std::wstring_view path) const;

    // Disk cache: the index is shown right after start, before the rescan finishes. The hash of
    // roots + options is stored; a cache built with other settings is not loaded.
    static uint64_t config_hash(const std::vector<IndexRoot>& roots, const IndexOptions& options);
    std::string serialize(uint64_t config_hash) const;
    static std::optional<FileIndex> deserialize(std::string_view data, uint64_t config_hash, const std::vector<IndexRoot>& roots);

    struct Match {
        uint32_t entry;
        int score;
    };
    // Narrowing while typing: a query that extends the previous one can only match entries the
    // previous one matched, so only those are searched again.
    struct SearchState {
        std::wstring query;  // folded query of `matched`
        uint64_t generation = 0;
        std::vector<uint32_t> matched;
    };
    // Best `limit` entries for the query: fuzzy score on the name + folder priority + a small
    // bonus for runnable scripts. Sorted best first. Large indexes are searched on several threads.
    std::vector<Match> search(const FuzzyMatcher& matcher, size_t limit, SearchState* state = nullptr) const;

    std::wstring_view name(uint32_t e) const;
    std::wstring_view folded_name(uint32_t e) const;
    const std::wstring& dir(uint32_t e) const { return dirs_[entries_[e].dir]; }
    std::wstring path(uint32_t e) const;
    bool is_dir(uint32_t e) const { return entries_[e].flags & kDir; }
    bool is_script(uint32_t e) const { return entries_[e].flags & kScript; }
    int bonus(uint32_t e) const { return entries_[e].bonus; }

private:
    enum : uint8_t { kDir = 1, kScript = 2, kRemoved = 4 };
    struct Entry {
        uint32_t name_off;
        uint16_t name_len;
        uint8_t flags;
        int8_t bonus;
        uint32_t dir;
        uint64_t mask;
    };

    void add(uint32_t dir, std::wstring_view name, bool is_dir, int bonus);
    uint32_t add_dir(std::wstring path, uint16_t root);
    std::optional<uint32_t> find_entry(std::wstring_view path) const;
    const std::vector<uint32_t>& children(uint32_t dir) const;

    std::wstring names_;   // original names, back to back
    std::wstring folded_;  // folded names at the same offsets
    std::vector<std::wstring> dirs_;
    std::vector<Entry> entries_;
    std::vector<uint16_t> dir_root_;                      // root index of every dir
    std::unordered_map<std::wstring, uint32_t> dir_ids_;  // normalized folded dir -> id
    // Entries per folder, built lazily for the few folders that receive incremental updates.
    mutable std::unordered_map<uint32_t, std::vector<uint32_t>> children_;
    std::vector<IndexRoot> roots_;
    size_t removed_ = 0;
    static uint64_t new_generation();
    uint64_t generation_ = new_generation();  // changes whenever the entries change (invalidates SearchState)
    bool truncated_ = false;
};

// Extensions Kamil runs directly (Enter = run, Alt+E = edit).
bool is_script_extension(std::wstring_view ext_folded);

}  // namespace kamil
