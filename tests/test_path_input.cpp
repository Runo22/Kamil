#include <filesystem>
#include <fstream>

#include "core/path_input.h"
#include "core/text.h"
#include "test.h"

using namespace kamil;
namespace fs = std::filesystem;

namespace {

const std::wstring kHome = L"C:\\Users\\tuna";

std::wstring fake_expand(const std::wstring& s) {
    std::wstring out = s;
    auto replace = [&](const std::wstring& var, const std::wstring& value) {
        for (size_t pos; (pos = out.find(var)) != std::wstring::npos;) out.replace(pos, var.size(), value);
    };
    replace(L"%USERPROFILE%", kHome);
    replace(L"%APPDATA%", kHome + L"\\AppData\\Roaming");
    return out;
}

std::optional<PathQuery> pq(const wchar_t* q) { return parse_path_query(q, kHome, fake_expand); }
std::optional<FileLocation> fl(const char* q) { return parse_file_location(widen(q)); }

bool loc_is(const std::optional<FileLocation>& l, const char* file, int line, int column = 0) {
    if (!l) {
        std::printf("    (no location for %s)\n", file);
        return false;
    }
    const bool ok = l->file == widen(file) && l->line == line && l->column == column;
    if (!ok) std::printf("    got '%s' %d:%d\n", narrow(l->file).c_str(), l->line, l->column);
    return ok;
}

DirListing listing(std::initializer_list<std::pair<const wchar_t*, bool>> entries) {
    DirListing l;
    l.ok = true;
    for (const auto& [name, dir] : entries) {
        DirEntry e;
        e.name = name;
        e.folded = fold(e.name);
        e.mask = char_mask(e.folded);
        e.is_dir = dir;
        l.entries.push_back(std::move(e));
    }
    return l;
}

std::vector<std::wstring> names(const DirListing& l, const std::vector<DirMatch>& ms) {
    std::vector<std::wstring> out;
    for (const auto& m : ms) out.push_back(l.entries[m.index].name);
    return out;
}

}  // namespace

// ---- basics ------------------------------------------------------------------------------------

TEST(path_input_expand_and_kinds) {
    CHECK_EQ(expand_user_path(L"~\\.conan\\", kHome, fake_expand), kHome + L"\\.conan\\");  // trailing separator kept
    CHECK_EQ(expand_user_path(L"\"C:/x y/\"", kHome, fake_expand), L"C:\\x y\\");
    CHECK_EQ(expand_user_path(L"%APPDATA%\\Code", kHome, fake_expand), kHome + L"\\AppData\\Roaming\\Code");
    CHECK(is_absolute_path(L"C:\\"));
    CHECK(is_absolute_path(L"d:/x"));
    CHECK(is_absolute_path(L"\\\\server\\share"));
    CHECK(!is_absolute_path(L"C:"));
    CHECK(!is_absolute_path(L"C:x"));
    CHECK(!is_absolute_path(L"\\\\"));
    CHECK(!is_absolute_path(L"\\x"));
    CHECK(!is_absolute_path(L"src\\a.cpp"));
    CHECK(!is_absolute_path(L"1:\\x"));

    CHECK(looks_like_path_start(L"C:"));
    CHECK(looks_like_path_start(L"c:"));
    CHECK(looks_like_path_start(L"~"));
    CHECK(looks_like_path_start(L"%appdata%"));
    CHECK(looks_like_path_start(L"\\"));
    CHECK(looks_like_path_start(L"\"C:"));
    CHECK(!looks_like_path_start(L""));
    CHECK(!looks_like_path_start(L"conan"));
    CHECK(!looks_like_path_start(L"Conan cache"));
    CHECK(!looks_like_path_start(L"code"));
}

// ---- browsing ----------------------------------------------------------------------------------

TEST(path_query_parse) {
    auto q = pq(L"C:\\Users\\tuna\\.co");
    CHECK(q && q->dir == L"C:\\Users\\tuna\\" && q->filter == L".co");
    q = pq(L"C:\\Users\\tuna\\");
    CHECK(q && q->dir == L"C:\\Users\\tuna\\" && q->filter.empty());
    q = pq(L"C:\\");
    CHECK(q && q->dir == L"C:\\" && q->filter.empty());
    q = pq(L"c:/users/TU");
    CHECK(q && q->dir == L"c:\\users\\" && q->filter == L"TU");
    q = pq(L"~\\.conan\\data\\");
    CHECK(q && q->dir == kHome + L"\\.conan\\data\\" && q->filter.empty());
    q = pq(L"%USERPROFILE%\\Doc");
    CHECK(q && q->dir == kHome + L"\\" && q->filter == L"Doc");
    q = pq(L"\"C:\\Program Files\\Mic");  // pasted with an opening quote
    CHECK(q && q->dir == L"C:\\Program Files\\" && q->filter == L"Mic");
    q = pq(L"\\\\server\\share\\");
    CHECK(q && q->dir == L"\\\\server\\share\\" && q->filter.empty());
    q = pq(L"\\\\server\\sh");
    CHECK(q && q->dir == L"\\\\server\\" && q->filter == L"sh");
    // Not paths: a normal search.
    CHECK(!pq(L"conan"));
    CHECK(!pq(L"C:"));
    CHECK(!pq(L"~"));
    CHECK(!pq(L"%USERPROFILE%"));
    CHECK(pq(L"~\\"));
    CHECK(!pq(L"\\\\server"));
    CHECK(!pq(L"src\\core"));
    CHECK(!pq(L"%NOPE%\\x"));
    CHECK(!pq(L""));
}

TEST(dir_entries_rank) {
    const DirListing l = listing({{L"zeta.txt", false}, {L".conan", true}, {L"Documents", true}, {L"conanfile.py", false},
                                  {L"conan", true}, {L"build.bat", false}, {L"AppData", true}});
    // Nothing typed: folders first, then files, each alphabetically (case insensitive).
    auto all = names(l, rank_dir_entries(l, L"", 100));
    CHECK(all.size() == 7u);
    CHECK((all == std::vector<std::wstring>{L".conan", L"AppData", L"conan", L"Documents", L"build.bat", L"conanfile.py", L"zeta.txt"}));
    CHECK_EQ(rank_dir_entries(l, L"", 3).size(), 3u);  // limit

    auto c = names(l, rank_dir_entries(l, L"conan", 100));
    CHECK(c.size() == 3u);
    CHECK(!c.empty() && c[0] == L"conan");  // exact first
    CHECK(std::find(c.begin(), c.end(), L".conan") != c.end());
    CHECK(std::find(c.begin(), c.end(), L"conanfile.py") != c.end());
    auto dot = names(l, rank_dir_entries(l, L".co", 100));
    CHECK(!dot.empty() && dot[0] == L".conan");
    CHECK(rank_dir_entries(l, L"xyz", 100).empty());
    auto turkish = listing({{L"Çalışma", true}, {L"cache", true}});
    auto t = names(turkish, rank_dir_entries(turkish, L"calis", 10));
    CHECK(t.size() == 1u && t[0] == L"Çalışma");
}

TEST(dir_listing_real_folder) {
    const fs::path root = fs::temp_directory_path() / "kamil_test_browse";
    fs::remove_all(root);
    fs::create_directories(root / ".conan" / "data");
    fs::create_directories(root / "project");
    std::ofstream(root / "notes.txt") << "x";
    std::ofstream(root / ".hidden_file") << "x";
    std::wstring dir = root.wstring();
    dir.push_back(static_cast<wchar_t>(fs::path::preferred_separator));
    DirListing l = list_directory(dir);
    CHECK(l.ok);
    CHECK(!l.truncated);
    CHECK_EQ(l.entries.size(), 4u);  // hidden entries included
    auto n = names(l, rank_dir_entries(l, L"", 10));
    CHECK((n == std::vector<std::wstring>{L".conan", L"project", L".hidden_file", L"notes.txt"}));
    DirListing small = list_directory(dir, 2);
    CHECK(small.ok && small.truncated && small.entries.size() == 2u);
    DirListing missing = list_directory((root / "nope").wstring());
    CHECK(!missing.ok && missing.entries.empty());
    fs::remove_all(root);
}

// ---- locations ---------------------------------------------------------------------------------

TEST(file_location_short_forms) {
    CHECK(loc_is(fl("main.cpp:120"), "main.cpp", 120));
    CHECK(loc_is(fl("main.cpp:120:7"), "main.cpp", 120, 7));
    CHECK(loc_is(fl("main.cpp(120)"), "main.cpp", 120));
    CHECK(loc_is(fl("main.cpp(120,7)"), "main.cpp", 120, 7));
    CHECK(loc_is(fl("  src/core/search.cpp:42  "), "src/core/search.cpp", 42));
    CHECK(loc_is(fl("C:\\src\\app\\main.cpp:12"), "C:\\src\\app\\main.cpp", 12));
    CHECK(loc_is(fl("C:\\src\\app\\main.cpp(12)"), "C:\\src\\app\\main.cpp", 12));
    CHECK(loc_is(fl("C:\\Program Files (x86)\\Windows Kits\\um\\winnt.h(1234)"), "C:\\Program Files (x86)\\Windows Kits\\um\\winnt.h", 1234));
    CHECK(loc_is(fl("C:\\Program Files (x86)\\x.h:5"), "C:\\Program Files (x86)\\x.h", 5));
    CHECK(loc_is(fl("CMakeLists.txt:3"), "CMakeLists.txt", 3));
    CHECK(loc_is(fl("tools\\Makefile:3"), "tools\\Makefile", 3));  // no extension, but a folder
    CHECK(loc_is(fl("\"C:\\a b\\c.py:9\""), "C:\\a b\\c.py", 9));
    CHECK(loc_is(fl("~\\.conan\\profiles\\default.txt:2"), "~\\.conan\\profiles\\default.txt", 2));
    // grep -n style: "file:line:text"
    CHECK(loc_is(fl("src/a.cpp:17:    int x = 0;"), "src/a.cpp", 17));
}

TEST(file_location_messages) {
    CHECK(loc_is(fl("C:\\src\\app\\main.cpp(12): error C2065: 'x': undeclared identifier"), "C:\\src\\app\\main.cpp", 12));
    CHECK(loc_is(fl("C:\\src\\app\\main.cpp(12,5): error C2065: 'x': undeclared identifier"), "C:\\src\\app\\main.cpp", 12, 5));
    CHECK(loc_is(fl("D:\\a\\_work\\1\\s\\src\\core\\search.cpp(42): warning C4100: 'opt': unreferenced"),
                 "D:\\a\\_work\\1\\s\\src\\core\\search.cpp", 42));
    CHECK(loc_is(fl("/home/ci/src/core/search.cpp:42:9: error: expected ';' before '}' token"), "/home/ci/src/core/search.cpp", 42, 9));
    CHECK(loc_is(fl("C:\\src\\a.cpp:42:9: warning: unused variable 'x' [-Wunused-variable]"), "C:\\src\\a.cpp", 42, 9));
    CHECK(loc_is(fl("  File \"C:\\tools\\flash.py\", line 33, in main"), "C:\\tools\\flash.py", 33));
}

TEST(file_location_not_locations) {
    CHECK(!fl("conan"));
    CHECK(!fl("Kamil: 2"));
    CHECK(!fl("10:30"));
    CHECK(!fl("C:"));
    CHECK(!fl("C:\\src\\"));
    CHECK(!fl("main.cpp"));
    CHECK(!fl("main.cpp:"));
    CHECK(!fl("main.cpp:0"));
    CHECK(!fl("main.cpp:abc"));
    CHECK(!fl("main.cpp:12345678"));  // more than 7 digits
    CHECK(!fl("http://server.lan:8080"));
    CHECK(!fl("Visual Studio (2"));
    CHECK(!fl("Program Files (x86)"));
    CHECK(!fl("ma"));
    CHECK(!fl("a.cpp:12\nb.cpp:3"));  // several lines: not typed into the search
}

TEST(file_location_match) {
    CHECK_EQ(location_match(L"D:\\src\\app\\core\\a.cpp", L"core/a.cpp"), 2);
    CHECK_EQ(location_match(L"D:\\src\\app\\core\\a.cpp", L"a.cpp"), 1);
    CHECK_EQ(location_match(L"D:\\src\\app\\core\\A.CPP", L"a.cpp"), 1);
    CHECK_EQ(location_match(L"D:\\src\\app\\core\\a.cpp", L"D:\\a\\_work\\1\\s\\app\\core\\a.cpp"), 3);
    CHECK_EQ(location_match(L"D:\\src\\app\\core\\a.cpp", L"b.cpp"), 0);
    CHECK_EQ(location_match(L"D:\\src\\app\\core\\a.cpp", L"other\\a.cpp"), 1);
    CHECK_EQ(location_match(L"D:\\src\\a.cpp", L".\\a.cpp"), 1);
    CHECK_EQ(location_match(L"", L"a.cpp"), 0);
}

// ---- clipboard ---------------------------------------------------------------------------------

TEST(clipboard_paths) {
    auto c = [](const wchar_t* text) { return parse_clipboard_path(text, kHome, fake_expand); };
    auto is = [](const std::optional<ClipboardPath>& p, const std::wstring& path, int line = 0, int col = 0) {
        if (!p) return false;
        const bool ok = p->path == path && p->line == line && p->column == col;
        if (!ok) std::printf("    got '%s' %d:%d\n", narrow(p->path).c_str(), p->line, p->column);
        return ok;
    };
    CHECK(is(c(L"C:\\src\\app"), L"C:\\src\\app"));
    CHECK(is(c(L"C:\\src\\app\\"), L"C:\\src\\app"));
    CHECK(is(c(L"  C:\\src\\app  \r\n"), L"C:\\src\\app"));
    CHECK(is(c(L"\"C:\\Program Files\\Git\\bin\\bash.exe\""), L"C:\\Program Files\\Git\\bin\\bash.exe"));  // Copy as path
    CHECK(is(c(L"C:/src/app/main.cpp"), L"C:\\src\\app\\main.cpp"));
    CHECK(is(c(L"C:\\"), L"C:\\"));
    CHECK(is(c(L"%USERPROFILE%\\.conan"), kHome + L"\\.conan"));
    CHECK(is(c(L"~\\.conan\\profiles"), kHome + L"\\.conan\\profiles"));
    CHECK(is(c(L"\\\\server\\share\\docs"), L"\\\\server\\share\\docs"));
    CHECK(is(c(L"file:///C:/My%20Docs/a.txt"), L"C:\\My Docs\\a.txt"));
    CHECK(is(c(L"file:///C:/%C3%87al%C4%B1%C5%9Fma"), L"C:\\" + widen("Çalışma")));
    CHECK(is(c(L"file://server/share/x"), L"\\\\server\\share\\x"));
    CHECK(is(c(L"C:\\src\\app\\main.cpp(12): error C2065: 'x': undeclared identifier"), L"C:\\src\\app\\main.cpp", 12));
    CHECK(is(c(L"C:\\src\\a.cpp:42:9: warning: unused"), L"C:\\src\\a.cpp", 42, 9));
    // A traceback: the first line with a path wins.
    CHECK(is(c(L"Traceback (most recent call last):\r\n  File \"C:\\tools\\flash.py\", line 33, in <module>\r\n    main()\r\n"),
             L"C:\\tools\\flash.py", 33));
    // Not paths.
    CHECK(!c(L""));
    CHECK(!c(L"hello world"));
    CHECK(!c(L"conan"));
    CHECK(!c(L"src\\a.cpp:12"));       // relative: nothing to open without a project
    CHECK(!c(L"https://example.com/a/b"));
    CHECK(!c(L"C:\\a<b>"));
    CHECK(!c(L"C:\\x:y"));
    CHECK(!c(L"hunter2"));
    CHECK(!c(std::wstring(10000, L'a').c_str()));
    std::wstring many;
    for (int i = 0; i < 30; ++i) many += L"line\n";
    many += L"C:\\late\\path";
    CHECK(!c(many.c_str()));  // only the first 20 lines are looked at
}

TEST(step_into_folder) {
    auto step = [](const wchar_t* folder, const wchar_t* query) { return step_into_query(folder, query, kHome, fake_expand); };
    // From the normal search: the folder's path, ready to list.
    CHECK(step(L"C:\\Users\\tuna\\.conan", L"conan") == std::optional<std::wstring>(L"C:\\Users\\tuna\\.conan\\"));
    CHECK(step(L"C:\\Users\\tuna\\.conan", L"") == std::optional<std::wstring>(L"C:\\Users\\tuna\\.conan\\"));
    CHECK(step(L"D:/src/app/", L"app") == std::optional<std::wstring>(L"D:\\src\\app\\"));
    CHECK(step(L"C:\\", L"loc") == std::optional<std::wstring>(L"C:\\"));
    CHECK(step(L"\\\\server\\share", L"share") == std::optional<std::wstring>(L"\\\\server\\share\\"));
    // While browsing: into the selected sub folder.
    CHECK(step(L"C:\\Users\\tuna\\.conan", L"C:\\Users\\tuna\\.co") == std::optional<std::wstring>(L"C:\\Users\\tuna\\.conan\\"));
    CHECK(step(kHome.c_str(), L"~\\") == std::optional<std::wstring>(L"~\\"));  // "this folder": swallowed, nothing changes
    CHECK(step(L"c:\\users\\TUNA", L"C:\\Users\\tuna\\") == std::optional<std::wstring>(L"C:\\Users\\tuna\\"));
    // A path being typed: the '\' is typed.
    CHECK(!step(L"C:\\Some\\Folder", L"C:"));
    CHECK(!step(L"C:\\Users\\tuna", L"~"));
    CHECK(!step(L"C:\\Users\\tuna\\AppData\\Roaming", L"%appdata%"));
    CHECK(!step(L"", L"x"));
}
