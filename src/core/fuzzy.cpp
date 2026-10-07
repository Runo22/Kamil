#include "core/fuzzy.h"

#include <algorithm>

#include "core/text.h"

namespace kamil {

namespace {

constexpr int32_t kNeg = -1'000'000;
constexpr int32_t kMatch = 16;
constexpr int32_t kGapStart = 3;
constexpr int32_t kGapExtend = 1;
constexpr int32_t kConsecutive = 4;
constexpr int32_t kBonusBoundary = 8;
constexpr int32_t kBonusCamel = 7;
constexpr int32_t kBonusDigit = 4;
constexpr int32_t kFirstCharMultiplier = 2;
constexpr int32_t kPrefixBonus = 20;
constexpr int32_t kExactBonus = 30;

int8_t bonus_at(std::wstring_view original, size_t j) {
    const wchar_t cur = original[j];
    if (is_separator(cur)) return 0;
    if (j == 0) return kBonusBoundary;
    const wchar_t prev = original[j - 1];
    if (is_separator(prev)) return kBonusBoundary;
    if (is_lower(prev) && is_upper(cur)) return kBonusCamel;
    if (!is_digit(prev) && is_digit(cur)) return kBonusDigit;
    return 0;
}

bool is_subsequence(std::wstring_view token, std::wstring_view folded) {
    size_t i = 0;
    for (size_t j = 0; j < folded.size() && i < token.size(); ++j)
        if (folded[j] == token[i]) ++i;
    return i == token.size();
}

}  // namespace

FuzzyMatcher::FuzzyMatcher(std::wstring_view query) {
    folded_query_ = fold(trim(query));
    std::wstring_view rest = folded_query_;
    while (!rest.empty()) {
        const size_t start = rest.find_first_not_of(L' ');
        if (start == std::wstring_view::npos) break;
        rest.remove_prefix(start);
        const size_t end = std::min(rest.find(L' '), rest.size());
        std::wstring token(rest.substr(0, std::min(end, kMaxToken)));
        mask_ |= char_mask(token);
        tokens_.push_back(std::move(token));
        rest.remove_prefix(end);
    }
}

std::optional<int> FuzzyMatcher::match(std::wstring_view original, std::wstring_view folded,
                                       std::vector<uint16_t>* positions) const {
    if (tokens_.empty()) return 0;
    if (folded.size() > kMaxTarget) {
        original = original.substr(0, kMaxTarget);
        folded = folded.substr(0, kMaxTarget);
    }
    if (positions) positions->clear();

    int total = 0;
    for (const auto& token : tokens_) {
        auto s = match_token(token, original, folded, positions);
        if (!s) return std::nullopt;
        total += *s;
    }
    if (tokens_.size() == 1 && folded == tokens_[0]) total += kExactBonus;
    // Prefer shorter candidates on ties (fzf does the same).
    total -= static_cast<int>(folded.size() / 16);

    if (positions) {
        std::sort(positions->begin(), positions->end());
        positions->erase(std::unique(positions->begin(), positions->end()), positions->end());
    }
    return total;
}

std::optional<int> FuzzyMatcher::match_token(std::wstring_view token, std::wstring_view original,
                                             std::wstring_view folded,
                                             std::vector<uint16_t>* positions) const {
    const size_t m = token.size();
    const size_t n = folded.size();
    if (m == 0) return 0;
    if (m > n || !is_subsequence(token, folded)) return std::nullopt;

    bonus_.resize(n);
    for (size_t j = 0; j < n; ++j) bonus_[j] = bonus_at(original, j);

    score_.assign(m * n, kNeg);
    from_.assign(m * n, -1);

    // Row 0: the first token character may start anywhere; earlier is slightly better.
    for (size_t j = 0; j < n; ++j) {
        if (folded[j] != token[0]) continue;
        score_[j] = kMatch + bonus_[j] * kFirstCharMultiplier - static_cast<int32_t>(std::min<size_t>(j, 20) / 4);
    }

    for (size_t i = 1; i < m; ++i) {
        const int32_t* prev = &score_[(i - 1) * n];
        int32_t* cur = &score_[i * n];
        int16_t* cur_from = &from_[i * n];
        int32_t gap_best = kNeg;  // best prev[k] - gap penalty, over k <= j - 2
        int32_t gap_from = -1;
        for (size_t j = i; j < n; ++j) {
            if (j >= 2) {
                if (gap_best > kNeg) gap_best -= kGapExtend;
                const int32_t cand = prev[j - 2] > kNeg ? prev[j - 2] - kGapStart : kNeg;
                if (cand > gap_best) {
                    gap_best = cand;
                    gap_from = static_cast<int32_t>(j - 2);
                }
            }
            if (folded[j] != token[i]) continue;
            int32_t best = kNeg;
            int32_t best_from = -1;
            if (prev[j - 1] > kNeg) {
                best = prev[j - 1] + kConsecutive;
                best_from = static_cast<int32_t>(j - 1);
            }
            if (gap_best > best) {
                best = gap_best;
                best_from = gap_from;
            }
            if (best == kNeg) continue;
            cur[j] = best + kMatch + bonus_[j];
            cur_from[j] = static_cast<int16_t>(best_from);
        }
    }

    const int32_t* last = &score_[(m - 1) * n];
    int32_t best = kNeg;
    size_t best_j = 0;
    for (size_t j = m - 1; j < n; ++j) {
        if (last[j] > best) {
            best = last[j];
            best_j = j;
        }
    }
    if (best == kNeg) return std::nullopt;

    if (folded.substr(0, m) == token) best += kPrefixBonus;

    if (positions) {
        size_t j = best_j;
        for (size_t i = m; i-- > 0;) {
            positions->push_back(static_cast<uint16_t>(j));
            if (i == 0) break;
            j = static_cast<size_t>(from_[i * n + j]);
        }
    }
    return best;
}

}  // namespace kamil
