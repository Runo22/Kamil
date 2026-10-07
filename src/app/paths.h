#pragma once

#include <filesystem>

namespace kamil {

struct Paths {
    std::filesystem::path exe;           // Kamil.exe
    std::filesystem::path config_dir;    // settings.yaml, usage.tsv (roaming, or .\data in portable mode)
    std::filesystem::path cache_dir;     // rebuildable data (local app data, or .\data\cache)
    bool portable = false;

    std::filesystem::path settings_file() const { return config_dir / "settings.yaml"; }
    std::filesystem::path schema_file() const { return config_dir / "schema" / "settings.json"; }
    std::filesystem::path usage_file() const { return config_dir / "usage.tsv"; }
    std::filesystem::path apps_cache_file() const { return cache_dir / "apps.tsv"; }
    std::filesystem::path projects_file() const { return config_dir / "projects.tsv"; }
    std::filesystem::path vs_report_file() const { return cache_dir / "vs-baglanti-raporu.txt"; }
};

// Portable mode: a "data" folder or a "portable.txt" file next to Kamil.exe.
Paths resolve_paths();

}  // namespace kamil
