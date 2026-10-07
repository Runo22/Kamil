#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kamil {

// Fuzzy matcher in the spirit of fzf's v2 algorithm: every query character must appear in order,
// and the score rewards word starts, CamelCase humps, consecutive runs and prefixes while
// penalising gaps. Space separated query tokens must all match (in any order).
//
// Usage: construct once per query, then call match() for each candidate. The matcher keeps
// scratch buffers, so one instance must not be used from several threads at the same time.
class FuzzyMatcher {
public:
    explicit FuzzyMatcher(std::wstring_view query);

    bool empty() const noexcept { return tokens_.empty(); }
    uint64_t mask() const noexcept { return mask_; }
    const std::wstring& folded_query() const noexcept { return folded_query_; }

    // `original` is the display text, `folded` must be fold(original). Returns nullopt when the
    // candidate does not match. When `positions` is given it receives the matched indices
    // (sorted, unique), suitable for highlighting.
    std::optional<int> match(std::wstring_view original, std::wstring_view folded,
                             std::vector<uint16_t>* positions = nullptr) const;

    static constexpr size_t kMaxTarget = 512;
    static constexpr size_t kMaxToken = 64;

private:
    std::optional<int> match_token(std::wstring_view token, std::wstring_view original,
                                   std::wstring_view folded, std::vector<uint16_t>* positions) const;

    std::wstring folded_query_;
    std::vector<std::wstring> tokens_;
    uint64_t mask_ = 0;

    mutable std::vector<int32_t> score_;
    mutable std::vector<int16_t> from_;
    mutable std::vector<int8_t> bonus_;
};

}  // namespace kamil
