#include "core/settings.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>

#ifndef _RYML_SINGLE_HEADER_AMALGAMATED_HPP_
#include <ryml_all.hpp>
#endif

#include "core/hotkey.h"
#include "core/i18n.h"
#include "core/ryml_support.h"
#include "core/text.h"

namespace kamil {

// ---------------------------------------------------------------------------------------------
// Value / Object

const Value* Object::find(std::string_view name) const {
    for (const auto& [k, v] : fields)
        if (k == name) return &v;
    return nullptr;
}

bool Object::operator==(const Object& other) const { return fields == other.fields; }

bool Value::operator==(const Value& other) const { return data == other.data; }

// ---------------------------------------------------------------------------------------------
// Schema

SettingDef& Schema::add(SettingDef def) {
    index_[def.key] = defs_.size();
    defs_.push_back(std::move(def));
    return defs_.back();
}

const SettingDef* Schema::find(std::string_view key) const {
    auto it = index_.find(std::string(key));
    return it == index_.end() ? nullptr : &defs_[it->second];
}

bool Schema::has_section(std::string_view prefix) const {
    for (const auto& d : defs_)
        if (d.key.size() > prefix.size() && d.key.compare(0, prefix.size(), prefix) == 0 &&
            d.key[prefix.size()] == '.')
            return true;
    return false;
}

void Schema::set_section_title(std::string section, std::string title_en, std::string title_tr) {
    section_titles_.push_back(SectionTitle{std::move(section), std::move(title_en), std::move(title_tr)});
}

std::string_view Schema::section_title(std::string_view section) const {
    for (const auto& t : section_titles_)
        if (t.section == section) return language() == Lang::Tr && !t.tr.empty() ? t.tr : t.en;
    return {};
}

const std::string& setting_title(const SettingDef& d) { return language() == Lang::Tr && !d.title_tr.empty() ? d.title_tr : d.title; }
const std::string& setting_description(const SettingDef& d) {
    return language() == Lang::Tr && !d.description_tr.empty() ? d.description_tr : d.description;
}

// ---------------------------------------------------------------------------------------------
// Diagnostics

std::string Diagnostic::to_string() const {
    std::string out = file.empty() ? std::string(loc("settings", "ayarlar")) : file;
    if (line > 0) {
        out += ":" + std::to_string(line);
        if (column > 0) out += ":" + std::to_string(column);
    }
    out += severity == Severity::Error ? loc(": error: ", ": hata: ") : loc(": warning: ", ": uyarı: ");
    if (!key.empty()) out += "'" + key + "': ";
    out += message;
    return out;
}

bool LoadResult::has_errors() const {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& d) { return d.severity == Diagnostic::Severity::Error; });
}

// ---------------------------------------------------------------------------------------------
// Settings snapshot

const Settings::Entry& Settings::entry(std::string_view key) const {
    auto it = values_.find(std::string(key));
    if (it == values_.end()) throw std::logic_error("unknown setting: " + std::string(key));
    return it->second;
}

const Value& Settings::get(std::string_view key) const { return entry(key).value; }
bool Settings::get_bool(std::string_view key) const { return entry(key).value.as_bool(); }
int64_t Settings::get_int(std::string_view key) const { return entry(key).value.as_int(); }
double Settings::get_float(std::string_view key) const { return entry(key).value.as_float(); }
const std::string& Settings::get_string(std::string_view key) const { return entry(key).value.as_string(); }
const std::vector<std::string>& Settings::get_list(std::string_view key) const { return entry(key).value.as_list(); }
const std::vector<Object>& Settings::get_objects(std::string_view key) const { return entry(key).value.as_objects(); }
Source Settings::source(std::string_view key) const { return entry(key).source; }
int Settings::line(std::string_view key) const { return entry(key).line; }

std::vector<std::string> Settings::diff(const Settings& other) const {
    std::vector<std::string> changed;
    for (const auto& [k, e] : values_) {
        auto it = other.values_.find(k);
        if (it == other.values_.end() || !(it->second.value == e.value)) changed.push_back(k);
    }
    std::sort(changed.begin(), changed.end());
    return changed;
}

// ---------------------------------------------------------------------------------------------
// Durations

std::string format_duration(int64_t ms) {
    if (ms != 0) {
        if (ms % 86'400'000 == 0) return std::to_string(ms / 86'400'000) + "d";
        if (ms % 3'600'000 == 0) return std::to_string(ms / 3'600'000) + "h";
        if (ms % 60'000 == 0) return std::to_string(ms / 60'000) + "m";
        if (ms % 1000 == 0) return std::to_string(ms / 1000) + "s";
    }
    return std::to_string(ms) + "ms";
}

bool parse_duration(std::string_view text, int64_t* ms) {
    text = trim(text);
    size_t i = 0;
    int64_t n = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        n = n * 10 + (text[i] - '0');
        if (n > 1'000'000'000'000) return false;
        ++i;
    }
    if (i == 0) return false;
    const std::string unit = to_lower_ascii(trim(text.substr(i)));
    int64_t mul = 0;
    if (unit.empty() || unit == "ms") mul = 1;
    else if (unit == "s" || unit == "sn") mul = 1000;
    else if (unit == "m" || unit == "dk") mul = 60'000;
    else if (unit == "h" || unit == "sa") mul = 3'600'000;
    else if (unit == "d" || unit == "g") mul = 86'400'000;
    else return false;
    *ms = n * mul;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Loader

namespace {

std::string to_std(ryml::csubstr s) { return std::string(s.data() ? s.data() : "", s.size()); }

int levenshtein(std::string_view a, std::string_view b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= b.size(); ++j) {
            const int sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, sub});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

bool is_null_scalar(std::string_view s) { return s.empty() || s == "~" || s == "null" || s == "Null" || s == "NULL"; }

bool has_control_chars(std::string_view s) {
    return std::any_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; });
}

bool is_plain_yaml_safe(std::string_view s) {
    if (s.empty() || s.front() == '-' || s.front() == ' ' || s.back() == ' ') return false;
    static constexpr std::string_view kReserved[] = {"true", "false", "yes", "no", "on", "off", "null", "~", "y", "n"};
    const std::string lower = to_lower_ascii(s);
    for (auto r : kReserved)
        if (lower == r) return false;
    bool all_digits = true;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '-' || c == '+' || c == '.' || c == ' ';
        if (!ok) return false;
        if (!(c >= '0' && c <= '9')) all_digits = false;
    }
    return !all_digits;
}

std::string yaml_quote(std::string_view s) {
    if (is_plain_yaml_safe(s)) return std::string(s);
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "''";
        else out.push_back(c);
    }
    out += "'";
    return out;
}

bool valid_color(std::string_view s) {
    if (iequals_ascii(s, "auto")) return true;
    if (s.size() != 4 && s.size() != 7 && s.size() != 9) return false;
    if (s[0] != '#') return false;
    for (size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Bool: return "true/false";
        case Kind::Int: return loc("an integer", "tam sayı");
        case Kind::Float: return loc("a number", "sayı");
        case Kind::String: return loc("text", "metin");
        case Kind::Enum: return loc("a choice", "seçenek");
        case Kind::Path: return loc("a path", "yol");
        case Kind::Hotkey: return loc("a hotkey", "kısayol");
        case Kind::Color: return loc("a color", "renk");
        case Kind::Duration: return loc("a duration", "süre");
        case Kind::StringList: return loc("a list of text", "metin listesi");
        case Kind::PathList: return loc("a list of paths", "yol listesi");
        case Kind::GlobList: return loc("a list of patterns", "desen listesi");
        case Kind::ObjectList: return loc("a list of objects", "nesne listesi");
    }
    return "?";
}

bool is_list_kind(Kind k) { return k == Kind::StringList || k == Kind::PathList || k == Kind::GlobList; }

}  // namespace

class SettingsLoader {
public:
    SettingsLoader(const Schema& schema, std::string filename) : schema_(schema), filename_(std::move(filename)) {}

    static std::shared_ptr<Settings> defaults(const Schema& schema) {
        auto s = std::make_shared<Settings>();
        for (const auto& d : schema.all()) s->values_[d.key] = Settings::Entry{normalized_default(d), Source::Default, 0};
        return s;
    }

    // Object list defaults may list only some fields (the YAML writer shows just those); the
    // value itself gets every field in schema order, exactly like a loaded list.
    static Value normalized_default(const SettingDef& d) {
        if (d.kind != Kind::ObjectList) return d.def;
        Value::Objects out;
        for (const auto& obj : d.def.as_objects()) {
            Object full;
            for (const auto& fd : d.fields) {
                const Value* v = obj.find(fd.key);
                full.fields.emplace_back(fd.key, v ? *v : fd.def);
            }
            out.push_back(std::move(full));
        }
        return Value(std::move(out));
    }

    LoadResult load(std::string_view yaml) {
        LoadResult result;
        auto settings = defaults(schema_);
        result.settings = settings;

        install_ryml_error_handler();
        if (trim(yaml).empty()) return result;

        try {
            ryml::EventHandlerTree handler;
            ryml::Parser parser(&handler, ryml::ParserOptions().locations(true));
            ryml::Tree tree = ryml::parse_in_arena(&parser, ryml::to_csubstr(filename_),
                                                   ryml::csubstr(yaml.data(), yaml.size()));
            parser_ = &parser;
            ryml::ConstNodeRef root = tree.crootref();
            if (root.is_stream()) {
                if (root.num_children() == 0) return result;
                if (root.num_children() > 1) warn(root, "", loc("the file has more than one YAML document; only the first is read", "dosyada birden fazla YAML belgesi var; sadece ilki okunur"));
                root = root.first_child();
            }
            if (!root.is_container() && (!root.has_val() || is_null_scalar(to_std(root.val())))) {
                return result;  // empty document (only comments, or "~")
            }
            if (!root.is_map()) {
                error(root, "", loc("the top level of the file must be a key: value mapping", "dosyanın en üst seviyesi anahtar: değer eşlemesi olmalı"));
                result.diagnostics = std::move(diags_);
                return result;
            }
            walk_map(root, "", *settings);
            parser_ = nullptr;
        } catch (const RymlError& e) {
            Diagnostic d;
            d.severity = Diagnostic::Severity::Error;
            d.file = filename_;
            d.line = static_cast<int>(e.line) + 1;
            d.column = static_cast<int>(e.col) + 1;
            d.message = std::string(loc("YAML syntax error: ", "YAML sözdizimi hatası: ")) + e.message;
            diags_.push_back(std::move(d));
            result.parse_failed = true;
            result.settings = defaults(schema_);
        }
        result.diagnostics = std::move(diags_);
        return result;
    }

private:
    void add_diag(Diagnostic::Severity sev, ryml::ConstNodeRef node, std::string key, std::string message) {
        Diagnostic d;
        d.severity = sev;
        d.file = filename_;
        d.key = std::move(key);
        d.message = std::move(message);
        if (parser_ && !node.invalid()) {
            const ryml::Location loc = parser_->location(node);
            d.line = static_cast<int>(loc.line) + 1;
            d.column = static_cast<int>(loc.col) + 1;
        }
        diags_.push_back(std::move(d));
    }
    void error(ryml::ConstNodeRef n, std::string key, std::string msg) {
        add_diag(Diagnostic::Severity::Error, n, std::move(key), std::move(msg));
    }
    void warn(ryml::ConstNodeRef n, std::string key, std::string msg) {
        add_diag(Diagnostic::Severity::Warning, n, std::move(key), std::move(msg));
    }
    int line_of(ryml::ConstNodeRef n) const {
        if (!parser_) return 0;
        return static_cast<int>(parser_->location(n).line) + 1;
    }

    std::string suggestion(const std::string& key) const {
        std::string best;
        int best_d = 4;
        for (const auto& d : schema_.all()) {
            const int dist = levenshtein(key, d.key);
            if (dist < best_d) {
                best_d = dist;
                best = d.key;
            }
        }
        return best;
    }

    void walk_map(ryml::ConstNodeRef map, const std::string& prefix, Settings& out) {
        std::vector<std::string> seen;
        for (ryml::ConstNodeRef child : map.children()) {
            const std::string name = to_std(child.key());
            const std::string key = prefix.empty() ? name : prefix + "." + name;
            if (std::find(seen.begin(), seen.end(), name) != seen.end())
                warn(child, key, loc("key written more than once; the last one wins", "anahtar birden fazla kez yazılmış; sonuncusu geçerli"));
            seen.push_back(name);

            if (const SettingDef* def = schema_.find(key)) {
                Value v;
                if (convert(child, *def, key, &v)) out.values_[key] = Settings::Entry{std::move(v), Source::User, line_of(child)};
                continue;
            }
            if (schema_.has_section(key)) {
                if (child.is_map()) {
                    walk_map(child, key, out);
                } else if (!(child.has_val() && is_null_scalar(std::string_view(child.val().data(), child.val().size())))) {
                    error(child, key, loc("this is a section; it needs key: value lines below it", "bu bir bölüm; altında anahtar: değer satırları olmalı"));
                }
                continue;
            }
            std::string msg = loc("unknown setting", "bilinmeyen ayar");
            const std::string s = suggestion(key);
            if (!s.empty()) msg += std::string(loc(" (did you mean '", " (şunu mu demek istediniz: '")) + s + "'?)";
            warn(child, key, msg);
        }
    }

    // Converts `node` according to `def`. Returns false (after reporting) when the value is
    // invalid; the caller then keeps the default.
    bool convert(ryml::ConstNodeRef node, const SettingDef& def, const std::string& key, Value* out) {
        if (def.kind == Kind::ObjectList) return convert_objects(node, def, key, out);

        if (is_list_kind(def.kind)) {
            Value::List list;
            if (node.is_seq()) {
                for (ryml::ConstNodeRef item : node.children()) {
                    if (item.is_container()) {
                        error(item, key, loc("list items must be plain values", "liste elemanı düz bir değer olmalı"));
                        return false;
                    }
                    std::string s(trim(to_std(item.val())));
                    if (def.kind == Kind::PathList && item.is_val_dquo() && has_control_chars(s))
                        warn(item, key, loc("inside double quotes \\ is an escape character; use single quotes for Windows paths", "çift tırnak içinde \\ kaçış karakteridir; Windows yolları için tek tırnak kullanın"));
                    if (s.empty()) {
                        warn(item, key, loc("empty list item ignored", "boş liste elemanı yok sayıldı"));
                        continue;
                    }
                    list.push_back(std::move(s));
                }
            } else if (node.is_map()) {
                error(node, key, std::string(loc("expected ", "beklenen: ")) + kind_name(def.kind));
                return false;
            } else {
                const std::string s(trim(to_std(node.val())));
                if (!is_null_scalar(s)) list.push_back(s);
            }
            *out = Value(std::move(list));
            return true;
        }

        if (node.is_container()) {
            error(node, key, std::string(loc("expected ", "beklenen: ")) + kind_name(def.kind) + loc(", not a list or mapping", ", liste/eşleme değil"));
            return false;
        }
        const std::string raw(trim(to_std(node.val())));
        if (is_null_scalar(raw) && !node.is_val_quoted()) {
            *out = def.def;  // "key:" with no value -> default, silently
            return true;
        }
        return convert_scalar(node, def, key, raw, out);
    }

    bool convert_scalar(ryml::ConstNodeRef node, const SettingDef& def, const std::string& key,
                        const std::string& raw, Value* out) {
        auto bad = [&](std::string msg) {
            error(node, key, std::move(msg));
            return false;
        };
        switch (def.kind) {
            case Kind::Bool: {
                const std::string l = to_lower_ascii(raw);
                if (l == "true" || l == "yes" || l == "on" || l == "evet" || l == "açık") *out = Value(true);
                else if (l == "false" || l == "no" || l == "off" || l == "hayır" || l == "kapalı") *out = Value(false);
                else return bad(std::string(loc("must be true or false, got '", "true veya false olmalı, '")) + raw + loc("'", "' yazılmış"));
                return true;
            }
            case Kind::Int: {
                char* end = nullptr;
                errno = 0;
                const long long v = std::strtoll(raw.c_str(), &end, 10);
                if (end == raw.c_str() || *end != '\0' || errno == ERANGE) return bad(std::string(loc("must be an integer, got '", "tam sayı olmalı, '")) + raw + loc("'", "' yazılmış"));
                if (v < def.min || v > def.max)
                    return bad(std::string(loc("must be between ", "")) + std::to_string(def.min) + loc(" and ", " ile ") + std::to_string(def.max) +
                               loc("", " arasında olmalı") + " (" + raw + ")");
                *out = Value(static_cast<int64_t>(v));
                return true;
            }
            case Kind::Float: {
                char* end = nullptr;
                const double v = std::strtod(raw.c_str(), &end);
                if (end == raw.c_str() || *end != '\0') return bad(std::string(loc("must be a number, got '", "sayı olmalı, '")) + raw + loc("'", "' yazılmış"));
                if ((def.min != INT64_MIN && v < static_cast<double>(def.min)) ||
                    (def.max != INT64_MAX && v > static_cast<double>(def.max)))
                    return bad(std::string(loc("must be between ", "")) + std::to_string(def.min) + loc(" and ", " ile ") + std::to_string(def.max) +
                               loc("", " arasında olmalı"));
                *out = Value(v);
                return true;
            }
            case Kind::Enum: {
                for (const auto& c : def.choices) {
                    if (iequals_ascii(c, raw)) {
                        *out = Value(c);
                        return true;
                    }
                }
                std::string all;
                for (const auto& c : def.choices) all += (all.empty() ? "" : " | ") + c;
                return bad(std::string(loc("invalid choice '", "geçersiz seçenek '")) + raw + loc("'; valid: ", "'; geçerli: ") + all);
            }
            case Kind::Hotkey: {
                if (raw.empty() && def.def.as_string().empty()) {  // optional hotkey (e.g. of a command)
                    *out = Value("");
                    return true;
                }
                std::string err;
                auto hk = parse_hotkey(raw, &err);
                if (!hk) return bad(err);
                *out = Value(format_hotkey(*hk));
                return true;
            }
            case Kind::Color: {
                if (!valid_color(raw)) return bad(std::string(loc("a color must be 'auto' or #RRGGBB, got '", "renk 'auto' veya #RRGGBB biçiminde olmalı, '")) + raw +
                                                   loc("'", "' yazılmış"));
                *out = Value(to_lower_ascii(raw));
                return true;
            }
            case Kind::Duration: {
                int64_t ms = 0;
                if (!parse_duration(raw, &ms)) return bad(loc("a duration looks like 250ms, 30s, 5m, 1h or 2d", "süre 250ms, 30s, 5m, 1h veya 2d biçiminde olmalı"));
                if (ms < def.min || ms > def.max)
                    return bad(std::string(loc("must be between ", "")) + format_duration(def.min) + loc(" and ", " ile ") + format_duration(def.max) +
                               loc("", " arasında olmalı"));
                *out = Value(ms);
                return true;
            }
            case Kind::Path:
                if (node.is_val_dquo() && has_control_chars(raw))
                    warn(node, key, loc("inside double quotes \\ is an escape character (\\n, \\t ...); use single quotes for Windows paths", "çift tırnak içinde \\ kaçış karakteridir (\\n, \\t ...); Windows yolları için tek tırnak kullanın"));
                *out = Value(raw);
                return true;
            case Kind::String:
                *out = Value(raw);
                return true;
            default:
                return bad(loc("unsupported type", "desteklenmeyen tür"));
        }
    }

    bool convert_objects(ryml::ConstNodeRef node, const SettingDef& def, const std::string& key, Value* out) {
        Value::Objects objects;
        if (!node.is_seq()) {
            if (!node.is_container() && is_null_scalar(to_std(node.val()))) {
                *out = Value(std::move(objects));
                return true;
            }
            error(node, key, loc("must be a list (each item as '- field: value')", "liste olmalı (her eleman '- alan: değer' biçiminde)"));
            return false;
        }
        size_t index = 0;
        for (ryml::ConstNodeRef item : node.children()) {
            const std::string item_key = key + "[" + std::to_string(index++) + "]";
            if (!item.is_map()) {
                error(item, item_key, loc("each item must be a mapping ({ field: value, ... })", "eleman bir eşleme olmalı ({ alan: değer, ... })"));
                continue;
            }
            Object obj;
            bool ok = true;
            for (ryml::ConstNodeRef f : item.children()) {
                const std::string fname = to_std(f.key());
                auto it = std::find_if(def.fields.begin(), def.fields.end(), [&](const SettingDef& fd) { return fd.key == fname; });
                if (it == def.fields.end()) {
                    warn(f, item_key + "." + fname, loc("unknown field", "bilinmeyen alan"));
                    continue;
                }
                Value v;
                if (!convert(f, *it, item_key + "." + fname, &v)) {
                    if (it->required) ok = false;
                    continue;
                }
                obj.fields.emplace_back(fname, std::move(v));
            }
            for (const auto& fd : def.fields) {
                if (obj.find(fd.key)) continue;
                if (fd.required) {
                    error(item, item_key, std::string(loc("required field missing: '", "zorunlu alan eksik: '")) + fd.key + "'");
                    ok = false;
                } else {
                    obj.fields.emplace_back(fd.key, fd.def);
                }
            }
            if (ok) {
                // Keep field order stable (schema order) so comparisons and writers are deterministic.
                Object ordered;
                for (const auto& fd : def.fields)
                    if (const Value* v = obj.find(fd.key)) ordered.fields.emplace_back(fd.key, *v);
                objects.push_back(std::move(ordered));
            }
        }
        *out = Value(std::move(objects));
        return true;
    }

    const Schema& schema_;
    std::string filename_;
    ryml::Parser* parser_ = nullptr;
    std::vector<Diagnostic> diags_;
};

std::shared_ptr<const Settings> default_settings(const Schema& schema) { return SettingsLoader::defaults(schema); }

LoadResult load_settings_text(const Schema& schema, std::string_view yaml, std::string_view filename) {
    return SettingsLoader(schema, std::string(filename)).load(yaml);
}

LoadResult load_settings_file(const Schema& schema, const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LoadResult r;
        r.settings = default_settings(schema);
        return r;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);  // UTF-8 BOM (Notepad)
    return load_settings_text(schema, text, path.filename().string());
}

// ---------------------------------------------------------------------------------------------
// Writers

namespace {

std::string scalar_to_yaml(const SettingDef& def, const Value& v) {
    switch (def.kind) {
        case Kind::Bool: return v.as_bool() ? "true" : "false";
        case Kind::Int: return std::to_string(v.as_int());
        case Kind::Float: {
            std::ostringstream ss;
            ss << v.as_float();
            return ss.str();
        }
        case Kind::Duration: return format_duration(v.as_int());
        default: return yaml_quote(v.as_string());
    }
}

std::string list_to_yaml(const Value::List& list) {
    std::string out = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i) out += ", ";
        out += yaml_quote(list[i]);
    }
    return out + "]";
}

std::string value_to_yaml_inline(const SettingDef& def, const Value& v) {
    if (is_list_kind(def.kind)) return list_to_yaml(v.as_list());
    if (def.kind == Kind::ObjectList) {
        if (v.as_objects().empty()) return "[]";
        return "";  // block form, written separately
    }
    return scalar_to_yaml(def, v);
}

std::string describe(const SettingDef& def) {
    std::string extra;
    if (def.kind == Kind::Enum) {
        for (const auto& c : def.choices) extra += (extra.empty() ? "" : " | ") + c;
    } else if (def.kind == Kind::Int && def.min != INT64_MIN && def.max != INT64_MAX) {
        extra = std::to_string(def.min) + ".." + std::to_string(def.max);
    } else if (def.kind == Kind::Duration && def.min != INT64_MIN && def.max != INT64_MAX) {
        extra = format_duration(def.min) + ".." + format_duration(def.max);
    }
    return extra;
}

std::vector<std::string_view> split_key(std::string_view key) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    for (size_t i = 0; i <= key.size(); ++i) {
        if (i == key.size() || key[i] == '.') {
            parts.push_back(key.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

void write_comment_block(std::string& out, const std::string& indent, const std::string& text) {
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) out += indent + "# " + line + "\n";
}

std::string json_escape(std::string_view s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

std::string json_value(const SettingDef& def, const Value& v);

std::string json_object_value(const SettingDef& def, const Object& o) {
    std::string out = "{";
    bool first = true;
    for (const auto& fd : def.fields) {
        const Value* fv = o.find(fd.key);
        if (!fv) continue;
        if (!first) out += ",";
        first = false;
        out += "\"" + json_escape(fd.key) + "\":" + json_value(fd, *fv);
    }
    return out + "}";
}

std::string json_value(const SettingDef& def, const Value& v) {
    switch (def.kind) {
        case Kind::Bool: return v.as_bool() ? "true" : "false";
        case Kind::Int: return std::to_string(v.as_int());
        case Kind::Float: {
            std::ostringstream ss;
            ss << v.as_float();
            return ss.str();
        }
        case Kind::Duration: return "\"" + format_duration(v.as_int()) + "\"";
        case Kind::StringList:
        case Kind::PathList:
        case Kind::GlobList: {
            std::string out = "[";
            for (size_t i = 0; i < v.as_list().size(); ++i) out += (i ? "," : "") + ("\"" + json_escape(v.as_list()[i]) + "\"");
            return out + "]";
        }
        case Kind::ObjectList: {
            std::string out = "[";
            for (size_t i = 0; i < v.as_objects().size(); ++i) out += (i ? "," : "") + json_object_value(def, v.as_objects()[i]);
            return out + "]";
        }
        default: return "\"" + json_escape(v.as_string()) + "\"";
    }
}

std::string json_type(const SettingDef& def, const std::string& indent);

std::string json_leaf(const SettingDef& def, const std::string& indent) {
    std::string desc = setting_title(def);
    if (!setting_description(def).empty()) desc += desc.empty() ? setting_description(def) : " — " + setting_description(def);
    std::string out = "{\n";
    out += indent + "  \"description\": \"" + json_escape(desc) + "\",\n";
    out += indent + "  " + json_type(def, indent + "  ");
    if (!(def.kind == Kind::ObjectList && def.required)) out += ",\n" + indent + "  \"default\": " + json_value(def, def.def);
    out += "\n" + indent + "}";
    return out;
}

std::string json_type(const SettingDef& def, const std::string& indent) {
    switch (def.kind) {
        case Kind::Bool: return "\"type\": \"boolean\"";
        case Kind::Int: {
            std::string s = "\"type\": \"integer\"";
            if (def.min != INT64_MIN) s += ", \"minimum\": " + std::to_string(def.min);
            if (def.max != INT64_MAX) s += ", \"maximum\": " + std::to_string(def.max);
            return s;
        }
        case Kind::Float: return "\"type\": \"number\"";
        case Kind::Enum: {
            std::string s = "\"type\": \"string\", \"enum\": [";
            for (size_t i = 0; i < def.choices.size(); ++i) s += (i ? ", " : "") + ("\"" + json_escape(def.choices[i]) + "\"");
            return s + "]";
        }
        case Kind::Color: return "\"type\": \"string\", \"pattern\": \"^(auto|#([0-9a-fA-F]{3}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8}))$\"";
        case Kind::Duration: return "\"type\": [\"string\", \"integer\"], \"pattern\": \"^[0-9]+\\\\s*(ms|s|m|h|d)?$\"";
        case Kind::StringList:
        case Kind::PathList:
        case Kind::GlobList: return "\"type\": [\"array\", \"string\"], \"items\": { \"type\": \"string\" }";
        case Kind::ObjectList: {
            std::string s = "\"type\": \"array\",\n" + indent + "\"items\": {\n";
            s += indent + "  \"type\": \"object\",\n" + indent + "  \"additionalProperties\": false,\n";
            std::string req;
            for (const auto& f : def.fields)
                if (f.required) req += (req.empty() ? "" : ", ") + ("\"" + json_escape(f.key) + "\"");
            if (!req.empty()) s += indent + "  \"required\": [" + req + "],\n";
            s += indent + "  \"properties\": {\n";
            for (size_t i = 0; i < def.fields.size(); ++i) {
                s += indent + "    \"" + json_escape(def.fields[i].key) + "\": " + json_leaf(def.fields[i], indent + "    ");
                s += i + 1 < def.fields.size() ? ",\n" : "\n";
            }
            s += indent + "  }\n" + indent + "}";
            return s;
        }
        default: return "\"type\": \"string\"";
    }
}

struct JsonNode {
    std::string name;
    const SettingDef* leaf = nullptr;
    std::vector<JsonNode> children;

    JsonNode& child(std::string_view n) {
        for (auto& c : children)
            if (c.name == n) return c;
        children.push_back(JsonNode{std::string(n), nullptr, {}});
        return children.back();
    }
};

void json_emit_object(std::string& out, const JsonNode& node, const Schema& schema, const std::string& path,
                      const std::string& indent) {
    out += "{\n" + indent + "  \"type\": \"object\",\n" + indent + "  \"additionalProperties\": false,\n";
    const auto title = schema.section_title(path);
    if (!title.empty()) out += indent + "  \"description\": \"" + json_escape(title) + "\",\n";
    out += indent + "  \"properties\": {\n";
    for (size_t i = 0; i < node.children.size(); ++i) {
        const auto& c = node.children[i];
        out += indent + "    \"" + json_escape(c.name) + "\": ";
        const std::string child_path = path.empty() ? c.name : path + "." + c.name;
        if (c.leaf) out += json_leaf(*c.leaf, indent + "    ");
        else json_emit_object(out, c, schema, child_path, indent + "    ");
        out += i + 1 < node.children.size() ? ",\n" : "\n";
    }
    out += indent + "  }\n" + indent + "}";
}

}  // namespace

std::string generate_default_yaml(const Schema& schema, std::string_view schema_ref) {
    std::string out;
    if (!schema_ref.empty()) out += "# yaml-language-server: $schema=" + std::string(schema_ref) + "\n";
    out += loc(
        "# Kamil settings\n"
        "#\n"
        "# This file is reloaded as soon as it is saved. An invalid value falls back to its default and\n"
        "# is reported in the tray with its line number. Write Windows paths in single quotes:\n"
        "#   'D:\\src\\project'   (inside double quotes \\ is an escape character)\n"
        "# Any setting that is deleted or commented out returns to its default.\n",
        "# Kamil ayarları\n"
        "#\n"
        "# Bu dosya kaydedildiği anda yeniden yüklenir. Hatalı bir değer varsayılanına döner ve\n"
        "# tray simgesinde satır numarasıyla bildirilir. Windows yollarını tek tırnakla yazın:\n"
        "#   'D:\\src\\proje'   (çift tırnak içinde \\ kaçış karakteridir)\n"
        "# Silinen veya yorum satırı yapılan her ayar varsayılan değerine döner.\n");

    std::vector<std::string_view> open;  // currently open map path
    for (const auto& def : schema.all()) {
        const auto parts = split_key(def.key);
        size_t common = 0;
        while (common < open.size() && common + 1 < parts.size() && open[common] == parts[common]) ++common;
        open.resize(common);
        for (size_t i = common; i + 1 < parts.size(); ++i) {
            const std::string indent(i * 2, ' ');
            std::string section;
            for (size_t k = 0; k <= i; ++k) section += (k ? "." : "") + std::string(parts[k]);
            out += "\n";
            const auto title = schema.section_title(section);
            if (!title.empty()) write_comment_block(out, indent, std::string(title));
            out += indent + std::string(parts[i]) + ":\n";
            open.push_back(parts[i]);
        }
        const std::string indent((parts.size() - 1) * 2, ' ');
        std::string comment = setting_title(def);
        const std::string& desc = setting_description(def);
        if (!desc.empty()) comment += comment.empty() ? desc : " — " + desc;
        const std::string extra = describe(def);
        if (!extra.empty()) comment += "\n(" + extra + ")";
        if (def.apply == Apply::Restart) comment += loc("\nRestart Kamil for a change to take effect.", "\nDeğişiklik için Kamil'i yeniden başlatın.");
        out += "\n";
        write_comment_block(out, indent, comment);
        const std::string inline_value = value_to_yaml_inline(def, def.def);
        if (def.kind == Kind::ObjectList && !def.def.as_objects().empty()) {
            out += indent + std::string(parts.back()) + ":\n";
            for (const auto& obj : def.def.as_objects()) {
                bool first = true;
                for (const auto& fd : def.fields) {
                    const Value* fv = obj.find(fd.key);
                    if (!fv) continue;
                    out += indent + (first ? "  - " : "    ") + fd.key + ": " + value_to_yaml_inline(fd, *fv) + "\n";
                    first = false;
                }
            }
        } else {
            out += indent + std::string(parts.back()) + ": " + inline_value + "\n";
        }
        if (def.kind == Kind::ObjectList && !def.fields.empty()) {
            std::string example = std::string(loc("Example:\n", "Örnek:\n")) + std::string(parts.back()) + ":\n";
            bool first = true;
            for (const auto& fd : def.fields) {
                example += (first ? "  - " : "    ") + fd.key + ": " +
                           (setting_description(fd).empty() ? std::string("...") : setting_description(fd)) + "\n";
                first = false;
            }
            write_comment_block(out, indent, example);
        }
    }
    return out;
}

std::string generate_json_schema(const Schema& schema) {
    JsonNode root;
    for (const auto& def : schema.all()) {
        const auto parts = split_key(def.key);
        JsonNode* node = &root;
        for (size_t i = 0; i + 1 < parts.size(); ++i) node = &node->child(parts[i]);
        node->child(parts.back()).leaf = &def;
    }
    std::string out =
        "{\n  \"$schema\": \"http://json-schema.org/draft-07/schema#\",\n  \"title\": \"Kamil settings\",\n  \"allOf\": [\n    ";
    json_emit_object(out, root, schema, "", "    ");
    out += "\n  ]\n}\n";
    return out;
}

// ---------------------------------------------------------------------------------------------
// Store

SettingsStore::SettingsStore(const Schema& schema) : schema_(schema), current_(default_settings(schema)) {}

LoadResult SettingsStore::reload(const std::filesystem::path& path) {
    LoadResult r = load_settings_file(schema_, path);
    if (!r.parse_failed) publish(r.settings);
    else r.settings = current();
    return r;
}

void SettingsStore::replace(std::shared_ptr<const Settings> settings) { publish(std::move(settings)); }

void SettingsStore::publish(std::shared_ptr<const Settings> next) {
    auto prev = current_.exchange(next);
    const auto changed = prev ? next->diff(*prev) : std::vector<std::string>{};
    if (changed.empty()) return;
    std::vector<Sub> subs;
    {
        std::lock_guard lock(mutex_);
        subs = subs_;
    }
    for (const auto& s : subs) {
        const bool hit = s.prefix.empty() || std::any_of(changed.begin(), changed.end(), [&](const std::string& k) {
                             return k == s.prefix || (k.size() > s.prefix.size() && k.compare(0, s.prefix.size(), s.prefix) == 0 &&
                                                      k[s.prefix.size()] == '.');
                         });
        if (hit) s.cb(*next, changed);
    }
}

int SettingsStore::subscribe(std::string prefix, Callback cb) {
    std::lock_guard lock(mutex_);
    subs_.push_back(Sub{next_id_, std::move(prefix), std::move(cb)});
    return next_id_++;
}

void SettingsStore::unsubscribe(int id) {
    std::lock_guard lock(mutex_);
    subs_.erase(std::remove_if(subs_.begin(), subs_.end(), [id](const Sub& s) { return s.id == id; }), subs_.end());
}

}  // namespace kamil
