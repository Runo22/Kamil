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
    fs::remove_all(root);
}
