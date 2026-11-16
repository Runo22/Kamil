#pragma once

// Paths the user types or pastes into the search box, or copies to the clipboard:
//  - browsing: "C:\Users\me\.co" lists C:\Users\me\ filtered by "co" (hidden entries included);
//  - locations: "main.cpp:120", "a.cpp(12,5): error C2065 ...", 'File "x.py", line 3' open a file at a line;
//  - clipboard: a copied path or location becomes the first result.
// Platform independent: the caller expands %VARS% and touches the file system on Windows.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kamil {

// Expands %ENVIRONMENT_VARIABLES% in a path (ExpandEnvironmentStrings on Windows).
using EnvExpander = std::function<std::wstring(const std::wstring&)>;

// Trims spaces and surrounding quotes, expands a leading '~' to `home`, then %VARS% with `expand`,
// and turns '/' into '\\'. A trailing separator is kept ("C:\x\" stays a folder to list).
std::wstring expand_user_path(std::wstring_view raw, const std::wstring& home, const EnvExpander& expand);

// The last part of a path: "C:\\Users\\me\\.conan" -> ".conan", "C:\\" -> "C:".
std::wstring last_path_part(std::wstring_view path);

// "\\\\server\\share\\...": never touched on the UI thread (an unreachable server blocks for seconds).
bool is_unc_path(std::wstring_view path);

// "C:\\..." or "\\\\server\\..." (after expansion).
bool is_absolute_path(std::wstring_view path);

// Text that starts a path but is not one yet: "C:", "~", "%APPDATA%", "\\". A '\' typed after it is
// part of the path, not "step into the selected folder".
bool looks_like_path_start(std::wstring_view query);

// ---- Browsing ----------------------------------------------------------------------------------

struct PathQuery {
    std::wstring dir;     // the folder to list, expanded, ends with '\\'
    std::wstring filter;  // what is typed after the last separator
};

// "C:\Users\me\.co" -> { "C:\Users\me\", ".co" }; nullopt when the query is not an absolute path.
std::optional<PathQuery> parse_path_query(std::wstring_view query, const std::wstring& home, const EnvExpander& expand);

// '\' typed at the end of `query` while `folder` is the selected result: the search text that lists
// the folder ("D:\src\app\"), `query` itself when that folder is already listed (the key is
// swallowed), or nullopt when the '\' belongs to a path being typed ("C:", "~", "%APPDATA%").
std::optional<std::wstring> step_into_query(std::wstring_view folder, std::wstring_view query, const std::wstring& home,
                                            const EnvExpander& expand);

struct DirEntry {
    std::wstring name;
    std::wstring folded;
    uint64_t mask = 0;
    bool is_dir = false;
};

struct DirListing {
    std::wstring dir;  // as given to list_directory
    std::vector<DirEntry> entries;
    bool ok = false;         // the folder could be read
    bool truncated = false;  // more than max_entries
};

// Every entry of `dir`, hidden ones included (std::filesystem; may block on network folders).
DirListing list_directory(const std::wstring& dir, size_t max_entries = 50'000);

struct DirMatch {
    uint32_t index = 0;  // into DirListing::entries
    int score = 0;
};

// Entries matching `filter` (fuzzy), best first; folders before files on equal scores, then by
// name. An empty filter lists everything: folders first, then files, each alphabetically.
std::vector<DirMatch> rank_dir_entries(const DirListing& listing, std::wstring_view filter, size_t limit);

// ---- Locations (file:line) -----------------------------------------------------------------------

struct FileLocation {
    std::wstring file;  // as written: absolute, relative ("src\\a.cpp") or a bare name
    int line = 0;       // 1-based
    int column = 0;     // 0: not given
};

// "a.cpp:12", "a.cpp:12:5", "a.cpp(12)", "a.cpp(12,5)", whole MSVC / GCC / Clang messages and
// Python traceback lines. The file must look like one (an extension or a folder), so "Kamil: 2" or
// "10:30" are not locations.
std::optional<FileLocation> parse_file_location(std::wstring_view text);

// How well an existing `path` fits the `written` file of a location: the number of trailing path
// parts that are equal, case insensitive ("D:\src\app\core\a.cpp" vs "core/a.cpp" -> 2). 0 when the
// file names differ.
int location_match(std::wstring_view path, std::wstring_view written);

// ---- Clipboard ---------------------------------------------------------------------------------

struct ClipboardPath {
    std::wstring path;  // absolute, expanded, no trailing separator (except "C:\\")
    int line = 0;       // > 0: a location
    int column = 0;
};

// A path or location in copied text: "C:\x\y", "\"C:\x y\z.txt\"" (Explorer's Copy as path),
// "file:///C:/x/y%20z", "%USERPROFILE%\.conan", "C:\a.cpp(12): error ...", a Python traceback ...
// Only absolute paths count; the first of at most 20 lines that has one wins.
std::optional<ClipboardPath> parse_clipboard_path(std::wstring_view text, const std::wstring& home, const EnvExpander& expand);

}  // namespace kamil
