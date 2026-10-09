#include "core/cmake.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>

#ifndef _RYML_SINGLE_HEADER_AMALGAMATED_HPP_
#include <ryml_all.hpp>
#endif

#include "core/ryml_support.h"
#include "core/i18n.h"
#include "core/text.h"

namespace kamil {

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------------------------
// JSON helpers (rapidyaml parses JSON)

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF)
        s.erase(0, 3);
    return s;
}

std::string str(ryml::csubstr s) { return std::string(s.data() ? s.data() : "", s.size()); }

ryml::ConstNodeRef child(ryml::ConstNodeRef n, const char* key) {
    if (n.invalid() || !n.is_map()) return {};
    const ryml::csubstr k = ryml::to_csubstr(key);
    return n.has_child(k) ? n[k] : ryml::ConstNodeRef{};
}

std::string child_str(ryml::ConstNodeRef n, const char* key) {
    ryml::ConstNodeRef c = child(n, key);
    if (c.invalid() || c.is_container() || !c.has_val()) return {};
    return str(c.val());
}

bool child_bool(ryml::ConstNodeRef n, const char* key) { return child_str(n, key) == "true"; }

fs::path utf8_path(std::string_view s) { return fs::path(widen(s)); }

// ---------------------------------------------------------------------------------------------
// Presets

struct Condition {
    std::string type;  // const, equals, notEquals, inList, notInList, matches, notMatches, anyOf, allOf, not
    bool value = true;
    std::string lhs, rhs, string, regex;
    std::vector<std::string> list;
    std::vector<Condition> conditions;
    std::vector<Condition> inner;  // "not": one element
};

Condition parse_condition(ryml::ConstNodeRef n) {
    Condition c;
    if (n.invalid()) return c;
    if (!n.is_map()) {  // a literal true/false/null is allowed
        c.type = "const";
        c.value = n.has_val() && str(n.val()) != "false";
        return c;
    }
    c.type = child_str(n, "type");
    c.value = child_str(n, "value") != "false";
    c.lhs = child_str(n, "lhs");
    c.rhs = child_str(n, "rhs");
    c.string = child_str(n, "string");
    c.regex = child_str(n, "regex");
    if (auto l = child(n, "list"); !l.invalid() && l.is_seq())
        for (auto i : l.children()) c.list.push_back(str(i.val()));
    if (auto l = child(n, "conditions"); !l.invalid() && l.is_seq())
        for (auto i : l.children()) c.conditions.push_back(parse_condition(i));
    if (auto inner = child(n, "condition"); !inner.invalid()) c.inner.push_back(parse_condition(inner));
    return c;
}

struct RawPreset {
    std::string name, display_name, generator, binary_dir, build_type;
    std::vector<std::string> inherits;
    bool hidden = false;
    bool has_condition = false;
    Condition condition;
    fs::path file_dir;
};

struct Resolved {
    std::string display_name, generator, binary_dir, build_type;
    fs::path file_dir;  // of the preset that provided binaryDir
    const Condition* condition = nullptr;
};

class PresetLoader {
public:
    PresetLoader(fs::path source_dir, const EnvLookup& env, std::string_view host)
        : source_(std::move(source_dir)), env_(env), host_(host) {}

    PresetsResult run() {
        PresetsResult r;
        install_ryml_error_handler();
        const fs::path main = source_ / "CMakePresets.json";
        const fs::path user = source_ / "CMakeUserPresets.json";
        std::error_code ec;
        const bool has_main = fs::exists(main, ec), has_user = fs::exists(user, ec);
        r.found = has_main || has_user;
        if (has_main) load_file(main);
        if (has_user) load_file(user);  // implicitly includes CMakePresets.json (already loaded)
        for (const auto& name : order_) {
            const RawPreset& p = raw_.at(name);
            if (p.hidden) continue;
            std::set<std::string> visiting;
            Resolved res;
            if (!resolve(name, res, visiting)) continue;
            if (res.condition && !eval(*res.condition, name, res)) continue;
            ConfigurePreset out;
            out.name = name;
            out.display_name = expand(res.display_name, name, res);
            out.generator = res.generator;
            out.build_type = expand(res.build_type, name, res);
            std::string bin = res.binary_dir.empty() ? std::string("${sourceDir}/out/build/${presetName}") : res.binary_dir;
            fs::path dir = utf8_path(expand(bin, name, res));
            if (dir.is_relative()) dir = source_ / dir;
            out.binary_dir = dir.lexically_normal().make_preferred();
            r.presets.push_back(std::move(out));
        }
        r.errors = std::move(errors_);
        return r;
    }

private:
    void load_file(const fs::path& file) {
        std::error_code ec;
        const fs::path canonical = fs::weakly_canonical(file, ec);
        if (!loaded_.insert(canonical.empty() ? file : canonical).second) return;
        const std::string text = read_file(file);
        if (text.empty()) return;
        try {
            ryml::Tree tree = ryml::parse_json_in_arena(ryml::to_csubstr(narrow(file.filename().wstring())),
                                                        ryml::csubstr(text.data(), text.size()));
            ryml::ConstNodeRef root = tree.crootref();
            if (auto inc = child(root, "include"); !inc.invalid() && inc.is_seq()) {
                for (auto i : inc.children()) {
                    fs::path p = utf8_path(str(i.val()));
                    if (p.is_relative()) p = file.parent_path() / p;
                    load_file(p);
                }
            }
            auto list = child(root, "configurePresets");
            if (list.invalid() || !list.is_seq()) return;
            for (auto n : list.children()) {
                RawPreset p;
                p.name = child_str(n, "name");
                if (p.name.empty()) continue;
                p.display_name = child_str(n, "displayName");
                p.generator = child_str(n, "generator");
                p.binary_dir = child_str(n, "binaryDir");
                p.hidden = child_bool(n, "hidden");
                p.file_dir = file.parent_path();
                if (auto inh = child(n, "inherits"); !inh.invalid()) {
                    if (inh.is_seq()) {
                        for (auto i : inh.children()) p.inherits.push_back(str(i.val()));
                    } else if (inh.has_val()) {
                        p.inherits.push_back(str(inh.val()));
                    }
                }
                if (auto cache = child(n, "cacheVariables"); !cache.invalid()) {
                    auto bt = child(cache, "CMAKE_BUILD_TYPE");
                    if (!bt.invalid()) p.build_type = bt.is_map() ? child_str(bt, "value") : (bt.has_val() ? str(bt.val()) : "");
                }
                if (auto cond = child(n, "condition"); !cond.invalid()) {
                    p.has_condition = true;
                    p.condition = parse_condition(cond);
                }
                if (!raw_.count(p.name)) order_.push_back(p.name);
                raw_[p.name] = std::move(p);
            }
        } catch (const RymlError& e) {
            errors_.push_back(narrow(file.filename().wstring()) + ":" + std::to_string(e.line + 1) + ": " + e.message);
        }
    }

    // Own fields win; otherwise the first parent (in `inherits` order) that has the field.
    bool resolve(const std::string& name, Resolved& out, std::set<std::string>& visiting) {
        auto it = raw_.find(name);
        if (it == raw_.end()) {
            errors_.push_back(std::string(loc("unknown preset: ", "bilinmeyen preset: ")) + name);
            return false;
        }
        if (!visiting.insert(name).second) {
            errors_.push_back(std::string(loc("circular inherits: ", "döngüsel inherits: ")) + name);
            return false;
        }
        const RawPreset& p = it->second;
        auto take = [](std::string& dst, const std::string& src) {
            if (dst.empty()) dst = src;
        };
        take(out.display_name, p.display_name);
        take(out.generator, p.generator);
        if (out.binary_dir.empty() && !p.binary_dir.empty()) {
            out.binary_dir = p.binary_dir;
            out.file_dir = p.file_dir;
        }
        take(out.build_type, p.build_type);
        if (!out.condition && p.has_condition) out.condition = &p.condition;
        for (const auto& parent : p.inherits)
            if (!resolve(parent, out, visiting)) return false;
        visiting.erase(name);
        if (out.file_dir.empty()) out.file_dir = p.file_dir;
        return true;
    }

    std::string expand(const std::string& in, const std::string& preset, const Resolved& res) const {
        std::string out;
        for (size_t i = 0; i < in.size();) {
            if (in[i] != '$') {
                out.push_back(in[i++]);
                continue;
            }
            auto macro = [&](std::string_view prefix) { return in.compare(i, prefix.size(), prefix) == 0; };
            if (!macro("${") && !macro("$env{") && !macro("$penv{") && !macro("$vendor{")) {
                out.push_back(in[i++]);
                continue;
            }
            const size_t close = in.find('}', i);
            if (close == std::string::npos) {
                out.append(in, i, std::string::npos);
                break;
            }
            const std::string token = in.substr(i, close - i + 1);
            std::string value;
            if (token == "${sourceDir}") value = narrow(source_.wstring());
            else if (token == "${sourceParentDir}") value = narrow(source_.parent_path().wstring());
            else if (token == "${sourceDirName}") value = narrow(source_.filename().wstring());
            else if (token == "${presetName}") value = preset;
            else if (token == "${generator}") value = res.generator;
            else if (token == "${hostSystemName}") value = host_;
            else if (token == "${fileDir}") value = narrow(res.file_dir.wstring());
            else if (token == "${dollar}") value = "$";
            else if (token == "${pathListSep}") value = ";";
            else if (macro("$env{") || macro("$penv{")) {
                const size_t open = in.find('{', i);
                const std::string var = in.substr(open + 1, close - open - 1);
                if (env_)
                    if (auto v = env_(var)) value = *v;
            } else if (macro("$vendor{")) {
                value.clear();
            } else {
                value = token;  // unknown macro: keep verbatim
            }
            out += value;
            i = close + 1;
        }
        return out;
    }

    bool eval(const Condition& c, const std::string& preset, const Resolved& res) const {
        auto x = [&](const std::string& s) { return expand(s, preset, res); };
        if (c.type.empty() || c.type == "const") return c.value;
        if (c.type == "equals") return x(c.lhs) == x(c.rhs);
        if (c.type == "notEquals") return x(c.lhs) != x(c.rhs);
        if (c.type == "inList" || c.type == "notInList") {
            const std::string s = x(c.string);
            const bool in = std::any_of(c.list.begin(), c.list.end(), [&](const std::string& v) { return x(v) == s; });
            return c.type == "inList" ? in : !in;
        }
        if (c.type == "anyOf") return std::any_of(c.conditions.begin(), c.conditions.end(), [&](const Condition& k) { return eval(k, preset, res); });
        if (c.type == "allOf") return std::all_of(c.conditions.begin(), c.conditions.end(), [&](const Condition& k) { return eval(k, preset, res); });
        if (c.type == "not") return c.inner.empty() || !eval(c.inner.front(), preset, res);
        return true;  // matches/notMatches: not evaluated, assume visible
    }

    fs::path source_;
    const EnvLookup& env_;
    std::string host_;
    std::map<std::string, RawPreset> raw_;
    std::vector<std::string> order_;
    std::set<fs::path> loaded_;
    std::vector<std::string> errors_;
};

fs::file_time_type newest_configure_time(const fs::path& build_dir) {
    std::error_code ec;
    fs::file_time_type best = fs::file_time_type::min();
    const fs::path reply = build_dir / ".cmake" / "api" / "v1" / "reply";
    for (fs::directory_iterator it(reply, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const auto name = it->path().filename().wstring();
        if (name.rfind(L"index-", 0) != 0) continue;
        best = std::max(best, it->last_write_time(ec));
    }
    const auto cache = fs::last_write_time(build_dir / "CMakeCache.txt", ec);
    if (!ec) best = std::max(best, cache);
    return best;
}

}  // namespace

PresetsResult load_cmake_presets(const fs::path& source_dir, const EnvLookup& env, std::string_view host_system) {
    return PresetLoader(source_dir, env, host_system).run();
}

std::optional<size_t> guess_active_preset(const std::vector<ConfigurePreset>& presets) {
    std::optional<size_t> best;
    fs::file_time_type best_time = fs::file_time_type::min();
    for (size_t i = 0; i < presets.size(); ++i) {
        const auto t = newest_configure_time(presets[i].binary_dir);
        if (t > best_time) {
            best_time = t;
            best = i;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------------------------
// File API

bool write_codemodel_query(const fs::path& build_dir) {
    std::error_code ec;
    const fs::path dir = build_dir / ".cmake" / "api" / "v1" / "query" / "client-kamil";
    fs::create_directories(dir, ec);
    if (ec) return false;
    const fs::path file = dir / "codemodel-v2";
    if (fs::exists(file, ec)) return true;
    std::ofstream(file, std::ios::binary).flush();
    return fs::exists(file, ec);
}

namespace {

// Parsed codemodels by build directory. A reply is immutable once written (CMake writes a new
// index-<time>.json on every configure), so the newest index file name + its time identify it.
struct CachedModel {
    fs::path index;
    fs::file_time_type time;
    CodeModel model;
};
std::mutex g_codemodel_mutex;
std::map<fs::path, CachedModel> g_codemodels;

CodeModel parse_codemodel(const fs::path& reply, const fs::path& build_dir, const fs::path& index);

}  // namespace

CodeModel read_codemodel(const fs::path& build_dir) {
    std::error_code ec;
    const fs::path reply = build_dir / ".cmake" / "api" / "v1" / "reply";
    fs::path index;
    for (fs::directory_iterator it(reply, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const auto name = it->path().filename().wstring();
        if (name.rfind(L"index-", 0) == 0 && name.size() > 11 && (index.empty() || name > index.filename().wstring())) index = it->path();
    }
    if (index.empty()) {
        CodeModel cm;
        cm.error = loc("no File API reply (the project has not been configured yet)", "File API yanıtı yok (proje henüz configure edilmemiş)");
        return cm;
    }
    const auto time = fs::last_write_time(index, ec);
    {
        std::lock_guard lock(g_codemodel_mutex);
        auto it = g_codemodels.find(build_dir);
        if (it != g_codemodels.end() && it->second.index == index && it->second.time == time) return it->second.model;
    }
    CodeModel cm = parse_codemodel(reply, build_dir, index);
    std::lock_guard lock(g_codemodel_mutex);
    g_codemodels[build_dir] = CachedModel{index, time, cm};
    return cm;
}

namespace {

CodeModel parse_codemodel(const fs::path& reply, const fs::path& build_dir, const fs::path& index) {
    CodeModel cm;
    install_ryml_error_handler();
    try {
        const std::string index_text = read_file(index);
        ryml::Tree idx = ryml::parse_json_in_arena(ryml::csubstr(index_text.data(), index_text.size()));
        std::string codemodel_file;
        if (auto objects = child(idx.crootref(), "objects"); !objects.invalid() && objects.is_seq()) {
            for (auto o : objects.children()) {
                if (child_str(o, "kind") != "codemodel") continue;
                if (child_str(child(o, "version"), "major") != "2") continue;
                codemodel_file = child_str(o, "jsonFile");
            }
        }
        if (codemodel_file.empty()) {
            cm.error = loc("the File API reply has no codemodel", "File API yanıtında codemodel yok");
            return cm;
        }
        const std::string cm_text = read_file(reply / utf8_path(codemodel_file));
        ryml::Tree model = ryml::parse_json_in_arena(ryml::csubstr(cm_text.data(), cm_text.size()));
        fs::path top_build = build_dir;
        if (auto b = child_str(child(model.crootref(), "paths"), "build"); !b.empty()) top_build = utf8_path(b);
        auto configs = child(model.crootref(), "configurations");
        if (configs.invalid() || !configs.is_seq() || configs.num_children() == 0) {
            cm.error = loc("codemodel is empty", "codemodel boş");
            return cm;
        }
        auto config = configs.first_child();  // single-config generators (Ninja) have exactly one
        cm.configuration = child_str(config, "name");
        if (auto targets = child(config, "targets"); !targets.invalid() && targets.is_seq()) {
            for (auto t : targets.children()) {
                CMakeTarget target;
                target.name = child_str(t, "name");
                const std::string json = child_str(t, "jsonFile");
                if (json.empty()) continue;
                const std::string t_text = read_file(reply / utf8_path(json));
                if (t_text.empty()) continue;
                ryml::Tree tt = ryml::parse_json_in_arena(ryml::csubstr(t_text.data(), t_text.size()));
                target.type = child_str(tt.crootref(), "type");
                if (auto arts = child(tt.crootref(), "artifacts"); !arts.invalid() && arts.is_seq()) {
                    for (auto a : arts.children()) {
                        fs::path p = utf8_path(child_str(a, "path"));
                        if (p.empty()) continue;
                        if (p.is_relative()) p = top_build / p;
                        target.artifacts.push_back(p.lexically_normal().make_preferred());
                    }
                }
                cm.targets.push_back(std::move(target));
            }
        }
        cm.valid = true;
    } catch (const RymlError& e) {
        cm.error = std::string(loc("cannot read File API JSON: ", "File API JSON okunamadı: ")) + e.message;
    }
    return cm;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Build log

namespace {

bool parse_issue(std::string_view line, BuildIssue* out) {
    struct Marker {
        std::string_view text;
        bool error;
    };
    static constexpr Marker kMarkers[] = {{": fatal error ", true}, {": error ", true}, {": warning ", false}};
    for (const auto& m : kMarkers) {
        const size_t pos = line.find(m.text);
        if (pos == std::string_view::npos) continue;
        BuildIssue issue;
        issue.error = m.error;
        std::string_view prefix = trim(line.substr(0, pos));
        // Drop MSBuild style "12>" prefixes.
        if (const size_t gt = prefix.find('>'); gt != std::string_view::npos && gt < 5) {
            bool digits = gt > 0;
            for (size_t i = 0; i < gt; ++i) digits &= prefix[i] >= '0' && prefix[i] <= '9';
            if (digits) prefix.remove_prefix(gt + 1);
        }
        if (!prefix.empty() && prefix.back() == ')') {
            const size_t open = prefix.rfind('(');
            if (open != std::string_view::npos) {
                int n = 0;
                for (size_t i = open + 1; i < prefix.size() && prefix[i] >= '0' && prefix[i] <= '9'; ++i) n = n * 10 + (prefix[i] - '0');
                issue.line = n;
                prefix = prefix.substr(0, open);
            }
        }
        issue.file = std::string(trim(prefix));
        std::string_view rest = line.substr(pos + m.text.size());
        const size_t colon = rest.find(':');
        if (colon != std::string_view::npos && colon < 12) {
            issue.code = std::string(trim(rest.substr(0, colon)));
            rest.remove_prefix(colon + 1);
        }
        issue.message = std::string(trim(rest));
        *out = std::move(issue);
        return true;
    }
    return false;
}

}  // namespace

BuildSummary summarize_build_log(std::string_view log) {
    BuildSummary s;
    std::set<std::string> seen;
    std::vector<BuildIssue> warnings;
    size_t start = 0;
    while (start <= log.size()) {
        size_t end = log.find('\n', start);
        if (end == std::string_view::npos) end = log.size();
        std::string_view line = log.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        start = end + 1;
        if (line.empty()) {
            if (end == log.size()) break;
            continue;
        }
        if (trim(line).substr(0, 8) == "FAILED: ") s.ninja_failed = true;
        if (line.find("Build All succeeded") != std::string_view::npos || line.find("Rebuild All succeeded") != std::string_view::npos)
            s.reported_success = true;
        if (line.find("Build All failed") != std::string_view::npos || line.find("Rebuild All failed") != std::string_view::npos)
            s.reported_failure = true;
        BuildIssue issue;
        if (!parse_issue(line, &issue)) continue;
        if (!seen.insert(std::string(line)).second) continue;  // Ninja and the compiler may echo the same line
        if (issue.error) {
            ++s.errors;
            if (s.issues.size() < 50) s.issues.push_back(std::move(issue));
        } else {
            ++s.warnings;
            if (warnings.size() < 50) warnings.push_back(std::move(issue));
        }
        if (end == log.size()) break;
    }
    for (auto& w : warnings) s.issues.push_back(std::move(w));
    return s;
}

}  // namespace kamil
