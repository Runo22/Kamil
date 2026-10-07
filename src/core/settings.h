#pragma once

// Schema-driven settings infrastructure.
//
// Every setting is declared exactly once as a SettingDef (key, type, default, limits, texts).
// From that single declaration Kamil derives: YAML parsing and validation with line-accurate
// diagnostics, typed access, the commented default settings.yaml, the JSON Schema used for
// editor auto-completion, and (later) the generated Settings window form.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace kamil {

enum class Kind {
    Bool,
    Int,
    Float,
    String,
    Enum,
    Path,
    Hotkey,
    Color,       // "auto" or #RGB / #RRGGBB / #RRGGBBAA
    Duration,    // stored as milliseconds; written as 250ms, 30s, 5m, 1h, 2d
    StringList,
    PathList,
    GlobList,
    ObjectList,  // list of maps; the element layout is described by SettingDef::fields
};

// What has to happen for a changed value to take effect. Used by subscribers and shown in the
// Settings window.
enum class Apply { Live, Hotkey, Reindex, Restart };

struct Value;

struct Object {
    std::vector<std::pair<std::string, Value>> fields;

    const Value* find(std::string_view name) const;
    bool operator==(const Object& other) const;
};

struct Value {
    using List = std::vector<std::string>;
    using Objects = std::vector<Object>;
    std::variant<std::monostate, bool, int64_t, double, std::string, List, Objects> data;

    Value() = default;
    Value(bool b) : data(b) {}
    Value(int v) : data(static_cast<int64_t>(v)) {}
    Value(int64_t v) : data(v) {}
    Value(double v) : data(v) {}
    Value(const char* s) : data(std::string(s)) {}
    Value(std::string s) : data(std::move(s)) {}
    Value(List l) : data(std::move(l)) {}
    Value(Objects o) : data(std::move(o)) {}

    bool is_null() const { return std::holds_alternative<std::monostate>(data); }
    bool as_bool() const { return std::get<bool>(data); }
    int64_t as_int() const { return std::get<int64_t>(data); }
    double as_float() const { return std::get<double>(data); }
    const std::string& as_string() const { return std::get<std::string>(data); }
    const List& as_list() const { return std::get<List>(data); }
    const Objects& as_objects() const { return std::get<Objects>(data); }

    bool operator==(const Value& other) const;
};

struct SettingDef {
    std::string key;  // dotted path, e.g. "appearance.max_rows"; for ObjectList fields: field name
    Kind kind = Kind::String;
    Value def;
    std::string title;
    std::string description;
    int64_t min = INT64_MIN;  // Int, Duration (ms)
    int64_t max = INT64_MAX;
    std::vector<std::string> choices;  // Enum
    std::vector<SettingDef> fields;    // ObjectList element layout
    Apply apply = Apply::Live;
    bool required = false;             // ObjectList fields only
};

class Schema {
public:
    // Keys sharing a prefix must be added next to each other (the YAML writer relies on it).
    SettingDef& add(SettingDef def);
    const SettingDef* find(std::string_view key) const;
    const std::vector<SettingDef>& all() const { return defs_; }
    bool has_section(std::string_view prefix) const;  // true if some key starts with prefix + "."
    void set_section_title(std::string section, std::string title);
    std::string_view section_title(std::string_view section) const;

private:
    std::vector<SettingDef> defs_;
    std::unordered_map<std::string, size_t> index_;
    std::vector<std::pair<std::string, std::string>> section_titles_;
};

struct Diagnostic {
    enum class Severity { Error, Warning };
    Severity severity = Severity::Error;
    std::string file;
    int line = 0;    // 1-based, 0 = unknown
    int column = 0;  // 1-based, 0 = unknown
    std::string key;
    std::string message;

    std::string to_string() const;
};

enum class Source { Default, User };

// Immutable snapshot of all settings. Cheap to share between threads.
class Settings {
public:
    bool get_bool(std::string_view key) const;
    int64_t get_int(std::string_view key) const;
    double get_float(std::string_view key) const;
    const std::string& get_string(std::string_view key) const;  // String, Enum, Path, Hotkey, Color
    const std::vector<std::string>& get_list(std::string_view key) const;
    const std::vector<Object>& get_objects(std::string_view key) const;
    const Value& get(std::string_view key) const;
    Source source(std::string_view key) const;
    int line(std::string_view key) const;  // line in the user file, 0 if default

    std::vector<std::string> diff(const Settings& other) const;  // keys whose values differ

private:
    friend class SettingsLoader;
    struct Entry {
        Value value;
        Source source = Source::Default;
        int line = 0;
    };
    const Entry& entry(std::string_view key) const;
    std::unordered_map<std::string, Entry> values_;
};

struct LoadResult {
    std::shared_ptr<const Settings> settings;  // always valid (defaults on failure)
    std::vector<Diagnostic> diagnostics;
    bool parse_failed = false;                 // YAML syntax error: nothing from the file was used

    bool has_errors() const;
};

std::shared_ptr<const Settings> default_settings(const Schema& schema);
LoadResult load_settings_text(const Schema& schema, std::string_view yaml, std::string_view filename = {});
LoadResult load_settings_file(const Schema& schema, const std::filesystem::path& path);

// Commented default file. `schema_ref` (may be empty) is written as a yaml-language-server
// modeline so editors pick up the JSON Schema.
std::string generate_default_yaml(const Schema& schema, std::string_view schema_ref = {});
std::string generate_json_schema(const Schema& schema);

// Formatting helpers shared with the writer and tests.
std::string format_duration(int64_t ms);
bool parse_duration(std::string_view text, int64_t* ms);

// Holds the current snapshot and notifies subscribers about changes. Readers never block:
// current() is an atomic shared_ptr load.
class SettingsStore {
public:
    using Callback = std::function<void(const Settings& now, const std::vector<std::string>& changed)>;

    explicit SettingsStore(const Schema& schema);

    const Schema& schema() const { return schema_; }
    std::shared_ptr<const Settings> current() const { return current_.load(); }

    // Loads the file; on a YAML syntax error the previous snapshot is kept. Subscribers whose
    // prefix matches at least one changed key are called on the calling thread.
    LoadResult reload(const std::filesystem::path& path);
    void replace(std::shared_ptr<const Settings> settings);

    // prefix "" matches everything, "search" matches "search.*", "general.hotkey" matches exactly.
    int subscribe(std::string prefix, Callback cb);
    void unsubscribe(int id);

private:
    void publish(std::shared_ptr<const Settings> next);

    const Schema& schema_;
    std::atomic<std::shared_ptr<const Settings>> current_;
    std::mutex mutex_;
    struct Sub {
        int id;
        std::string prefix;
        Callback cb;
    };
    std::vector<Sub> subs_;
    int next_id_ = 1;
};

}  // namespace kamil
