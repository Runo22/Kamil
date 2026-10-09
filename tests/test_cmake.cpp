#include <chrono>
#include <filesystem>
#include <fstream>

#include "core/cmake.h"
#include "test.h"

using namespace kamil;
namespace fs = std::filesystem;

namespace {

fs::path fresh_dir(const char* name) {
    const fs::path d = fs::temp_directory_path() / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

}  // namespace

TEST(cmake_presets_visual_studio_template) {
    const fs::path src = fresh_dir("kamil_test_presets");
    // Shape of the presets file Visual Studio generates for a new CMake project.
    write(src / "CMakePresets.json", R"({
  "version": 3,
  "configurePresets": [
    { "name": "windows-base", "hidden": true, "generator": "Ninja",
      "binaryDir": "${sourceDir}/out/build/${presetName}",
      "cacheVariables": { "CMAKE_C_COMPILER": "cl.exe" },
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" } },
    { "name": "x64-debug", "displayName": "x64 Debug", "inherits": "windows-base",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" } },
    { "name": "x64-release", "displayName": "x64 Release", "inherits": "x64-debug",
      "cacheVariables": { "CMAKE_BUILD_TYPE": { "type": "STRING", "value": "Release" } } },
    { "name": "linux-debug", "generator": "Ninja",
      "binaryDir": "$env{BUILD_ROOT}/${sourceDirName}-${presetName}",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux" } },
    { "name": "custom-dir", "inherits": ["windows-base"], "binaryDir": "build/custom" }
  ]
})");
    write(src / "CMakeUserPresets.json", R"({
  "version": 3,
  "configurePresets": [ { "name": "mine", "inherits": "x64-debug", "displayName": "Benim" } ]
})");

    auto r = load_cmake_presets(src);
    for (const auto& e : r.errors) std::printf("    %s\n", e.c_str());
    CHECK(r.found);
    CHECK(r.errors.empty());
    CHECK_EQ(r.presets.size(), 4u);  // hidden base and the Linux-only preset are not listed
    CHECK_EQ(r.presets[0].name, std::string("x64-debug"));
    CHECK_EQ(r.presets[0].generator, std::string("Ninja"));
    CHECK_EQ(r.presets[0].build_type, std::string("Debug"));
    CHECK(r.presets[0].binary_dir == (src / "out" / "build" / "x64-debug").lexically_normal().make_preferred());
    CHECK_EQ(r.presets[1].build_type, std::string("Release"));  // own value beats the parent's
    CHECK(r.presets[1].binary_dir == (src / "out" / "build" / "x64-release").lexically_normal().make_preferred());
    CHECK(r.presets[2].binary_dir == (src / "build" / "custom").lexically_normal().make_preferred());
    CHECK_EQ(r.presets[3].display_name, std::string("Benim"));

    // The Linux preset appears on Linux, with $env{} expanded.
    const fs::path build_root = fs::temp_directory_path() / "builds";  // absolute on every platform
    EnvLookup env = [&](std::string_view n) -> std::optional<std::string> {
        if (n == "BUILD_ROOT") return build_root.string();
        return std::nullopt;
    };
    auto linux_r = load_cmake_presets(src, env, "Linux");
    bool found = false;
    for (const auto& p : linux_r.presets)
        if (p.name == "linux-debug") {
            found = true;
            CHECK(p.binary_dir == (build_root / "kamil_test_presets-linux-debug").lexically_normal().make_preferred());
        }
    CHECK(found);
    fs::remove_all(src);
}

TEST(cmake_presets_errors_and_missing) {
    const fs::path src = fresh_dir("kamil_test_presets_bad");
    auto r = load_cmake_presets(src);
    CHECK(!r.found);
    CHECK(r.presets.empty());
    write(src / "CMakePresets.json", R"({ "version": 3, "configurePresets": [ { "name": "a", "inherits": "nope" }, )");
    r = load_cmake_presets(src);
    CHECK(!r.errors.empty());
    fs::remove_all(src);
}

TEST(cmake_file_api_and_active_preset) {
    const fs::path src = fresh_dir("kamil_test_fileapi");
    const fs::path debug = src / "out" / "build" / "x64-debug";
    const fs::path release = src / "out" / "build" / "x64-release";
    const fs::path reply = debug / ".cmake" / "api" / "v1" / "reply";
    std::string build_path = debug.generic_string();
    write(reply / "index-2026-10-07T10-00-00-0000.json",
          R"({ "objects": [ { "kind": "cache", "version": { "major": 2, "minor": 0 }, "jsonFile": "cache.json" },
                           { "kind": "codemodel", "version": { "major": 2, "minor": 7 }, "jsonFile": "codemodel-v2-abc.json" } ] })");
    write(reply / "codemodel-v2-abc.json", R"({ "paths": { "build": ")" + build_path + R"(", "source": "x" },
        "configurations": [ { "name": "Debug", "targets": [
            { "name": "SensorUI", "jsonFile": "target-SensorUI.json" },
            { "name": "core", "jsonFile": "target-core.json" } ] } ] })");
    write(reply / "target-SensorUI.json", R"({ "name": "SensorUI", "type": "EXECUTABLE", "artifacts": [ { "path": "app/SensorUI.exe" }, { "path": "app/SensorUI.pdb" } ] })");
    write(reply / "target-core.json", R"({ "name": "core", "type": "STATIC_LIBRARY", "artifacts": [ { "path": "core/core.lib" } ] })");

    CodeModel cm = read_codemodel(debug);
    if (!cm.error.empty()) std::printf("    %s\n", cm.error.c_str());
    CHECK(cm.valid);
    CHECK_EQ(cm.configuration, std::string("Debug"));
    CHECK_EQ(cm.targets.size(), 2u);
    CHECK_EQ(cm.targets[0].type, std::string("EXECUTABLE"));
    CHECK(cm.targets[0].artifacts[0] == (debug / "app" / "SensorUI.exe").lexically_normal().make_preferred());
    CHECK(!read_codemodel(release).valid);

    // Cached until CMake writes a new reply (a new index file).
    write(reply / "target-SensorUI.json", R"({ "name": "SensorUI", "type": "EXECUTABLE", "artifacts": [ { "path": "bin/Renamed.exe" } ] })");
    CHECK(read_codemodel(debug).targets[0].artifacts[0] == cm.targets[0].artifacts[0]);
    write(reply / "index-2026-10-07T11-00-00-0000.json",
          R"({ "objects": [ { "kind": "codemodel", "version": { "major": 2, "minor": 7 }, "jsonFile": "codemodel-v2-abc.json" } ] })");
    CHECK(read_codemodel(debug).targets[0].artifacts[0] == (debug / "bin" / "Renamed.exe").lexically_normal().make_preferred());

    CHECK(write_codemodel_query(release));
    CHECK(fs::exists(release / ".cmake" / "api" / "v1" / "query" / "client-kamil" / "codemodel-v2"));

    std::vector<ConfigurePreset> presets(2);
    presets[0].name = "x64-debug";
    presets[0].binary_dir = debug;
    presets[1].name = "x64-release";
    presets[1].binary_dir = release;
    CHECK(guess_active_preset(presets) == std::optional<size_t>(0));
    write(release / "CMakeCache.txt", "# newer configure\n");
    // Explicit times: file systems differ in timestamp resolution.
    fs::last_write_time(release / "CMakeCache.txt",
                        fs::last_write_time(reply / "index-2026-10-07T10-00-00-0000.json") + std::chrono::seconds(10));
    CHECK(guess_active_preset(presets) == std::optional<size_t>(1));
    fs::remove_all(src);
}

TEST(cmake_build_log_summary) {
    const char* log =
        ">------ Build All started: Project: SensorUI, Configuration: x64-debug ------\n"
        "  [1/3] Building CXX object app\\CMakeFiles\\SensorUI.dir\\main.cpp.obj\n"
        "  FAILED: app/CMakeFiles/SensorUI.dir/main.cpp.obj\n"
        "D:\\src\\SensorUI\\app\\main.cpp(42,13): error C2065: 'port': undeclared identifier\n"
        "D:\\src\\SensorUI\\app\\main.cpp(42,13): error C2065: 'port': undeclared identifier\n"
        "D:\\src\\SensorUI\\app\\serial.cpp(7): warning C4996: 'strcpy': This function may be unsafe.\n"
        "LINK : fatal error LNK1168: cannot open SensorUI.exe for writing\n"
        "Build All failed.\n";
    auto s = summarize_build_log(log);
    CHECK(s.failed());
    CHECK(s.ninja_failed);
    CHECK(s.reported_failure);
    CHECK_EQ(s.errors, 2);  // duplicate line counted once
    CHECK_EQ(s.warnings, 1);
    CHECK_EQ(s.issues[0].file, std::string("D:\\src\\SensorUI\\app\\main.cpp"));
    CHECK_EQ(s.issues[0].line, 42);
    CHECK_EQ(s.issues[0].code, std::string("C2065"));
    CHECK_EQ(s.issues[1].code, std::string("LNK1168"));
    CHECK(!s.issues[2].error);

    auto ok = summarize_build_log("  [3/3] Linking CXX executable app\\SensorUI.exe\r\nBuild All succeeded.\r\n");
    CHECK(!ok.failed());
    CHECK(ok.reported_success);
}

#include "core/project_state.h"

TEST(project_state_roundtrip) {
    const fs::path dir = fresh_dir("kamil_test_state");
    ProjectStateStore store;
    store.set(L"D:\\src\\SensorUI", ProjectChoice{"x64-release", "SensorUI", "--port {com}\t--x", "COM7", "VID_0403&PID_6001"});
    CHECK(store.save(dir / "projects.tsv"));
    ProjectStateStore loaded;
    CHECK(loaded.load(dir / "projects.tsv"));
    const auto c = loaded.get(L"d:\\SRC\\sensorui");  // lookups are case-insensitive
    CHECK_EQ(c.preset, std::string("x64-release"));
    CHECK_EQ(c.args, std::string("--port {com} --x"));  // tab sanitized
    CHECK_EQ(c.com_hwid, std::string("VID_0403&PID_6001"));
    CHECK(loaded.get(L"D:\\other").preset.empty());
    fs::remove_all(dir);
}

TEST(placeholder_expansion) {
    const std::map<std::string, std::string> v{{"com", "COM7"}, {"preset", "x64-debug"}};
    CHECK_EQ(expand_placeholders("--port {com} --cfg {preset} {unknown} {", v), std::string("--port COM7 --cfg x64-debug {unknown} {"));
}
