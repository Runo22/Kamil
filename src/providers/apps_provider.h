#pragma once

#include <filesystem>
#include <thread>
#include <vector>

#include "core/item.h"
#include "platform/win.h"

namespace kamil {

// Enumerates everything in shell:AppsFolder (Start menu shortcuts and Store/UWP apps) on a
// background thread. Results are cached on disk so the list is available instantly on start.
class AppsProvider {
public:
    ~AppsProvider();

    static std::vector<Item> load_cache(const std::filesystem::path& file);
    static void save_cache(const std::filesystem::path& file, const std::vector<Item>& items);

    // Posts WM_KAMIL_APPS_READY with a heap-allocated std::vector<Item> to `target`.
    void scan_async(HWND target);

private:
    std::thread worker_;
};

std::vector<Item> enumerate_apps();

}  // namespace kamil
