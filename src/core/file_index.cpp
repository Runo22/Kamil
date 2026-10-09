#include "core/file_index.h"

#include <algorithm>
#include <cstring>
#include <system_error>
#include <thread>

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

// Map key of a folder: folded, backslashes, no trailing separator.
std::wstring dir_key(std::wstring_view path) {
    std::wstring out = fold(path);
    for (auto& c : out)
        if (c == L'/') c = L'\\';
    while (out.size() > 1 && out.back() == L'\\') out.pop_back();
    return out;
}

// Splits "C:\a\b.txt" into ("C:\a", "b.txt").
std::pair<std::wstring_view, std::wstring_view> split_path(std::wstring_view path) {
    while (path.size() > 1 && (path.back() == L'\\' || path.back() == L'/')) path.remove_suffix(1);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos) return {std::wstring_view(), path};
    return {path.substr(0, slash), path.substr(slash + 1)};
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
    if (auto it = children_.find(dir); it != children_.end()) it->second.push_back(static_cast<uint32_t>(entries_.size() - 1));
}

uint32_t FileIndex::add_dir(std::wstring path, uint16_t root) {
    const uint32_t id = static_cast<uint32_t>(dirs_.size());
    dir_ids_.emplace(dir_key(path), id);
    dirs_.push_back(std::move(path));
    dir_root_.push_back(root);
    return id;
}

FileIndex FileIndex::build(const std::vector<IndexRoot>& roots, const IndexOptions& opt, const std::atomic<bool>* cancel) {
    FileIndex index;
    index.roots_ = roots;
    struct Pending {
        fs::path dir;
        size_t depth;
    };
    for (size_t root_id = 0; root_id < roots.size(); ++root_id) {
        const IndexRoot& root = roots[root_id];
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
            const uint32_t dir_id = index.add_dir(cur.dir.wstring(), static_cast<uint16_t>(root_id));
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

std::vector<FileIndex::Match> FileIndex::search(const FuzzyMatcher& matcher, size_t limit, SearchState* state) const {
    std::vector<Match> matches;
    if (matcher.empty() || limit == 0) return matches;
    const uint64_t qmask = matcher.mask();
    const std::wstring& query = matcher.folded_query();
    const bool narrow = state && state->generation == generation_ && !state->query.empty() && query.size() >= state->query.size() &&
                        query.compare(0, state->query.size(), state->query) == 0;
    const size_t count = narrow ? state->matched.size() : entries_.size();
    auto entry_at = [&](size_t i) { return narrow ? state->matched[i] : static_cast<uint32_t>(i); };

    auto scan = [&](const FuzzyMatcher& m, size_t from, size_t to, std::vector<Match>& out) {
        for (size_t k = from; k < to; ++k) {
            const uint32_t i = entry_at(k);
            const Entry& e = entries_[i];
            if ((e.mask & qmask) != qmask || (e.flags & kRemoved)) continue;
            auto score = m.match(name(i), folded_name(i));
            if (!score) continue;
            int total = *score + e.bonus;
            if (e.flags & kScript) total += kScriptBonus;
            if (e.flags & kDir) total -= kDirPenalty;
            out.push_back({i, total});
        }
    };
    constexpr size_t kParallelMin = 8000;  // below this, starting threads costs more than it saves
    const size_t threads = count < kParallelMin ? 1 : std::clamp<size_t>(std::thread::hardware_concurrency(), 1, 8);
    if (threads == 1) {
        scan(matcher, 0, count, matches);
    } else {
        // Each thread gets its own matcher (it keeps scratch buffers) and a contiguous slice;
        // slices are joined in order, so the matched list stays sorted by entry.
        std::vector<std::vector<Match>> parts(threads);
        std::vector<std::thread> pool;
        const size_t step = (count + threads - 1) / threads;
        for (size_t t = 1; t < threads; ++t)
            pool.emplace_back([&, t] { scan(FuzzyMatcher(query), std::min(count, t * step), std::min(count, (t + 1) * step), parts[t]); });
        scan(matcher, 0, std::min(count, step), parts[0]);
        for (auto& th : pool) th.join();
        for (auto& p : parts) matches.insert(matches.end(), p.begin(), p.end());
    }
    if (state) {
        state->query = query;
        state->generation = generation_;
        state->matched.clear();
        state->matched.reserve(matches.size());
        for (const auto& m : matches) state->matched.push_back(m.entry);
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

FileIndex FileIndex::from_paths(const std::vector<std::wstring>& paths, const IndexRoot& root) {
    FileIndex index;
    index.roots_ = {root};
    index.add_dir(root.path.wstring(), 0);
    for (const auto& p : paths) {
        const auto [dir, name] = split_path(p);
        auto it = index.dir_ids_.find(dir_key(dir));
        const uint32_t dir_id = it != index.dir_ids_.end() ? it->second : index.add_dir(std::wstring(dir), 0);
        index.add(dir_id, name, false, root.bonus);
    }
    return index;
}

// ---------------------------------------------------------------------------------------------
// Incremental updates

const std::vector<uint32_t>& FileIndex::children(uint32_t dir) const {
    auto it = children_.find(dir);
    if (it != children_.end()) return it->second;
    std::vector<uint32_t> list;  // one pass over the index, then kept up to date by add()
    for (uint32_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].dir == dir) list.push_back(i);
    return children_.emplace(dir, std::move(list)).first->second;
}

std::optional<uint32_t> FileIndex::find_entry(std::wstring_view path) const {
    const auto [dir, name] = split_path(path);
    const auto it = dir_ids_.find(dir_key(dir));
    if (it == dir_ids_.end() || name.empty()) return std::nullopt;
    const std::wstring folded = fold(name);
    for (const uint32_t i : children(it->second))
        if (!(entries_[i].flags & kRemoved) && folded_name(i) == folded) return i;
    return std::nullopt;
}

uint64_t FileIndex::new_generation() {
    static std::atomic<uint64_t> counter{0};
    return ++counter;
}

bool FileIndex::add_file(std::wstring_view path) {
    const auto [dir, name] = split_path(path);
    if (name.empty() || name[0] == L'.' || name[0] == L'$' || name[0] == L'~') return false;
    const auto it = dir_ids_.find(dir_key(dir));
    if (it == dir_ids_.end() || find_entry(path)) return false;
    const IndexRoot& root = roots_[dir_root_[it->second]];
    if (!root.include.empty() && !matches_any(root.include, fold(name))) return false;
    add(it->second, name, false, root.bonus);
    generation_ = new_generation();
    return true;
}

bool FileIndex::remove(std::wstring_view path) {
    const auto e = find_entry(path);
    if (!e) return false;
    entries_[*e].flags |= kRemoved;
    ++removed_;
    generation_ = new_generation();
    return true;
}

bool FileIndex::is_indexed_dir(std::wstring_view path) const { return dir_ids_.count(dir_key(path)) != 0; }

bool FileIndex::under_root(std::wstring_view path) const {
    const std::wstring key = dir_key(path);
    for (const auto& r : roots_) {
        const std::wstring root = dir_key(r.path.wstring());
        if (key.size() >= root.size() && key.compare(0, root.size(), root) == 0 && (key.size() == root.size() || key[root.size()] == L'\\'))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Disk cache

namespace {

constexpr uint32_t kMagic = 0x4946'4D4B;  // "KMFI"
constexpr uint32_t kVersion = 1;

uint64_t fnv(uint64_t h, const void* data, size_t n) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

class Writer {
public:
    template <class T>
    void put(const T& v) {
        out.append(reinterpret_cast<const char*>(&v), sizeof(T));
    }
    void put_wstr(std::wstring_view s) {
        put(static_cast<uint32_t>(s.size()));
        out.append(reinterpret_cast<const char*>(s.data()), s.size() * sizeof(wchar_t));
    }
    std::string out;
};

class Reader {
public:
    explicit Reader(std::string_view d) : data(d) {}
    template <class T>
    bool get(T* v) {
        if (data.size() - pos < sizeof(T)) return false;
        std::memcpy(v, data.data() + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }
    bool get_wstr(std::wstring* s) {
        uint32_t n = 0;
        if (!get(&n) || (data.size() - pos) / sizeof(wchar_t) < n) return false;
        s->resize(n);
        std::memcpy(s->data(), data.data() + pos, n * sizeof(wchar_t));
        pos += n * sizeof(wchar_t);
        return true;
    }
    std::string_view data;
    size_t pos = 0;
};

}  // namespace

uint64_t FileIndex::config_hash(const std::vector<IndexRoot>& roots, const IndexOptions& options) {
    uint64_t h = 1469598103934665603ull;
    auto add_str = [&](std::wstring_view s) {
        h = fnv(h, s.data(), s.size() * sizeof(wchar_t));
        h = fnv(h, "|", 1);
    };
    for (const auto& r : roots) {
        add_str(r.path.wstring());
        h = fnv(h, &r.bonus, sizeof(r.bonus));
        h = fnv(h, &r.max_depth, sizeof(r.max_depth));
        for (const auto& g : r.include) add_str(g);
        add_str(L"#");
    }
    for (const auto& g : options.exclude_dirs) add_str(g);
    h = fnv(h, &options.max_entries, sizeof(options.max_entries));
    const uint32_t wchar_size = sizeof(wchar_t);
    return fnv(h, &wchar_size, sizeof(wchar_size));
}

std::string FileIndex::serialize(uint64_t hash) const {
    Writer w;
    w.put(kMagic);
    w.put(kVersion);
    w.put(hash);
    w.put(static_cast<uint8_t>(truncated_));
    w.put(static_cast<uint32_t>(dirs_.size()));
    for (size_t i = 0; i < dirs_.size(); ++i) {
        w.put_wstr(dirs_[i]);
        w.put(dir_root_[i]);
    }
    w.put_wstr(names_);
    uint32_t live = 0;
    for (const auto& e : entries_) live += !(e.flags & kRemoved);
    w.put(live);
    for (const auto& e : entries_) {
        if (e.flags & kRemoved) continue;
        w.put(e.name_off);
        w.put(e.name_len);
        w.put(e.flags);
        w.put(e.bonus);
        w.put(e.dir);
    }
    return std::move(w.out);
}

std::optional<FileIndex> FileIndex::deserialize(std::string_view data, uint64_t hash, const std::vector<IndexRoot>& roots) {
    Reader r(data);
    uint32_t magic = 0, version = 0, dirs = 0, count = 0;
    uint64_t stored_hash = 0;
    uint8_t truncated = 0;
    if (!r.get(&magic) || magic != kMagic || !r.get(&version) || version != kVersion || !r.get(&stored_hash) || stored_hash != hash ||
        !r.get(&truncated) || !r.get(&dirs))
        return std::nullopt;
    FileIndex index;
    index.roots_ = roots;
    index.truncated_ = truncated != 0;
    for (uint32_t i = 0; i < dirs; ++i) {
        std::wstring dir;
        uint16_t root = 0;
        if (!r.get_wstr(&dir) || !r.get(&root) || root >= roots.size()) return std::nullopt;
        index.add_dir(std::move(dir), root);
    }
    if (!r.get_wstr(&index.names_) || !r.get(&count)) return std::nullopt;
    index.folded_.resize(index.names_.size());
    for (size_t i = 0; i < index.names_.size(); ++i) index.folded_[i] = fold_char(index.names_[i]);
    index.entries_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Entry e{};
        if (!r.get(&e.name_off) || !r.get(&e.name_len) || !r.get(&e.flags) || !r.get(&e.bonus) || !r.get(&e.dir)) return std::nullopt;
        if (e.dir >= dirs || static_cast<size_t>(e.name_off) + e.name_len > index.names_.size()) return std::nullopt;
        e.mask = char_mask(std::wstring_view(index.folded_).substr(e.name_off, e.name_len));
        index.entries_.push_back(e);
    }
    return index;
}

}  // namespace kamil
