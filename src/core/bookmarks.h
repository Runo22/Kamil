#pragma once

// search.bookmarks: folders, files and scripts the user names in settings.yaml. A bookmark can point
// anywhere (hidden folders such as %USERPROFILE%\.conan, which the file index never enters), shows
// up under its own name and keeps every action of the folder / file / script it points at.

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/settings.h"

namespace kamil {

struct Bookmark {
    std::wstring name;            // the title shown in the search (the path's last part when not given)
    std::wstring path;            // expanded, '/' -> '\\', no trailing separator (except "C:\\")
    std::wstring keyword_folded;  // typed exactly, puts the bookmark first; empty: none
};

// Expands %ENVIRONMENT_VARIABLES% in a path (ExpandEnvironmentStrings on Windows).
using EnvExpander = std::function<std::wstring(const std::wstring&)>;

// Cleans up a bookmark path: trims spaces and surrounding quotes, expands a leading '~' to `home`,
// then %VARS% with `expand`, turns '/' into '\\' and drops trailing separators ("C:\\" stays).
std::wstring normalize_bookmark_path(std::wstring_view raw, const std::wstring& home, const EnvExpander& expand);

// The last part of a path: "C:\\Users\\me\\.conan" -> ".conan", "C:\\" -> "C:".
std::wstring last_path_part(std::wstring_view path);

// True for "\\\\server\\share\\..." paths: they are not touched on the UI thread (an unreachable
// server blocks for seconds).
bool is_unc_path(std::wstring_view path);

// Reads the search.bookmarks objects. Entries without a usable path, and a second bookmark of the
// same path (case insensitive), are skipped.
std::vector<Bookmark> parse_bookmarks(const Value::Objects& objects, const std::wstring& home, const EnvExpander& expand);

// Extra search words of a bookmark: its file / folder name, so ".conan" and "flash.py" still find
// bookmarks named "Conan cache" and "Flash device".
std::wstring bookmark_keywords(const Bookmark& b);

}  // namespace kamil
