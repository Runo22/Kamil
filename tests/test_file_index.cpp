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

TEST(file_index_incremental_and_cache) {
    const fs::path root = fs::temp_directory_path() / "kamil_test_index_inc";
    fs::remove_all(root);
    fs::create_directories(root / "tools" / "node_modules");
    std::ofstream(root / "tools" / "old.bat") << "x";

    IndexRoot r;
    r.path = root;
    r.max_depth = 4;
    IndexOptions opt;
    opt.exclude_dirs = {L"node_modules"};
    FileIndex idx = FileIndex::build({r}, opt);
    auto count = [&](FileIndex& i, std::wstring_view q) { return i.search(FuzzyMatcher(q), 10).size(); };
    CHECK_EQ(count(idx, L"newscript"), 0u);

    const std::wstring added = (root / "tools" / "newscript.ps1").wstring();
    CHECK(idx.add_file(added));
    CHECK(!idx.add_file(added));  // no duplicates
    CHECK_EQ(count(idx, L"newscript"), 1u);
    CHECK(!idx.add_file((root / "tools" / "node_modules" / "x.js").wstring()));  // excluded folder is not indexed
    CHECK(!idx.add_file((root / "elsewhere" / "y.txt").wstring()));
    CHECK(idx.is_indexed_dir((root / "tools").wstring()));
    CHECK(idx.under_root((root / "anything" / "deeper").wstring()));
    CHECK(!idx.under_root((fs::temp_directory_path() / "kamil_other").wstring()));

    CHECK(idx.remove((root / "tools" / "old.bat").wstring()));
    CHECK_EQ(count(idx, L"old.bat"), 0u);
    CHECK(!idx.remove((root / "tools" / "old.bat").wstring()));

    // Disk cache round trip keeps the incremental state; other settings reject it.
    const uint64_t hash = FileIndex::config_hash({r}, opt);
    const std::string blob = idx.serialize(hash);
    auto loaded = FileIndex::deserialize(blob, hash, {r});
    CHECK(loaded.has_value());
    CHECK_EQ(loaded->size(), idx.size());
    CHECK_EQ(count(*loaded, L"newscript"), 1u);
    CHECK_EQ(count(*loaded, L"old.bat"), 0u);
    CHECK(loaded->add_file((root / "tools" / "later.py").wstring()));  // still updatable after loading
    IndexOptions other = opt;
    other.max_entries = 10;
    CHECK(!FileIndex::deserialize(blob, FileIndex::config_hash({r}, other), {r}).has_value());
    CHECK(!FileIndex::deserialize(blob.substr(0, blob.size() / 2), hash, {r}).has_value());

    fs::remove_all(root);
}

TEST(file_index_narrowing_matches_full_search) {
    std::vector<std::wstring> paths;
    const wchar_t* const words[] = {L"build", L"deploy", L"sensor", L"flash", L"Çalışma", L"notes", L"debug"};
    for (int i = 0; i < 60'000; ++i)  // above the parallel threshold
        paths.push_back(std::wstring(L"C:\\src\\d") + std::to_wstring(i % 97) + L"\\" + words[i % 7] + L"_" + words[(i / 7) % 7] +
                        std::to_wstring(i) + L".txt");
    IndexRoot root;
    root.path = L"C:\\src";
    const FileIndex idx = FileIndex::from_paths(paths, root);
    CHECK_EQ(idx.size(), 60'000u);
    FileIndex::SearchState state;
    const std::wstring typed = L"dep fla 12";
    for (size_t n = 1; n <= typed.size(); ++n) {
        const std::wstring q = typed.substr(0, n);
        const auto narrowed = idx.search(FuzzyMatcher(q), 20, &state);
        const auto full = idx.search(FuzzyMatcher(q), 20);
        CHECK_EQ(narrowed.size(), full.size());
        for (size_t i = 0; i < narrowed.size() && i < full.size(); ++i) CHECK_EQ(narrowed[i].score, full[i].score);
    }
    CHECK(!state.matched.empty());
    // A query that does not extend the previous one searches everything again.
    CHECK(!idx.search(FuzzyMatcher(L"calisma"), 5, &state).empty());
}
