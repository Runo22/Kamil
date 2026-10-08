#include "core/file_index.h"

#include <algorithm>
#include <system_error>

#include "core/text.h"

namespace kamil {

namespace fs = std::filesystem;

namespace {

constexpr int kScriptBonus = 12;
constexpr int kDirPenalty = 4;  // on equal names the runnable file comes first

std::wstring_view extension_of(std::wstring_view name) {
    const size_t dot = name.rfind(L'.');
    return dot == std::wstring_view::npos || dot == 0 ? std::wstring_view() : name.substr(dot);
}

bool matches_any(const std::vector<std::wstring>& globs, std::wstring_view folded) {
    return std::any_of(globs.begin(), globs.end(), [&](const std::wstring& g) { return glob_match(g, folded); });
}

}  // namespace

bool is_script_extension(std::wstring_view ext) {
    static constexpr std::wstring_view kExt[] = {L".bat", L".cmd", L".ps1", L".py", L".pyw", L".sh", L".exe", L".lnk", L".vbs", L".js"};
    return std::find(std::begin(kExt), std::end(kExt), ext) != std::end(kExt);
}

void FileIndex::add(uint32_t dir, std::wstring_view name, bool is_dir, int bonus) {
    if (name.size() > 0xFFFF) return;
    Entry e{};
    e.name_off = static_cast<uint32_t>(names_.size());
    e.name_len = static_cast<uint16_t>(name.size());
    e.dir = dir;
    e.bonus = static_cast<int8_t>(std::clamp(bonus, -100, 100));
    names_.append(name);
    const size_t start = folded_.size();
    folded_.resize(start + name.size());
    for (size_t i = 0; i < name.size(); ++i) folded_[start + i] = fold_char(name[i]);
    const std::wstring_view folded(folded_.data() + start, name.size());
    e.mask = char_mask(folded);
    if (is_dir) e.flags |= kDir;
    else if (is_script_extension(extension_of(folded))) e.flags |= kScript;
    entries_.push_back(e);
}

FileIndex FileIndex::build(const std::vector<IndexRoot>& roots, const IndexOptions& opt, const std::atomic<bool>* cancel) {
    FileIndex index;
    struct Pending {
        fs::path dir;
        size_t depth;
    };
    for (const auto& root : roots) {
        std::vector<Pending> stack{{root.path, 0}};
        while (!stack.empty()) {
            if ((cancel && cancel->load()) || index.entries_.size() >= opt.max_entries) {
                index.truncated_ = index.entries_.size() >= opt.max_entries;
                return index;
            }
            Pending cur = std::move(stack.back());
            stack.pop_back();
            std::error_code ec;
            fs::directory_iterator it(cur.dir, fs::directory_options::skip_permission_denied, ec);
            if (ec) continue;
            const uint32_t dir_id = static_cast<uint32_t>(index.dirs_.size());
            index.dirs_.push_back(cur.dir.wstring());
            std::vector<fs::path> subdirs;
            for (; it != fs::directory_iterator(); it.increment(ec)) {
                if (ec) break;
                const std::wstring name = it->path().filename().wstring();
                if (name.empty() || name[0] == L'.' || name[0] == L'$' || name[0] == L'~') continue;  // hidden / temp
                std::error_code e2;
                const bool is_dir = it->is_directory(e2) && !it->is_symlink(e2);
                const std::wstring folded = fold(name);
                if (is_dir) {
                    if (matches_any(opt.exclude_dirs, folded)) continue;
                    index.add(dir_id, name, true, root.bonus);
                    if (cur.depth + 1 < root.max_depth) subdirs.push_back(it->path());
                } else {
                    if (!root.include.empty() && !matches_any(root.include, folded)) continue;
                    index.add(dir_id, name, false, root.bonus);
                }
            }
            for (auto s = subdirs.rbegin(); s != subdirs.rend(); ++s) stack.push_back({*s, cur.depth + 1});
        }
    }
    return index;
}

std::wstring_view FileIndex::name(uint32_t e) const { return std::wstring_view(names_).substr(entries_[e].name_off, entries_[e].name_len); }

std::wstring_view FileIndex::folded_name(uint32_t e) const {
    return std::wstring_view(folded_).substr(entries_[e].name_off, entries_[e].name_len);
}

std::wstring FileIndex::path(uint32_t e) const {
    const std::wstring& d = dir(e);
    std::wstring p = d;
    if (!p.empty() && p.back() != L'\\' && p.back() != L'/') p.push_back(static_cast<wchar_t>(fs::path::preferred_separator));
    p.append(name(e));
    return p;
}

std::vector<FileIndex::Match> FileIndex::search(const FuzzyMatcher& matcher, size_t limit) const {
    std::vector<Match> matches;
    if (matcher.empty() || limit == 0) return matches;
    const uint64_t qmask = matcher.mask();
    for (uint32_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        if ((e.mask & qmask) != qmask) continue;
        auto score = matcher.match(name(i), folded_name(i));
        if (!score) continue;
        int total = *score + e.bonus;
        if (e.flags & kScript) total += kScriptBonus;
        if (e.flags & kDir) total -= kDirPenalty;
        matches.push_back({i, total});
    }
    auto better = [this](const Match& a, const Match& b) {
        if (a.score != b.score) return a.score > b.score;
        return entries_[a.entry].name_len < entries_[b.entry].name_len;
    };
    if (matches.size() > limit) {
        std::nth_element(matches.begin(), matches.begin() + static_cast<std::ptrdiff_t>(limit), matches.end(), better);
        matches.resize(limit);
    }
    std::sort(matches.begin(), matches.end(), better);
    return matches;
}

}  // namespace kamil
