#include "core/git.h"

#include <charconv>
#include <fstream>
#include <sstream>
#include <system_error>

#include "core/text.h"

namespace kamil {

namespace fs = std::filesystem;

namespace {

std::string read_small_file(const fs::path& p, size_t limit = 4096) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::string s(limit, '\0');
    in.read(s.data(), static_cast<std::streamsize>(limit));
    s.resize(static_cast<size_t>(in.gcount()));
    return s;
}

int parse_int(std::string_view s) {
    int v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

}  // namespace

std::optional<fs::path> resolve_git_dir(const fs::path& worktree) {
    std::error_code ec;
    const fs::path dot_git = worktree / ".git";
    const auto st = fs::status(dot_git, ec);
    if (ec) return std::nullopt;
    if (fs::is_directory(st)) return dot_git;
    if (!fs::is_regular_file(st)) return std::nullopt;
    const std::string content = read_small_file(dot_git);
    constexpr std::string_view kPrefix = "gitdir:";
    std::string_view line = trim(std::string_view(content));
    if (line.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;
    line = trim(line.substr(kPrefix.size()));
    if (const auto nl = line.find_first_of("\r\n"); nl != std::string_view::npos) line = trim(line.substr(0, nl));
    fs::path dir = fs::path(widen(line));
    if (dir.is_relative()) dir = worktree / dir;
    return dir.lexically_normal();
}

GitHead parse_head(std::string_view content) {
    GitHead h;
    std::string_view line = trim(content);
    if (const auto nl = line.find_first_of("\r\n"); nl != std::string_view::npos) line = trim(line.substr(0, nl));
    constexpr std::string_view kRef = "ref:";
    if (line.substr(0, kRef.size()) == kRef) {
        std::string_view ref = trim(line.substr(kRef.size()));
        constexpr std::string_view kHeads = "refs/heads/";
        if (ref.substr(0, kHeads.size()) == kHeads) ref.remove_prefix(kHeads.size());
        h.branch = std::string(ref);
        h.valid = !h.branch.empty();
        return h;
    }
    if (line.size() >= 7) {
        bool hex = true;
        for (char c : line) hex &= (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (hex) {
            h.detached = std::string(line.substr(0, 7));
            h.valid = true;
        }
    }
    return h;
}

GitHead read_head(const fs::path& git_dir) { return parse_head(read_small_file(git_dir / "HEAD", 512)); }

GitStatus parse_porcelain_v1(std::string_view out) {
    GitStatus s;
    s.valid = true;
    std::istringstream in{std::string(out)};
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 2) continue;
        if (line.rfind("## ", 0) == 0) {
            // "## main...origin/main [ahead 1, behind 2]", "## HEAD (no branch)", "## No commits yet on main"
            std::string_view h = std::string_view(line).substr(3);
            constexpr std::string_view kNoCommits = "No commits yet on ";
            if (h.substr(0, kNoCommits.size()) == kNoCommits) {
                s.branch = std::string(h.substr(kNoCommits.size()));
                continue;
            }
            std::string_view tracking;
            if (const auto br = h.find(" ["); br != std::string_view::npos) {
                tracking = h.substr(br + 2);
                h = h.substr(0, br);
            }
            if (const auto dots = h.find("..."); dots != std::string_view::npos) {
                s.upstream = std::string(h.substr(dots + 3));
                h = h.substr(0, dots);
            }
            s.branch = h == "HEAD (no branch)" ? std::string() : std::string(h);
            if (const auto a = tracking.find("ahead "); a != std::string_view::npos) s.ahead = parse_int(tracking.substr(a + 6));
            if (const auto b = tracking.find("behind "); b != std::string_view::npos) s.behind = parse_int(tracking.substr(b + 7));
            continue;
        }
        const char x = line[0], y = line[1];
        if (x == '?' && y == '?') {
            ++s.untracked;
        } else if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D')) {
            ++s.conflicts;
        } else {
            if (x != ' ' && x != '!') ++s.staged;
            if (y != ' ' && y != '!') ++s.modified;
        }
    }
    return s;
}

std::vector<fs::path> find_repos(const std::vector<fs::path>& roots, const RepoScanOptions& opt) {
    std::vector<fs::path> repos;
    struct Pending {
        fs::path dir;
        size_t depth;
    };
    for (const auto& root : roots) {
        std::vector<Pending> stack{{root, 0}};
        while (!stack.empty() && repos.size() < opt.max_repos) {
            Pending cur = std::move(stack.back());
            stack.pop_back();
            std::error_code ec;
            if (fs::exists(cur.dir / ".git", ec) || (opt.include_cmake_roots && fs::exists(cur.dir / "CMakeLists.txt", ec))) {
                repos.push_back(cur.dir);
                continue;
            }
            if (cur.depth >= opt.max_depth) continue;
            fs::directory_iterator it(cur.dir, fs::directory_options::skip_permission_denied, ec);
            if (ec) continue;
            std::vector<fs::path> children;
            for (; it != fs::directory_iterator(); it.increment(ec)) {
                if (ec) break;
                std::error_code e2;
                if (!it->is_directory(e2) || it->is_symlink(e2)) continue;
                const std::wstring name = it->path().filename().wstring();
                if (name.empty() || name[0] == L'.' || name[0] == L'$') continue;
                const std::wstring folded = fold(name);
                bool skip = false;
                for (const auto& pattern : opt.exclude) {
                    if (glob_match(pattern, folded)) {
                        skip = true;
                        break;
                    }
                }
                if (!skip) children.push_back(it->path());
            }
            // Reverse so that the stack visits children in directory order.
            for (auto c = children.rbegin(); c != children.rend(); ++c) stack.push_back({*c, cur.depth + 1});
        }
    }
    return repos;
}

}  // namespace kamil
