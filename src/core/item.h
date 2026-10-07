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
    wchar_t glyph = 0;        // Segoe MDL2 Assets glyph drawn when there is no icon source
    ItemKind kind = ItemKind::App;

    // Derived search data, filled by prepare().
    std::wstring title_folded;
    uint64_t mask = 0;

    void prepare() {
        title_folded = fold(title);
        mask = char_mask(title_folded);
    }
};

}  // namespace kamil
