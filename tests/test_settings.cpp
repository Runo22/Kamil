#include <filesystem>
#include <fstream>

#include "core/settings.h"
#include "core/settings_schema.h"
#include "test.h"

using namespace kamil;

static bool has_diag(const LoadResult& r, Diagnostic::Severity sev, std::string_view key_part, int line = -1) {
    for (const auto& d : r.diagnostics) {
        if (d.severity != sev) continue;
        if (d.key.find(key_part) == std::string::npos) continue;
        if (line >= 0 && d.line != line) continue;
        return true;
    }
    return false;
}

TEST(settings_defaults) {
    auto s = default_settings(builtin_schema());
    CHECK_EQ(s->get_string(keys::kHotkey), std::string("Alt+Space"));
    CHECK_EQ(s->get_int(keys::kMaxRows), 8);
    CHECK_EQ(s->get_string(keys::kDefaultVs), std::string("vs2026"));
    CHECK(s->source(keys::kHotkey) == Source::Default);
}

TEST(settings_load_valid) {
    const char* yaml =
        "general:\n"
        "  hotkey: ctrl+space\n"
        "  start_with_windows: yes\n"
        "appearance:\n"
        "  theme: Dark\n"
        "  max_rows: 10\n"
        "  accent: '#3A7BD5'\n"
        "search:\n"
        "  exclude_apps: ['*helper*', '*setup*']\n"
        "  learning_half_life: 7d\n"
        "  aliases:\n"
        "    - alias: not defteri\n"
        "      target: Notepad\n"
        "    - { alias: hesap, target: Calculator }\n";
    auto r = load_settings_text(builtin_schema(), yaml, "settings.yaml");
    for (const auto& d : r.diagnostics) std::printf("    %s\n", d.to_string().c_str());
    CHECK(r.diagnostics.empty());
    const auto& s = *r.settings;
    CHECK_EQ(s.get_string(keys::kHotkey), std::string("Ctrl+Space"));
    CHECK(s.get_bool(keys::kStartWithWindows));
    CHECK_EQ(s.get_string(keys::kTheme), std::string("dark"));
    CHECK_EQ(s.get_int(keys::kMaxRows), 10);
    CHECK_EQ(s.get_string(keys::kAccent), std::string("#3a7bd5"));
    CHECK_EQ(s.get_list(keys::kExcludeApps).size(), 2u);
    CHECK_EQ(s.get_int(keys::kLearningHalfLife), int64_t{7} * 86'400'000);
    const auto& aliases = s.get_objects(keys::kAliases);
    CHECK_EQ(aliases.size(), 2u);
    CHECK_EQ(aliases[1].find("target")->as_string(), std::string("Calculator"));
    CHECK(s.source(keys::kMaxRows) == Source::User);
    CHECK_EQ(s.line(keys::kMaxRows), 6);
    CHECK(s.source(keys::kWidth) == Source::Default);
}

TEST(settings_invalid_values_fall_back) {
    const char* yaml =
        "general:\n"
        "  hotkey: Hyper+Q\n"
        "appearance:\n"
        "  max_rows: 99\n"
        "  theme: neon\n"
        "  widht: 800\n"
        "search:\n"
        "  aliases:\n"
        "    - alias: x\n";
    auto r = load_settings_text(builtin_schema(), yaml, "settings.yaml");
    for (const auto& d : r.diagnostics) std::printf("    %s\n", d.to_string().c_str());
    CHECK(!r.parse_failed);
    CHECK(has_diag(r, Diagnostic::Severity::Error, "general.hotkey", 2));
    CHECK(has_diag(r, Diagnostic::Severity::Error, "appearance.max_rows", 4));
    CHECK(has_diag(r, Diagnostic::Severity::Error, "appearance.theme", 5));
    CHECK(has_diag(r, Diagnostic::Severity::Warning, "appearance.widht", 6));
    CHECK(has_diag(r, Diagnostic::Severity::Error, "search.aliases[0]"));
    bool suggested = false;
    for (const auto& d : r.diagnostics) suggested |= d.message.find("appearance.width") != std::string::npos;
    CHECK(suggested);
    const auto& s = *r.settings;
    CHECK_EQ(s.get_string(keys::kHotkey), std::string("Alt+Space"));
    CHECK_EQ(s.get_int(keys::kMaxRows), 8);
    CHECK(s.get_objects(keys::kAliases).empty());
}

TEST(settings_syntax_error) {
    auto r = load_settings_text(builtin_schema(), "general:\n  hotkey: [unclosed\n", "settings.yaml");
    CHECK(r.parse_failed);
    CHECK(!r.diagnostics.empty());
    CHECK(r.diagnostics[0].line > 0);
    CHECK_EQ(r.settings->get_string(keys::kHotkey), std::string("Alt+Space"));
}

TEST(settings_empty_and_comments_only) {
    CHECK(load_settings_text(builtin_schema(), "", "a").diagnostics.empty());
    auto r = load_settings_text(builtin_schema(), "# just a comment\n\n", "a");
    CHECK(r.diagnostics.empty());
    CHECK(!r.parse_failed);
    r = load_settings_text(builtin_schema(), "general:\n", "a");
    CHECK(r.diagnostics.empty());
}

TEST(settings_default_yaml_roundtrip) {
    const std::string yaml = generate_default_yaml(builtin_schema(), "./schema/settings.json");
    auto r = load_settings_text(builtin_schema(), yaml, "settings.yaml");
    for (const auto& d : r.diagnostics) std::printf("    %s\n", d.to_string().c_str());
    CHECK(r.diagnostics.empty());
    auto def = default_settings(builtin_schema());
    CHECK(r.settings->diff(*def).empty());
    CHECK(yaml.find("yaml-language-server") != std::string::npos);
}

TEST(settings_json_schema) {
    const std::string js = generate_json_schema(builtin_schema());
    CHECK(js.find("\"hotkey\"") != std::string::npos);
    CHECK(js.find("\"enum\": [\"auto\", \"dark\", \"light\"]") != std::string::npos);
    // balanced braces
    int depth = 0;
    bool in_str = false, esc = false;
    for (char c : js) {
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') --depth;
        CHECK(depth >= 0);
    }
    CHECK_EQ(depth, 0);
}

TEST(settings_store_notifies) {
    const auto dir = std::filesystem::temp_directory_path() / "kamil_test_settings";
    std::filesystem::create_directories(dir);
    const auto file = dir / "settings.yaml";
    SettingsStore store(builtin_schema());
    int hotkey_calls = 0, search_calls = 0;
    store.subscribe(keys::kHotkey, [&](const Settings&, const std::vector<std::string>&) { ++hotkey_calls; });
    store.subscribe("search", [&](const Settings&, const std::vector<std::string>&) { ++search_calls; });

    std::ofstream(file) << "general:\n  hotkey: Ctrl+Alt+K\n";
    auto r = store.reload(file);
    CHECK(!r.has_errors());
    CHECK_EQ(hotkey_calls, 1);
    CHECK_EQ(search_calls, 0);
    CHECK_EQ(store.current()->get_string(keys::kHotkey), std::string("Ctrl+Alt+K"));

    std::ofstream(file) << "general:\n  hotkey: [broken\n";
    r = store.reload(file);
    CHECK(r.parse_failed);
    CHECK_EQ(store.current()->get_string(keys::kHotkey), std::string("Ctrl+Alt+K"));  // previous kept
    CHECK_EQ(hotkey_calls, 1);

    std::ofstream(file) << "\xEF\xBB\xBFsearch:\n  max_results: 20\n";  // BOM
    r = store.reload(file);
    CHECK(!r.has_errors());
    CHECK_EQ(search_calls, 1);
    CHECK_EQ(hotkey_calls, 2);  // hotkey went back to default
    std::filesystem::remove_all(dir);
}

TEST(duration_format) {
    int64_t ms = 0;
    CHECK(parse_duration("90s", &ms) && ms == 90'000);
    CHECK(parse_duration("250", &ms) && ms == 250);
    CHECK(parse_duration("2 h", &ms) && ms == 7'200'000);
    CHECK(!parse_duration("soon", &ms));
    CHECK_EQ(format_duration(86'400'000), std::string("1d"));
    CHECK_EQ(format_duration(1500), std::string("1500ms"));
}
