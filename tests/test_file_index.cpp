#include <filesystem>
#include <fstream>

#include "core/file_index.h"
#include "core/text.h"
#include "test.h"

using namespace kamil;
namespace fs = std::filesystem;

TEST(file_index_build_and_search) {
    const fs::path root = fs::temp_directory_path() / "kamil_test_index";
    fs::remove_all(root);
    auto touch = [&](const fs::path& rel) {
        fs::create_directories((root / rel).parent_path());
        std::ofstream(root / rel) << "x";
    };
    touch("tools/deploy.bat");
    touch("tools/deploy_notes.txt");
    touch("tools/py/flash_device.pyw");
    touch("tools/py/sub/deep/deeper/too_deep.py");
    touch("tools/node_modules/pkg/index.js");
    touch("tools/.git/HEAD");
#ifndef __MINGW32__  // the MinGW compile-check build under Wine cannot create non-ANSI names; MSVC CI covers it
    fs::create_directories(root / L"belgeler" / widen("Çalışma Planı"));
#else
    fs::create_directories(root / L"belgeler");
#endif

    IndexRoot r;
    r.path = root;
    r.bonus = 30;
    r.max_depth = 4;
    IndexOptions opt;
    opt.exclude_dirs = {L"node_modules"};
    FileIndex idx = FileIndex::build({r}, opt);

    auto find = [&](std::wstring_view q) {
        FuzzyMatcher m(q);
        return idx.search(m, 10);
    };
    auto names = [&](const std::vector<FileIndex::Match>& ms) {
        std::vector<std::wstring> out;
        for (const auto& m : ms) out.emplace_back(idx.name(m.entry));
        return out;
    };

    auto hits = find(L"deploy");
    CHECK(hits.size() >= 2);
    CHECK(std::wstring(idx.name(hits[0].entry)) == L"deploy.bat");  // runnable script first
    CHECK(idx.is_script(hits[0].entry));
    CHECK(idx.path(hits[0].entry) == (root / "tools" / "deploy.bat").wstring());
    CHECK_EQ(hits[0].score >= 30, true);  // includes the folder priority

    CHECK(!find(L"flash").empty());
    CHECK(find(L"too_deep").empty());     // beyond max_depth
    CHECK(find(L"index.js").empty());     // excluded directory
    CHECK(find(L"HEAD").empty());         // hidden .git directory
#ifndef __MINGW32__
    auto plan = names(find(L"calisma plan"));
    CHECK(!plan.empty() && plan[0] == widen("Çalışma Planı"));
#else
    (void)names;
#endif

    // Folders are indexed too.
    bool folder = false;
    for (const auto& m : find(L"belgeler")) folder |= idx.is_dir(m.entry);
    CHECK(folder);

    // include filter: only scripts
    r.include = {L"*.bat", L"*.pyw"};
    FileIndex scripts = FileIndex::build({r}, opt);
    FuzzyMatcher m(L"deploy");
    auto only = scripts.search(m, 10);
    CHECK_EQ(only.size(), 1u);

    fs::remove_all(root);
}
