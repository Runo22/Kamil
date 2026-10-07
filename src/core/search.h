#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/item.h"
#include "core/usage.h"

namespace kamil {

struct Alias {
    std::wstring alias_folded;
    std::wstring target_folded;
};

// Results under `folder` (folded, backslash separated, no trailing separator) get `bonus` points;
// a negative bonus pushes them down. The longest matching folder wins.
struct FolderBoost {
    std::wstring folder;
    int bonus = 0;
};

// Normalizes a path for FolderBoost: folds case/diacritics, '/' -> '\\', strips trailing separators.
std::wstring normalize_folder(std::wstring_view path);
int folder_bonus(std::wstring_view path_folded, const std::vector<FolderBoost>& boosts);

struct SearchOptions {
    size_t limit = 50;
    bool frequent_when_empty = true;
    bool learning = true;
    int64_t now_unix = 0;
    std::vector<Alias> aliases;
    std::vector<FolderBoost> folder_boosts;
};

struct Hit {
    const Item* item = nullptr;
    int score = 0;
    std::vector<uint16_t> positions;  // matched character indices in item->title (for highlighting)
};

// Ranks `items` for `query`. Score = fuzzy match + learned affinity + frecency (+ alias boost).
// With an empty query it returns the most frequently/recently used items.
std::vector<Hit> search(std::span<const Item> items, std::wstring_view query, const UsageStore& usage,
                        const SearchOptions& options);

}  // namespace kamil
