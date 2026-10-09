#include "core/search.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_map>

#include "core/fuzzy.h"

namespace kamil {

namespace {

constexpr int kAliasBoost = 100'000;
constexpr int kKeywordPenalty = 20;

}  // namespace

int learning_bonus(double frecency, double affinity) {
    // Affinity dominates: picking an item for exactly this prefix is the strongest signal.
    const double f = 12.0 * std::log2(1.0 + frecency);
    const double a = std::min(80.0, 30.0 * std::log2(1.0 + affinity));
    return static_cast<int>(f + a);
}

bool hit_better(const Hit& a, const Hit& b) {
    if (a.score != b.score) return a.score > b.score;
    if (a.item->title.size() != b.item->title.size()) return a.item->title.size() < b.item->title.size();
    return a.item->title < b.item->title;
}

std::wstring normalize_folder(std::wstring_view path) {
    std::wstring out = fold(trim(path));
    for (auto& c : out)
        if (c == L'/') c = L'\\';
    while (out.size() > 1 && out.back() == L'\\') out.pop_back();
    return out;
}

int folder_bonus(std::wstring_view path, const std::vector<FolderBoost>& boosts) {
    size_t best_len = 0;
    int bonus = 0;
    for (const auto& b : boosts) {
        const size_t n = b.folder.size();
        if (n == 0 || n < best_len || path.size() < n || path.compare(0, n, b.folder) != 0) continue;
        if (path.size() > n && path[n] != L'\\' && path[n] != L'/') continue;  // "D:\\src" must not match "D:\\src2"
        best_len = n;
        bonus = b.bonus;
    }
    return bonus;
}

std::vector<Hit> search(std::span<const Item> items, std::wstring_view query, const UsageStore& usage,
                        const SearchOptions& opt) {
    std::vector<Hit> hits;
    FuzzyMatcher matcher(query);

    if (matcher.empty()) {
        if (!opt.frequent_when_empty) return hits;
        const auto keys = usage.top(opt.limit, opt.now_unix);
        if (keys.empty()) return hits;
        std::unordered_map<std::wstring_view, const Item*> by_key;
        by_key.reserve(items.size());
        for (const auto& it : items) by_key.emplace(it.key, &it);
        for (const auto& k : keys) {
            auto found = by_key.find(k);
            if (found == by_key.end()) continue;
            hits.push_back(Hit{found->second, static_cast<int>(usage.frecency(k, opt.now_unix) * 100), {}});
        }
        return hits;
    }

    const uint64_t qmask = matcher.mask();
    const std::wstring& folded_query = matcher.folded_query();

    for (const auto& item : items) {
        std::optional<int> score;
        if ((item.mask & qmask) == qmask) score = matcher.match(item.title, item.title_folded);
        // Keywords match a little weaker than the title itself.
        if (!score && !item.keywords_folded.empty() && (item.keywords_mask & qmask) == qmask)
            if (auto k = matcher.match(item.keywords, item.keywords_folded)) score = *k - kKeywordPenalty;
        if (!score) continue;
        int total = *score;
        if (opt.learning)
            total += learning_bonus(usage.frecency(item.key, opt.now_unix), usage.affinity(folded_query, item.key, opt.now_unix));
        for (const auto& a : opt.aliases)
            if (a.alias_folded == folded_query && a.target_folded == item.title_folded) total += kAliasBoost;
        if (!opt.folder_boosts.empty() && !item.path_folded.empty()) total += folder_bonus(item.path_folded, opt.folder_boosts);
        hits.push_back(Hit{&item, total, {}});
    }

    // Aliases may point at items whose title does not fuzzy-match the alias text at all.
    for (const auto& a : opt.aliases) {
        if (a.alias_folded != folded_query) continue;
        for (const auto& item : items) {
            if (item.title_folded != a.target_folded) continue;
            const bool present = std::any_of(hits.begin(), hits.end(), [&](const Hit& h) { return h.item == &item; });
            if (!present) hits.push_back(Hit{&item, kAliasBoost, {}});
        }
    }

    const size_t count = std::min(opt.limit, hits.size());
    std::partial_sort(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(count), hits.end(), hit_better);
    hits.resize(count);

    for (auto& h : hits) matcher.match(h.item->title, h.item->title_folded, &h.positions);
    return hits;
}

}  // namespace kamil
