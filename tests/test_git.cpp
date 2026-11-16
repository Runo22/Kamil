#include <filesystem>
#include <fstream>

#include "core/git.h"
#include "core/text.h"
#include "test.h"

using namespace kamil;
namespace fs = std::filesystem;

TEST(git_parse_head) {
    auto h = parse_head("ref: refs/heads/feature/com-port\n");
    CHECK(h.valid);
    CHECK_EQ(h.branch, std::string("feature/com-port"));
    h = parse_head("3f2a9c1d0b7e6f5a4c3b2a1908f7e6d5c4b3a291\n");
    CHECK(h.valid && h.branch.empty());
    CHECK_EQ(h.detached, std::string("3f2a9c1"));
    CHECK_EQ(h.display(), std::string("3f2a9c1"));
    CHECK(!parse_head("garbage").valid);
}

TEST(git_parse_porcelain) {
    const char* out =
        "## main...origin/main [ahead 2, behind 1]\n"
        "M  src/a.cpp\n"
        " M src/b.cpp\n"
        "MM src/c.cpp\n"
        "A  src/new.cpp\n"
        "UU src/conflict.cpp\n"
        "?? notes.txt\n"
        "?? build/\n";
    const GitStatus s = parse_porcelain_v1(out);
    CHECK(s.valid);
    CHECK_EQ(s.branch, std::string("main"));
    CHECK_EQ(s.upstream, std::string("origin/main"));
    CHECK_EQ(s.ahead, 2);
    CHECK_EQ(s.behind, 1);
    CHECK_EQ(s.staged, 3);
    CHECK_EQ(s.modified, 2);
    CHECK_EQ(s.conflicts, 1);
    CHECK_EQ(s.untracked, 2);
    CHECK_EQ(s.changes(), 8);

    CHECK_EQ(parse_porcelain_v1("## feature\n").branch, std::string("feature"));
    CHECK_EQ(parse_porcelain_v1("## No commits yet on main\n").branch, std::string("main"));
    CHECK(parse_porcelain_v1("## HEAD (no branch)\n").branch.empty());
    CHECK_EQ(parse_porcelain_v1("## dev...origin/dev [behind 5]\r\n").behind, 5);
}

TEST(git_find_repos_and_worktree) {
    const fs::path root = fs::temp_directory_path() / "kamil_test_repos";
    fs::remove_all(root);
    fs::create_directories(root / "a" / ".git");
    fs::create_directories(root / "a" / "sub" / ".git");  // nested: not listed separately
    fs::create_directories(root / "group" / "b" / ".git");
    fs::create_directories(root / "node_modules" / "c" / ".git");  // excluded
    fs::create_directories(root / "deep" / "x" / "y" / "z" / ".git");  // beyond depth 3
    fs::create_directories(root / "wt");
    std::ofstream(root / "wt" / ".git") << "gitdir: ../a/.git/worktrees/wt\n";
    fs::create_directories(root / "a" / ".git" / "worktrees" / "wt");
    std::ofstream(root / "a" / ".git" / "worktrees" / "wt" / "HEAD") << "ref: refs/heads/hotfix\n";

    RepoScanOptions opt;
    opt.max_depth = 3;
    opt.exclude = {L"node_modules"};
    auto repos = find_repos({root}, opt);
    auto has = [&](const fs::path& p) {
        for (const auto& r : repos)
            if (r == p) return true;
        return false;
    };
    CHECK(has(root / "a"));
    CHECK(has(root / "group" / "b"));
    CHECK(has(root / "wt"));
    CHECK(!has(root / "a" / "sub"));
    CHECK(!has(root / "node_modules" / "c"));
    CHECK(!has(root / "deep" / "x" / "y" / "z"));

    auto git_dir = resolve_git_dir(root / "wt");
    CHECK(git_dir.has_value());
    CHECK_EQ(read_head(*git_dir).branch, std::string("hotfix"));
    // CMake roots without git are found when asked for; their subdirectories are not.
    fs::create_directories(root / "plain" / "src");
    std::ofstream(root / "plain" / "CMakeLists.txt") << "project(plain)\n";
    std::ofstream(root / "plain" / "src" / "CMakeLists.txt") << "add_executable(x x.cpp)\n";
    CHECK(!has(root / "plain"));
    opt.include_cmake_roots = true;
    repos = find_repos({root}, opt);
    CHECK(has(root / "plain"));
    CHECK(!has(root / "plain" / "src"));
    fs::remove_all(root);
}

TEST(git_list_branches) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "kamil_test_branches";
    fs::remove_all(root);
    const fs::path git = root / "repo" / ".git";
    auto write = [](const fs::path& p, const std::string& text) {
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
    };
    const std::string sha = "0123456789abcdef0123456789abcdef01234567\n";
    write(git / "HEAD", "ref: refs/heads/main\n");
    write(git / "refs" / "heads" / "main", sha);
    write(git / "refs" / "heads" / "feature" / "com-port", sha);  // nested name
    write(git / "refs" / "heads" / "wip.lock", sha);               // a lock file: not a branch
    write(git / "refs" / "remotes" / "origin" / "HEAD", "ref: refs/remotes/origin/main\n");
    write(git / "refs" / "remotes" / "origin" / "main", sha);
    write(git / "refs" / "tags" / "v1.0", sha);
    write(git / "packed-refs",
          "# pack-refs with: peeled fully-peeled sorted\n" + sha.substr(0, 40) + " refs/heads/release/1.2\n" + sha.substr(0, 40) +
              " refs/remotes/origin/develop\n" + sha.substr(0, 40) + " refs/remotes/origin/main\n" + sha.substr(0, 40) +
              " refs/remotes/upstream/feature/com-port\n" + sha.substr(0, 40) + " refs/tags/v0.9\n^" + sha.substr(0, 40) + "\n");
    GitBranches b = list_branches(git);
    CHECK((b.local == std::vector<std::string>{"feature/com-port", "main", "release/1.2"}));
    CHECK((b.remote == std::vector<std::string>{"origin/develop", "origin/main", "upstream/feature/com-port"}));
    CHECK((remote_only_branches(b) == std::vector<std::string>{"origin/develop"}));

    // A worktree: its own HEAD, the refs of the main repository through "commondir".
    const fs::path wt = git / "worktrees" / "wt1";
    write(wt / "HEAD", "ref: refs/heads/feature/com-port\n");
    write(wt / "commondir", "../..\n");
    GitBranches w = list_branches(wt);
    CHECK(w.local == b.local);
    CHECK(w.remote == b.remote);

    CHECK(list_branches(root / "none").local.empty());
    fs::remove_all(root);
}
