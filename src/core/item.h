#pragma once

#include <cstdint>
#include <string>

#include "core/text.h"

namespace kamil {

enum class ItemKind : uint8_t {
    App,      // Start menu / UWP application (launched through shell:AppsFolder)
    File,
    Folder,
    Command,  // built-in Kamil command
    Repo,     // git work tree
    Action,   // entry of the action panel (Ctrl+K)
};

struct Item {
    std::wstring key;         // stable identity used by the usage store, e.g. L"app:Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"
    std::wstring title;
    std::wstring subtitle;
    std::wstring target;      // what gets launched: AppsFolder parsing name, path, URL or command id
    std::wstring path;        // file system path if known (for "open location", "run as admin")
    std::wstring icon_source; // parsing name whose shell icon is shown; empty: target/path decide
    std::wstring completion;  // text put into the search box by Tab; empty: title
    std::wstring keywords;    // extra searchable text (e.g. a command's title in the other UI language)
    std::wstring icon_key;    // icon cache key when icons are shared (e.g. one per file extension); empty: key
    wchar_t glyph = 0;        // Segoe MDL2 Assets glyph drawn when there is no icon source
    ItemKind kind = ItemKind::App;

    // Derived search data, filled by prepare().
    std::wstring title_folded;
    std::wstring path_folded;  // for folder priority
    std::wstring keywords_folded;
    uint64_t mask = 0;
    uint64_t keywords_mask = 0;

    const std::wstring& icon_cache_key() const { return icon_key.empty() ? key : icon_key; }

    void prepare() {
        title_folded = fold(title);
        path_folded = fold(path);
        mask = char_mask(title_folded);
        keywords_folded = fold(keywords);
        keywords_mask = keywords_folded.empty() ? 0 : char_mask(keywords_folded);
    }
};

}  // namespace kamil
