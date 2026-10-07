#pragma once

// CMake knowledge Kamil needs without running CMake itself:
//  - CMakePresets.json / CMakeUserPresets.json: visible configure presets with resolved
//    inheritance, conditions and macro-expanded binaryDir (the same rules Visual Studio uses)
//  - CMake File API replies: executable targets and their output paths per build directory
//  - build log summaries (MSVC / Ninja errors and warnings)

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kamil {

struct ConfigurePreset {
    std::string name;
    std::string display_name;
    std::string generator;
    std::string build_type;           // CMAKE_BUILD_TYPE if given
    std::filesystem::path binary_dir; // absolute
};

struct PresetsResult {
    std::vector<ConfigurePreset> presets;  // visible (non-hidden, condition true) presets, file order
    std::vector<std::string> errors;
    bool found = false;                    // a presets file exists
};

using EnvLookup = std::function<std::optional<std::string>(std::string_view name)>;

// `host_system` is what ${hostSystemName} expands to ("Windows" on Kamil's target platform).
PresetsResult load_cmake_presets(const std::filesystem::path& source_dir, const EnvLookup& env = {},
                                 std::string_view host_system = "Windows");

// The preset Visual Studio most likely has selected: the one whose build directory was
// configured most recently (Open Folder configures the active preset automatically).
std::optional<size_t> guess_active_preset(const std::vector<ConfigurePreset>& presets);

struct CMakeTarget {
    std::string name;
    std::string type;  // EXECUTABLE, STATIC_LIBRARY, ...
    std::vector<std::filesystem::path> artifacts;  // absolute
};

struct CodeModel {
    bool valid = false;
    std::string configuration;  // e.g. "Debug"
    std::vector<CMakeTarget> targets;
    std::string error;
};

// Reads the newest File API reply in `build_dir`.
CodeModel read_codemodel(const std::filesystem::path& build_dir);
// Asks CMake to produce a codemodel on its next configure (Visual Studio already does this).
bool write_codemodel_query(const std::filesystem::path& build_dir);

struct BuildIssue {
    std::string file;
    int line = 0;
    std::string code;     // C2065, LNK2019 ...
    std::string message;
    bool error = true;
};

struct BuildSummary {
    int errors = 0;
    int warnings = 0;
    bool ninja_failed = false;          // "FAILED:" lines
    bool reported_success = false;      // "Build All succeeded" / "Derleme ... başarılı"
    bool reported_failure = false;
    std::vector<BuildIssue> issues;     // first 50 errors, then warnings

    bool failed() const { return errors > 0 || ninja_failed || reported_failure; }
};

BuildSummary summarize_build_log(std::string_view log);

}  // namespace kamil
