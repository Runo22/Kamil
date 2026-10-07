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
};

struct Item {
    std::wstring key;         // stable identity used by the usage store, e.g. L"app:Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"
    std::wstring title;
    std::wstring subtitle;
    std::wstring target;      // what gets launched: AppsFolder parsing name, path, URL or command id
    std::wstring path;        // file system path if known (for "open location", "run as admin")
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
