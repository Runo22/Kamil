#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kamil {

// Local, explainable learning:
//  - frecency: an exponentially decaying launch counter per item
//  - query affinity: "after typing <prefix> the user picked <item>" (Alfred's "knowledge"),
//    stored for every prefix of the typed query up to kMaxPrefix characters
// Persisted as a small UTF-8 TSV file.
class UsageStore {
public:
    static constexpr size_t kMaxPrefix = 12;
    static constexpr size_t kMaxAffinities = 4000;

    void set_half_life_days(double days) { half_life_days_ = days > 0 ? days : 14.0; }

    void record(std::wstring_view item_key, std::wstring_view folded_query, int64_t now_unix);
    double frecency(std::wstring_view item_key, int64_t now_unix) const;
    double affinity(std::wstring_view folded_query, std::wstring_view item_key, int64_t now_unix) const;
    std::vector<std::wstring> top(size_t n, int64_t now_unix) const;
    void forget(std::wstring_view item_key);
    void clear();

    bool load(const std::filesystem::path& path);
    bool save(const std::filesystem::path& path) const;  // atomic replace
    bool dirty() const { return dirty_; }

private:
    struct Score {
        double value = 0;
        int64_t last = 0;  // unix seconds
    };
    double decayed(const Score& s, int64_t now) const;
    void bump(Score& s, int64_t now);
    void prune_affinities();
    static std::wstring affinity_key(std::wstring_view query, std::wstring_view item);

    std::unordered_map<std::wstring, Score> frecency_;
    std::unordered_map<std::wstring, Score> affinity_;  // key: query + '\x1F' + item key
    double half_life_days_ = 14.0;
    mutable bool dirty_ = false;
};

}  // namespace kamil
