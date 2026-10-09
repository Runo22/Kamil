#include "core/usage.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <locale>
#include <fstream>
#include <sstream>
#include <system_error>

#include "core/text.h"

namespace kamil {

namespace {
constexpr wchar_t kSep = L'\x1F';
}

std::wstring UsageStore::affinity_key(std::wstring_view query, std::wstring_view item) {
    std::wstring k;
    k.reserve(query.size() + 1 + item.size());
    k.append(query);
    k.push_back(kSep);
    k.append(item);
    return k;
}

double UsageStore::decayed(const Score& s, int64_t now) const {
    const double age_days = static_cast<double>(std::max<int64_t>(0, now - s.last)) / 86400.0;
    return s.value * std::exp2(-age_days / half_life_days_);
}

void UsageStore::bump(Score& s, int64_t now) {
    s.value = decayed(s, now) + 1.0;
    s.last = now;
}

void UsageStore::record(std::wstring_view item_key, std::wstring_view folded_query, int64_t now) {
    bump(frecency_[std::wstring(item_key)], now);
    const std::wstring_view q = trim(folded_query);
    for (size_t len = 1; len <= std::min(q.size(), kMaxPrefix); ++len) {
        if (q[len - 1] == L' ') continue;
        bump(affinity_[affinity_key(q.substr(0, len), item_key)], now);
    }
    if (affinity_.size() > kMaxAffinities) prune_affinities();
    dirty_ = true;
}

double UsageStore::frecency(std::wstring_view item_key, int64_t now) const {
    auto it = frecency_.find(std::wstring(item_key));
    return it == frecency_.end() ? 0.0 : decayed(it->second, now);
}

double UsageStore::affinity(std::wstring_view folded_query, std::wstring_view item_key, int64_t now) const {
    const std::wstring_view q = trim(folded_query);
    if (q.empty()) return 0.0;
    auto it = affinity_.find(affinity_key(q.substr(0, std::min(q.size(), kMaxPrefix)), item_key));
    return it == affinity_.end() ? 0.0 : decayed(it->second, now);
}

std::vector<std::wstring> UsageStore::top(size_t n, int64_t now) const {
    std::vector<std::pair<double, const std::wstring*>> all;
    all.reserve(frecency_.size());
    for (const auto& [k, s] : frecency_) all.emplace_back(decayed(s, now), &k);
    const size_t count = std::min(n, all.size());
    std::partial_sort(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(count), all.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && *a.second < *b.second); });
    std::vector<std::wstring> out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) out.push_back(*all[i].second);
    return out;
}

void UsageStore::forget(std::wstring_view item_key) {
    frecency_.erase(std::wstring(item_key));
    for (auto it = affinity_.begin(); it != affinity_.end();) {
        const auto sep = it->first.find(kSep);
        if (sep != std::wstring::npos && std::wstring_view(it->first).substr(sep + 1) == item_key) it = affinity_.erase(it);
        else ++it;
    }
    dirty_ = true;
}

void UsageStore::clear() {
    frecency_.clear();
    affinity_.clear();
    dirty_ = true;
}

void UsageStore::prune_affinities() {
    // Drop the weakest quarter by recency (cheap, deterministic enough).
    std::vector<std::pair<int64_t, std::wstring>> by_age;
    by_age.reserve(affinity_.size());
    for (const auto& [k, s] : affinity_) by_age.emplace_back(s.last, k);
    std::sort(by_age.begin(), by_age.end());
    const size_t drop = affinity_.size() - kMaxAffinities * 3 / 4;
    for (size_t i = 0; i < drop && i < by_age.size(); ++i) affinity_.erase(by_age[i].second);
}

bool UsageStore::load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    frecency_.clear();
    affinity_.clear();
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        // F \t value \t last \t key        |  Q \t value \t last \t query \t key
        std::vector<std::string_view> cols;
        std::string_view rest = line;
        while (true) {
            const size_t tab = rest.find('\t');
            cols.push_back(rest.substr(0, tab));
            if (tab == std::string_view::npos) break;
            rest.remove_prefix(tab + 1);
        }
        if (cols.size() < 4) continue;
        Score s;
        // from_chars is locale independent (a Turkish locale would otherwise expect "1,5").
        if (std::from_chars(cols[1].data(), cols[1].data() + cols[1].size(), s.value).ec != std::errc{}) continue;
        if (std::from_chars(cols[2].data(), cols[2].data() + cols[2].size(), s.last).ec != std::errc{}) continue;
        if (cols[0] == "F") frecency_[widen(cols[3])] = s;
        else if (cols[0] == "Q" && cols.size() >= 5) affinity_[affinity_key(widen(cols[3]), widen(cols[4]))] = s;
    }
    dirty_ = false;
    return true;
}

bool UsageStore::save(const std::filesystem::path& path) const {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "# Kamil usage data (automatic). Deleting it resets what Kamil has learned.\n";
    out.precision(6);
    for (const auto& [k, s] : frecency_) out << "F\t" << s.value << '\t' << s.last << '\t' << narrow(k) << '\n';
    for (const auto& [k, s] : affinity_) {
        const auto sep = k.find(kSep);
        if (sep == std::wstring::npos) continue;
        out << "Q\t" << s.value << '\t' << s.last << '\t' << narrow(std::wstring_view(k).substr(0, sep)) << '\t'
            << narrow(std::wstring_view(k).substr(sep + 1)) << '\n';
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        const std::string data = out.str();
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) return false;
    dirty_ = false;
    return true;
}

}  // namespace kamil
