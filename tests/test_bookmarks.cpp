#include <filesystem>
#include <fstream>

#include "core/bookmarks.h"
#include "core/file_index.h"
#include "core/fuzzy.h"
#include "core/search.h"
#include "core/settings_schema.h"
#include "core/text.h"
#include "test.h"

using namespace kamil;
namespace fs = std::filesystem;

namespace {

const std::wstring kHome = L"C:\\Users\\tuna";

// Stand-in for ExpandEnvironmentStrings.
std::wstring fake_expand(const std::wstring& s) {
    std::wstring out = s;
    auto replace = [&](const std::wstring& var, const std::wstring& value) {
        for (size_t pos; (pos = out.find(var)) != std::wstring::npos;) out.replace(pos, var.size(), value);
    };
    replace(L"%USERPROFILE%", kHome);
    replace(L"%LOCALAPPDATA%", kHome + L"\\AppData\\Local");
    return out;  // unknown %VARS% stay as written, like ExpandEnvironmentStrings
}

Object bookmark(const char* name, const char* path, const char* keyword = nullptr) {
    Object o;
    if (name) o.fields.push_back({"name", Value(name)});
    if (path) o.fields.push_back({"path", Value(path)});
    if (keyword) o.fields.push_back({"keyword", Value(keyword)});
    return o;
}

Item folder_item(const std::wstring& title, const std::wstring& path, const std::wstring& keywords = {}) {
    Item it;
    it.kind = ItemKind::Folder;
    it.key = L"file:" + path;
    it.title = title;
    it.subtitle = path;
    it.path = path;
    it.keywords = keywords;
    it.prepare();
    return it;
}

}  // namespace

// ---- paths ---------------------------------------------------------------------------------

TEST(bookmark_path_normalization) {
    auto n = [](const char* raw) { return normalize_bookmark_path(widen(raw), kHome, fake_expand); };
    CHECK_EQ(n("%USERPROFILE%\\.conan"), kHome + L"\\.conan");
    CHECK_EQ(n("~\\.conan"), kHome + L"\\.conan");
    CHECK_EQ(n("~/.conan/data/"), kHome + L"\\.conan\\data");  // '/' -> '\\', trailing separator dropped
    CHECK_EQ(n("~"), kHome);
    CHECK_EQ(n("~tuna\\x"), L"~tuna\\x");                        // only "~" and "~\\" mean the user folder
    CHECK_EQ(n("  'D:\\tools\\flash.py'  "), L"D:\\tools\\flash.py");  // spaces and quotes
    CHECK_EQ(n("\"D:\\a b\\c\""), L"D:\\a b\\c");
    CHECK_EQ(n("C:\\"), L"C:\\");                                // a drive root keeps its separator
    CHECK_EQ(n("C:/"), L"C:\\");
    CHECK_EQ(n("D:\\src\\\\\\"), L"D:\\src");
    CHECK_EQ(n("\\\\server\\share\\"), L"\\\\server\\share");
    CHECK_EQ(n("%LOCALAPPDATA%\\Temp"), kHome + L"\\AppData\\Local\\Temp");
    CHECK_EQ(n("%NOPE%\\x"), L"%NOPE%\\x");
    CHECK(n("").empty());
    CHECK(n("   ").empty());
    CHECK(n("''").empty());
    CHECK(n("\\").empty());
    CHECK(n("/").empty());
    // Without a home folder "~" stays (it cannot be resolved) and no expander is fine.
    CHECK_EQ(normalize_bookmark_path(L"~\\x", L"", nullptr), L"~\\x");
    CHECK_EQ(normalize_bookmark_path(L"%USERPROFILE%", kHome, nullptr), L"%USERPROFILE%");
}

TEST(bookmark_path_parts) {
    CHECK_EQ(last_path_part(L"C:\\Users\\tuna\\.conan"), L".conan");
    CHECK_EQ(last_path_part(L"C:\\Users\\tuna\\.conan\\"), L".conan");
    CHECK_EQ(last_path_part(L"D:/tools/flash.py"), L"flash.py");
    CHECK_EQ(last_path_part(L"C:\\"), L"C:");
    CHECK_EQ(last_path_part(L"relative"), L"relative");
    CHECK(last_path_part(L"").empty());
    CHECK(is_unc_path(L"\\\\server\\share"));
    CHECK(is_unc_path(L"//server/share"));
    CHECK(!is_unc_path(L"C:\\x"));
    CHECK(!is_unc_path(L"\\x"));
    CHECK(!is_unc_path(L""));
}

// ---- parsing ---------------------------------------------------------------------------------

TEST(bookmark_parse) {
    Value::Objects objs{
        bookmark("Conan cache", "%USERPROFILE%\\.conan", "conan"),
        bookmark(nullptr, "D:\\tools\\flash.py"),               // no name: the file name
        bookmark("  ", "D:\\tools\\"),                           // blank name: the folder name
        bookmark("Again", "%userprofile%\\.CONAN\\"),            // the same path again: skipped
        bookmark("No path", nullptr),                            // skipped
        bookmark("Blank path", "  "),                            // skipped
        bookmark("Root", "C:\\", " Root  "),
        bookmark(" Çalışma ", "~\\Çalışma", " ÇAL "),
    };
    objs[3].fields[1].second = Value(narrow(kHome + L"\\.CONAN\\"));  // differs only in case
    const auto bs = parse_bookmarks(objs, kHome, fake_expand);
    CHECK_EQ(bs.size(), 5u);
    if (bs.size() != 5) return;
    CHECK_EQ(bs[0].name, L"Conan cache");
    CHECK_EQ(bs[0].path, kHome + L"\\.conan");
    CHECK_EQ(bs[0].keyword_folded, L"conan");
    CHECK_EQ(bs[1].name, L"flash.py");
    CHECK(bs[1].keyword_folded.empty());
    CHECK_EQ(bs[2].name, L"tools");
    CHECK_EQ(bs[2].path, L"D:\\tools");
    CHECK_EQ(bs[3].name, L"Root");
    CHECK_EQ(bs[3].path, L"C:\\");
    CHECK_EQ(bs[3].keyword_folded, L"root");
    CHECK_EQ(bs[4].name, widen("Çalışma"));
    CHECK_EQ(bs[4].keyword_folded, L"cal");  // folded like the search query
    CHECK(parse_bookmarks({}, kHome, fake_expand).empty());
}

TEST(bookmark_keywords_find_the_file_name) {
    Bookmark b{L"Conan cache", kHome + L"\\.conan", L""};
    CHECK_EQ(bookmark_keywords(b), L".conan conan");
    b = {L"Flash device", L"D:\\tools\\flash.py", L""};
    CHECK_EQ(bookmark_keywords(b), L"flash.py");
    b = {L"flash.py", L"D:\\tools\\flash.py", L""};  // the name is the file name: nothing to add
    CHECK(bookmark_keywords(b).empty());
    b = {L"Flash.PY", L"D:\\tools\\flash.py", L""};
    CHECK(bookmark_keywords(b).empty());
}

// ---- settings ----------------------------------------------------------------------------------

TEST(bookmark_settings) {
    const char* yaml =
        "search:\n"
        "  bookmarks:\n"
        "    - name: Conan cache\n"
        "      path: '%USERPROFILE%\\.conan'\n"
        "      keyword: conan\n"
        "    - { path: '~\\.conan\\profiles' }\n";
    auto r = load_settings_text(builtin_schema(), yaml, "settings.yaml");
    for (const auto& d : r.diagnostics) std::printf("    %s\n", d.to_string().c_str());
    CHECK(r.diagnostics.empty());
    const auto& objs = r.settings->get_objects(keys::kBookmarks);
    CHECK_EQ(objs.size(), 2u);
    const auto bs = parse_bookmarks(objs, kHome, fake_expand);
    CHECK_EQ(bs.size(), 2u);
    if (bs.size() == 2) {
        CHECK_EQ(bs[0].path, kHome + L"\\.conan");
        CHECK_EQ(bs[1].name, L"profiles");
        CHECK_EQ(bs[1].path, kHome + L"\\.conan\\profiles");
    }
    // Default: none.
    CHECK(default_settings(builtin_schema())->get_objects(keys::kBookmarks).empty());
}

TEST(bookmark_settings_errors) {
    const char* yaml =
        "search:\n"
        "  bookmarks:\n"
        "    - name: No path\n"                       // path is required
        "    - path: \"C:\\new\\tools\"\n"             // \n inside double quotes: warned
        "    - pathh: 'C:\\x'\n"                       // unknown field
        "    - 'C:\\just a string'\n";                 // not a mapping
    auto r = load_settings_text(builtin_schema(), yaml, "settings.yaml");
    for (const auto& d : r.diagnostics) std::printf("    %s\n", d.to_string().c_str());
    CHECK(!r.parse_failed);
    auto has = [&](Diagnostic::Severity sev, const char* key) {
        for (const auto& d : r.diagnostics)
            if (d.severity == sev && d.key.find(key) != std::string::npos) return true;
        return false;
    };
    CHECK(has(Diagnostic::Severity::Error, "search.bookmarks[0]"));
    CHECK(has(Diagnostic::Severity::Warning, "search.bookmarks[1]"));
    CHECK(has(Diagnostic::Severity::Error, "search.bookmarks[3]"));
    // Whatever survives validation still parses without throwing.
    (void)parse_bookmarks(r.settings->get_objects(keys::kBookmarks), kHome, fake_expand);
}

TEST(bookmark_in_generated_files) {
    const std::string yaml = generate_default_yaml(builtin_schema(), "./schema/settings.json");
    CHECK(yaml.find("bookmarks: []") != std::string::npos);
    CHECK(yaml.find(".conan") != std::string::npos);  // the example
    auto r = load_settings_text(builtin_schema(), yaml, "settings.yaml");
    CHECK(r.diagnostics.empty());
    const std::string json = generate_json_schema(builtin_schema());
    CHECK(json.find("\"bookmarks\"") != std::string::npos);
    CHECK(json.find("\"keyword\"") != std::string::npos);
}

// ---- search ----------------------------------------------------------------------------------

TEST(bookmark_keyword_beats_a_same_named_folder) {
    // The case this exists for: ~\.conan (hidden, never indexed) and a project's own "conan" folder.
    const Bookmark b{L"Conan cache", kHome + L"\\.conan", L"conan"};
    std::vector<Item> items{
        folder_item(L"conan", L"D:\\src\\app\\conan"),
        folder_item(b.name, b.path, bookmark_keywords(b)),
        folder_item(L"conan-center", L"D:\\src\\conan-center"),
    };
    UsageStore usage;
    SearchOptions opt;
    opt.now_unix = 1'800'000'000;

    // Without the keyword the exact folder name wins.
    auto hits = search(items, L"conan", usage, opt);
    CHECK_EQ(hits.size(), 3u);
    CHECK(hits[0].item->title == L"conan");

    opt.aliases.push_back({b.keyword_folded, fold(b.name)});
    hits = search(items, L"conan", usage, opt);
    CHECK(!hits.empty() && hits[0].item->title == L"Conan cache");
    CHECK(hits.size() == 3u);  // the others are still listed below it
    hits = search(items, L"CONAN", usage, opt);  // keywords are case insensitive
    CHECK(!hits.empty() && hits[0].item->title == L"Conan cache");

    // The hidden folder's own name finds it too.
    hits = search(items, L".conan", usage, opt);
    CHECK(!hits.empty() && hits[0].item->title == L"Conan cache");
    // And the name, fuzzily.
    hits = search(items, L"cc", usage, opt);
    CHECK(!hits.empty() && hits[0].item->title == L"Conan cache");
    // Highlight positions refer to the title.
    hits = search(items, L"cache", usage, opt);
    CHECK(!hits.empty() && hits[0].item->title == L"Conan cache" && !hits[0].positions.empty() && hits[0].positions[0] == 6);
}

TEST(bookmark_listed_once) {
    Item bookmarked = folder_item(L"Flash device", L"D:\\tools\\flash.py", L"flash.py");
    Item from_index = folder_item(L"flash.py", L"D:\\tools\\flash.py");
    Item other = folder_item(L"flash_old.py", L"D:\\tools\\flash_old.py");
    std::vector<Hit> hits{Hit{&bookmarked, 50, {}}};
    CHECK(!add_unique_hit(hits, Hit{&from_index, 200, {}}));  // same key: merged
    CHECK_EQ(hits.size(), 1u);
    CHECK(hits[0].item == &bookmarked);  // keeps the bookmark's name
    CHECK_EQ(hits[0].score, 200);        // and the better score
    CHECK(!add_unique_hit(hits, Hit{&from_index, 10, {}}));
    CHECK_EQ(hits[0].score, 200);
    CHECK(add_unique_hit(hits, Hit{&other, 70, {}}));
    CHECK_EQ(hits.size(), 2u);
}

TEST(bookmark_hidden_folder_is_not_indexed) {
    // Why bookmarks are needed: the file index never enters or lists names starting with '.'.
    const fs::path root = fs::temp_directory_path() / "kamil_test_bookmarks";
    fs::remove_all(root);
    fs::create_directories(root / ".conan" / "data");
    fs::create_directories(root / "project" / "conan");
    std::ofstream(root / ".conan" / "profiles.txt") << "x";
    IndexRoot r;
    r.path = root;
    r.max_depth = 6;
    FileIndex idx = FileIndex::build({r}, IndexOptions{});
    FuzzyMatcher m(L"conan");
    const auto hits = idx.search(m, 10);
    CHECK_EQ(hits.size(), 1u);
    if (!hits.empty()) CHECK(idx.path(hits[0].entry).find(L"project") != std::wstring::npos);
    FuzzyMatcher m2(L"profiles");
    CHECK(idx.search(m2, 10).empty());
    fs::remove_all(root);
}
