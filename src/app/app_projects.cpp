// CMake projects: presets, targets, run arguments, COM ports, and build / debug through Visual
// Studio. Kamil never builds by itself: build-like actions are sent to VS over COM (VsBridge).

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cwchar>

#include "app/action_item.h"
#include "app/app.h"
#include "core/i18n.h"
#include "core/log.h"
#include "core/settings_schema.h"
#include "core/text.h"
#include "platform/process.h"

namespace kamil {

namespace {

constexpr ULONGLONG kProjectCacheMs = 4000;

std::optional<std::string> env_lookup(std::string_view name) {
    const std::wstring w = widen(name);
    wchar_t buf[4096];
    const DWORD n = GetEnvironmentVariableW(w.c_str(), buf, 4096);
    if (n == 0 || n >= 4096) return std::nullopt;
    return narrow(std::wstring_view(buf, n));
}

std::wstring ago(const std::filesystem::path& file) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(file, ec);
    if (ec) return {};
    const auto age = std::filesystem::file_time_type::clock::now() - t;
    const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(age).count();
    if (minutes < 1) return loc(L"built just now", L"az önce derlendi");
    if (minutes < 60) return fmt(loc(L"built {} min ago", L"{} dk önce derlendi"), std::to_wstring(minutes));
    if (minutes < 24 * 60) return fmt(loc(L"built {} h ago", L"{} sa önce derlendi"), std::to_wstring(minutes / 60));
    return fmt(loc(L"built {} days ago", L"{} gün önce derlendi"), std::to_wstring(minutes / (24 * 60)));
}

bool file_exists(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

// Folders under out/build act as presets for CMake projects without a CMakePresets.json
// (Visual Studio's CMakeSettings.json default layout: out/build/x64-Debug).
std::vector<ConfigurePreset> build_dirs_as_presets(const std::filesystem::path& root) {
    std::vector<ConfigurePreset> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root / "out" / "build", ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        ConfigurePreset p;
        p.name = narrow(it->path().filename().wstring());
        p.binary_dir = it->path();
        out.push_back(std::move(p));
    }
    return out;
}

std::filesystem::path first_exe(const CMakeTarget& t) {
    for (const auto& a : t.artifacts) {
        std::wstring ext = a.extension().wstring();
        for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
        if (ext == L".exe") return a;
    }
    return t.artifacts.empty() ? std::filesystem::path() : t.artifacts.front();
}

}  // namespace

std::wstring ProjectView::preset_name() const { return active() ? widen(active()->name) : std::wstring(); }
std::wstring ProjectView::target_name() const {
    return target < executables.size() ? widen(executables[target].name) : std::wstring();
}

bool App::is_project(const std::wstring& root) const {
    for (const auto& r : repos_)
        if (r.path == root) return r.cmake;
    return false;
}

std::string App::vs_choice() const { return store_.current()->get_string(keys::kDefaultVs); }

const std::vector<ComPort>& App::com_ports() {
    if (com_ports_stale_) {
        com_ports_ = list_com_ports();
        com_ports_stale_ = false;
    }
    return com_ports_;
}

void App::invalidate_project(const std::wstring& root) {
    if (root.empty()) projects_.clear();
    else projects_.erase(root);
}

const ProjectView& App::project_view(const std::wstring& root, bool fresh) {
    auto it = projects_.find(root);
    if (it != projects_.end() && !fresh && GetTickCount64() - it->second.built_at < kProjectCacheMs) return it->second;

    ProjectView v;
    v.root = root;
    v.name = std::filesystem::path(root).filename().wstring();
    v.choice = project_state_.get(root);
    v.built_at = GetTickCount64();

    PresetsResult presets = load_cmake_presets(root, env_lookup);
    v.presets = std::move(presets.presets);
    if (v.presets.empty()) v.presets = build_dirs_as_presets(root);
    if (v.presets.empty()) {
        v.problem = presets.errors.empty() ? std::wstring(loc(L"No CMakePresets.json, and the project was not configured in VS yet",
                                                             L"CMakePresets.json yok ve proje henüz VS'te configure edilmemiş"))
                                           : fmt(loc(L"CMakePresets.json could not be read: {}", L"CMakePresets.json okunamadı: {}"),
                                                 widen(presets.errors.front()));
        return projects_[root] = std::move(v);
    }

    v.preset_guessed = true;
    if (auto g = guess_active_preset(v.presets)) v.preset = *g;
    for (size_t i = 0; i < v.presets.size(); ++i) {
        if (v.presets[i].name == v.choice.preset) {
            v.preset = i;
            v.preset_guessed = false;
        }
    }

    const auto& bin = v.presets[v.preset].binary_dir;
    write_codemodel_query(bin);  // so the next configure in VS produces the target list for Kamil too
    v.model = read_codemodel(bin);
    for (const auto& t : v.model.targets)
        if (t.type == "EXECUTABLE") v.executables.push_back(t);
    if (v.executables.empty()) {
        v.problem = v.model.valid ? std::wstring(loc(L"The project has no executable target", L"Projede çalıştırılabilir hedef yok"))
                                  : fmt(loc(L"No target list yet: configure this preset in VS first ({})",
                                            L"Hedef listesi yok: önce VS'te bu preset ile configure edin ({})"),
                                        widen(v.model.error));
    } else {
        const std::string wanted = v.choice.target.empty() ? narrow(v.name) : v.choice.target;
        for (size_t i = 0; i < v.executables.size(); ++i)
            if (iequals_ascii(v.executables[i].name, wanted)) v.target = i;
        v.exe = first_exe(v.executables[v.target]);
    }

    // COM port: the remembered port, else the same device under a new number, else the only port.
    const auto& ports = com_ports();
    const std::wstring want_port = widen(v.choice.com_port), want_hwid = widen(v.choice.com_hwid);
    const ComPort* port = nullptr;
    for (const auto& p : ports)
        if (!want_port.empty() && p.port == want_port && (want_hwid.empty() || p.hwid.empty() || p.hwid == want_hwid)) port = &p;
    if (!port && !want_hwid.empty())
        for (const auto& p : ports)
            if (p.hwid == want_hwid) port = &p;
    if (!port && want_port.empty() && ports.size() == 1) port = &ports.front();
    if (port) {
        v.com = port->port;
        v.com_label = port->friendly_name.empty() ? port->port : port->friendly_name;
    }

    v.args_template = v.choice.args.empty() ? store_.current()->get_string(keys::kDefaultArgs) : v.choice.args;
    const std::map<std::string, std::string> values{
        {"com", narrow(v.com)}, {"preset", narrow(v.preset_name())}, {"target", narrow(v.target_name())},
        {"config", v.model.configuration}, {"project", narrow(v.name)}};
    v.args = widen(expand_placeholders(v.args_template, values));
    if (v.problem.empty() && v.args_template.find("{com}") != std::string::npos && v.com.empty())
        v.problem = ports.empty() ? loc(L"The arguments need {com} but no serial port is connected", L"Argümanlar {com} istiyor ama bağlı seri port yok")
                                  : loc(L"No COM port selected", L"COM port seçilmedi");
    return projects_[root] = std::move(v);
}

void App::warm_projects() {
    std::vector<std::wstring> roots;
    for (const auto& r : repos_)
        if (r.cmake) roots.push_back(r.path);
    if (warm_.joinable()) {
        warm_cancel_ = true;
        warm_.join();
    }
    warm_cancel_ = false;
    if (roots.empty()) return;
    warm_ = std::thread([this, roots = std::move(roots)] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const ULONGLONG start = GetTickCount64();
        size_t models = 0;
        for (const auto& root : roots) {
            PresetsResult presets = load_cmake_presets(root, env_lookup);
            if (presets.presets.empty()) presets.presets = build_dirs_as_presets(root);
            for (const auto& p : presets.presets) {
                if (warm_cancel_) return;
                models += read_codemodel(p.binary_dir).valid;  // fills the codemodel cache
            }
        }
        warm_ms_ = GetTickCount64() - start;
        log_line("[projects] warmed " + std::to_string(roots.size()) + " projects, " + std::to_string(models) + " codemodels in " +
                 std::to_string(warm_ms_) + " ms");
    });
}

std::wstring App::job_title(const Job& job) const {
    std::wstring title = vs_job_verb(job.kind);
    if (!job.label.empty()) title += L" " + job.label;
    const std::wstring project = std::filesystem::path(job.root).filename().wstring();
    if (!project.empty()) title += L" — " + project;
    return title;
}

std::wstring App::job_status() const {
    for (const auto& j : jobs_) {
        if (!j.running) continue;
        const ULONGLONG s = (GetTickCount64() - j.started) / 1000;
        wchar_t clock[16];
        swprintf(clock, 16, L"%llu:%02llu", s / 60, s % 60);
        std::wstring out = L"⚒ " + (j.text.empty() ? job_title(j) : j.text) + L"  " + clock;
        if (jobs_.size() > 1) out += fmt(loc(L"   +{} queued", L"   +{} sırada"), std::to_wstring(jobs_.size() - 1));
        return out;
    }
    if (!jobs_.empty()) return fmt(loc(L"⚒ {} queued", L"⚒ {} işi sırada"), std::to_wstring(jobs_.size()));
    if (last_job_.finished && GetTickCount64() - last_job_.finished < 60'000) {
        const size_t nl = last_job_.text.find(L'\n');
        return nl == std::wstring::npos ? last_job_.text : last_job_.text.substr(0, nl);
    }
    return {};
}

std::wstring App::project_context(const std::wstring& root) {
    const ProjectView& v = project_view(root);
    std::wstring out;
    if (v.active()) out += L"⚙ " + v.preset_name();
    if (!v.executables.empty()) out += L"   ◎ " + v.target_name();
    if (!v.com.empty()) out += L"   ⇄ " + v.com;
    return out;
}

// ---------------------------------------------------------------------------------------------
// Job list: visible queue with cancel

std::vector<Item> App::job_actions(const std::wstring& root) const {
    std::vector<Item> items;
    const ULONGLONG now = GetTickCount64();
    for (const auto& j : jobs_) {
        if (!root.empty() && j.root != root) continue;
        std::wstring sub;
        if (j.cancelling) {
            sub = loc(L"Cancelling…", L"İptal ediliyor…");
        } else if (j.running) {
            const ULONGLONG s = (now - j.started) / 1000;
            wchar_t clock[16];
            swprintf(clock, 16, L"%llu:%02llu", s / 60, s % 60);
            sub = fmt(loc(L"Running {}", L"Sürüyor {}"), clock) + (j.text.empty() ? std::wstring() : L"     " + j.text);
        } else {
            sub = loc(L"Queued: starts when the job before it finishes", L"Sırada: önceki iş bitince başlar");
        }
        items.push_back(make_action(L"cancel-job:" + std::to_wstring(j.id), fmt(loc(L"Cancel {}", L"İptal et: {}"), job_title(j)), {},
                                    kGlyphCancel, sub));
    }
    return items;
}

void App::show_jobs() {
    Item parent;
    parent.kind = ItemKind::Command;
    parent.key = L"kamil:jobs";
    parent.target = L"jobs";
    parent.title = loc(L"Kamil: VS jobs", L"Kamil: VS işleri");
    std::vector<Item> items = job_actions({});
    if (items.size() > 1)
        items.push_back(make_action(L"cancel-all-jobs", loc(L"Cancel all", L"Tümünü iptal et"), {}, kGlyphCancel,
                                    fmt(loc(L"{} jobs", L"{} iş"), std::to_wstring(items.size()))));
    if (items.empty()) {
        std::wstring sub = last_job_.text;
        if (const size_t nl = sub.find(L'\n'); nl != std::wstring::npos) sub.resize(nl);
        items.push_back(make_action(L"jobs-none", loc(L"No Visual Studio jobs running or queued", L"Çalışan ya da sırada bekleyen VS işi yok"), {}, kGlyphCheck,
                                    sub.empty() ? std::wstring() : fmt(loc(L"Last: {}", L"Son: {}"), sub)));
    }
    items.push_back(make_action(L"open-log", loc(L"Open the log", L"Günlüğü aç"), {}, kGlyphInfo, log_file().wstring()));
    if (!launcher_.visible()) launcher_.show();
    launcher_.show_list(parent, std::move(items), loc(L"VS jobs: Enter cancels the selected job", L"VS işleri: Enter seçili işi iptal eder"));
}

void App::cancel_job(uint64_t id) {
    for (auto& j : jobs_) {
        if (j.id != id) continue;
        if (vs_->cancel(id) && j.running) j.cancelling = true;
    }
    launcher_.refresh_footer();
}

bool App::run_job_action(const Item& parent, const std::wstring& action) {
    if (action.rfind(L"cancel-job:", 0) == 0) {
        cancel_job(std::wcstoull(action.c_str() + 11, nullptr, 10));
    } else if (action == L"cancel-all-jobs") {
        std::vector<uint64_t> ids;
        for (const auto& j : jobs_) ids.push_back(j.id);
        for (auto it = ids.rbegin(); it != ids.rend(); ++it) cancel_job(*it);  // queued ones first, so none starts meanwhile
    } else if (action == L"jobs-none") {
        launcher_.hide();
        return true;
    } else if (action == L"open-log") {
        launcher_.hide();
        const auto file = log_file();
        executor_->post([file] { open_in_editor(file); });
        return true;
    } else {
        return false;
    }
    if (parent.target == L"jobs") show_jobs();
    else if (!parent.key.empty() && launcher_.visible()) launcher_.reopen_actions(parent);
    return true;
}

void App::update_job_timer() {
    if (!jobs_.empty()) SetTimer(hwnd_, 4 /* job clock */, 1000, nullptr);
    else KillTimer(hwnd_, 4);
    std::wstring tip = L"Kamil — ";
    const std::wstring status = job_status();
    tray_.set_tooltip(tip + (jobs_.empty() || status.empty() ? widen(hotkey_text_) : status.substr(0, 100)));
}

// ---------------------------------------------------------------------------------------------
// Actions

std::vector<Item> App::project_actions(const Item& repo) {
    std::vector<Item> list = job_actions(repo.path);  // running / queued jobs of this project first: cancel
    const ProjectView& v = project_view(repo.path, true);
    const std::wstring vs_label = vs_choice() == "vs2022" ? L"VS 2022" : L"VS 2026";
    const std::wstring& devenv = tools_.devenv(vs_choice());
    const std::wstring preset = v.preset_name();
    const std::wstring target = v.target_name();
    const std::wstring problem = v.problem.empty() ? std::wstring() : L"⚠ " + v.problem;
    // Subtitles: "<preset>     <what happens>     <arguments>", separated by space, not by dots.
    auto sub = [&](std::initializer_list<std::wstring> parts) {
        std::wstring out;
        for (const auto& p : parts) {
            if (p.empty()) continue;
            if (!out.empty()) out += L"     ";
            out += p;
        }
        return out;
    };

    list.push_back(make_action(L"proj-configure", L"Configure", {}, kGlyphRefresh,
                               sub({preset, fmt(loc(L"CMake configure in {}", L"{}'da CMake configure"), vs_label)})));
    list.push_back(make_action(L"proj-build", L"Build", devenv, kGlyphPlay, sub({preset, vs_label + L" Build All"})));
    list.push_back(make_action(L"proj-debug", L"Debug " + target, devenv, kGlyphPlay,
                               !problem.empty() ? problem
                                                : sub({preset,
                                                       store_.current()->get_bool(keys::kBuildBeforeDebug)
                                                           ? fmt(loc(L"build in {} → start → attach", L"{}'da derle → başlat → bağlan"), vs_label)
                                                           : fmt(loc(L"start → attach {}", L"başlat → {} bağlan"), vs_label),
                                                       v.args})));
    list.push_back(make_action(L"proj-run", fmt(loc(L"Run {}", L"Çalıştır {}"), target), file_exists(v.exe) ? v.exe.wstring() : std::wstring(),
                               kGlyphPlay,
                               !problem.empty() ? problem
                                                : sub({preset, file_exists(v.exe) ? ago(v.exe) : std::wstring(loc(L"not built", L"derlenmemiş")), v.args})));
    list.push_back(make_action(L"pick-preset", L"Preset: " + (preset.empty() ? L"—" : preset), {}, kGlyphPreset,
                               fmt(v.preset_guessed ? loc(L"{} presets, the one configured last in VS", L"{} preset, VS'te son configure edilen")
                                                    : loc(L"{} presets, chosen in Kamil", L"{} preset, Kamil'de seçilen"),
                                   std::to_wstring(v.presets.size()))));
    if (v.executables.size() > 1)
        list.push_back(make_action(L"pick-target", fmt(loc(L"Target: {}", L"Hedef: {}"), target), {}, kGlyphTarget,
                                   fmt(loc(L"{} executable targets", L"{} çalıştırılabilir hedef"), std::to_wstring(v.executables.size()))));
    list.push_back(make_action(L"pick-com", L"COM port: " + (v.com.empty() ? L"—" : v.com), {}, kGlyphSerial,
                               v.com.empty() ? fmt(loc(L"{} ports connected", L"{} port bağlı"), std::to_wstring(com_ports().size())) : v.com_label));
    list.push_back(make_action(L"edit-args", fmt(loc(L"Arguments: {}", L"Argümanlar: {}"), v.args_template.empty() ? std::wstring(L"—") : widen(v.args_template)),
                               {}, kGlyphEdit,
                               v.args.empty() ? std::wstring(loc(L"{com}, {preset}, {target}, {config} can be used", L"{com}, {preset}, {target}, {config} kullanılabilir"))
                                              : L"→ " + v.args));

    // The same target as built by the other presets: run them side by side.
    for (size_t i = 0; i < v.presets.size(); ++i) {
        if (i == v.preset || target.empty()) continue;
        const CodeModel other = read_codemodel(v.presets[i].binary_dir);
        for (const auto& t : other.targets) {
            if (t.type != "EXECUTABLE" || widen(t.name) != target) continue;
            const auto exe = first_exe(t);
            if (!file_exists(exe)) continue;
            list.push_back(make_action(L"proj-run:" + widen(v.presets[i].name), fmt(loc(L"Run {}", L"Çalıştır {}"), target), exe.wstring(), kGlyphPlay,
                                       sub({widen(v.presets[i].name), ago(exe)})));
        }
    }
    list.push_back(make_action(L"proj-rebuild", L"Rebuild", devenv, kGlyphRefresh, sub({preset, vs_label + L" Rebuild All"})));
    list.push_back(make_action(L"proj-reconfigure", loc(L"Delete cache and reconfigure", L"Önbelleği sil ve yeniden yapılandır"), {}, kGlyphRefresh,
                               sub({preset, vs_label + L" Delete Cache and Reconfigure"})));
    return list;
}

bool App::quick_action(const Item& item, wchar_t key) {
    if (item.kind == ItemKind::File || item.kind == ItemKind::Folder) {
        if (key == L'E') {
            edit_file(item);
            return true;
        }
        if (key == L'R' && item.kind == ItemKind::File) {
            run_script(item, false);
            return true;
        }
        return false;
    }
    if (item.kind != ItemKind::Repo || !is_project(item.path)) return false;
    switch (key) {
        case L'C': return run_project_action(item, L"proj-configure");
        case L'D': return run_project_action(item, L"proj-debug");
        case L'B': return run_project_action(item, L"proj-build");
        case L'R': return run_project_action(item, L"proj-run");
        default: return false;
    }
}

bool App::run_project_action(const Item& repo, const std::wstring& action) {
    set_last_project(repo.path);
    auto choice = project_state_.get(repo.path);
    auto save_choice = [&] {
        project_state_.set(repo.path, choice);
        project_state_.save(paths_.projects_file());
        invalidate_project(repo.path);
    };

    if (action == L"proj-debug" || action == L"proj-build" || action == L"proj-rebuild" || action == L"proj-configure" ||
        action == L"proj-reconfigure") {
        const ProjectView& v = project_view(repo.path, true);
        if (action == L"proj-debug" && !v.problem.empty()) {
            notify(loc(L"Debug could not start", L"Debug başlatılamadı"), v.problem, Tray::Balloon::Warning);
            if (v.problem.find(L"COM") != std::wstring::npos) run_project_action(repo, L"pick-com");
            return true;
        }
        const VsJob::Kind kind = action == L"proj-debug"       ? VsJob::Kind::Debug
                                 : action == L"proj-build"     ? VsJob::Kind::Build
                                 : action == L"proj-rebuild"   ? VsJob::Kind::Rebuild
                                 : action == L"proj-configure" ? VsJob::Kind::Configure
                                                               : VsJob::Kind::Reconfigure;
        launcher_.hide();
        submit_vs(kind, repo);
        return true;
    }
    if (action == L"proj-run") {
        launcher_.hide();
        run_project_exe(repo, {});
        return true;
    }
    if (action.rfind(L"proj-run:", 0) == 0) {
        launcher_.hide();
        run_project_exe(repo, action.substr(9));
        return true;
    }

    if (action == L"pick-preset") {
        const ProjectView& v = project_view(repo.path, true);
        std::vector<Item> items;
        for (size_t i = 0; i < v.presets.size(); ++i) {
            const auto& p = v.presets[i];
            std::wstring sub = p.binary_dir.wstring();
            if (!p.display_name.empty()) sub = widen(p.display_name) + L"   " + sub;
            Item it = make_action(L"set-preset:" + widen(p.name), widen(p.name), {}, i == v.preset ? kGlyphCheck : kGlyphPreset, sub);
            it.completion = widen(p.name);
            items.push_back(std::move(it));
        }
        launcher_.show_list(repo, std::move(items), fmt(loc(L"Pick a preset (Tab completes) — {}", L"Preset seç (Tab tamamlar) — {}"), repo.title));
        return true;
    }
    if (action == L"pick-target") {
        const ProjectView& v = project_view(repo.path, true);
        std::vector<Item> items;
        for (size_t i = 0; i < v.executables.size(); ++i) {
            const auto exe = first_exe(v.executables[i]);
            Item it = make_action(L"set-target:" + widen(v.executables[i].name), widen(v.executables[i].name),
                                  file_exists(exe) ? exe.wstring() : std::wstring(), i == v.target ? kGlyphCheck : kGlyphTarget, exe.wstring());
            it.completion = it.title;
            items.push_back(std::move(it));
        }
        launcher_.show_list(repo, std::move(items), fmt(loc(L"Pick a target — {}", L"Hedef seç — {}"), repo.title));
        return true;
    }
    if (action == L"pick-com") {
        com_ports_stale_ = true;
        const ProjectView& v = project_view(repo.path, true);
        std::vector<Item> items;
        for (const auto& p : com_ports()) {
            std::wstring sub = p.friendly_name;
            if (!p.manufacturer.empty()) sub += L" — " + p.manufacturer;
            if (!p.hwid.empty()) sub += L"   " + p.hwid;
            Item it = make_action(L"set-com:" + p.port + L"|" + p.hwid, p.port, {}, p.port == v.com ? kGlyphCheck : kGlyphSerial, sub);
            it.completion = p.port;
            items.push_back(std::move(it));
        }
        items.push_back(make_action(L"set-com:|", loc(L"No COM port", L"COM port kullanma"), {}, kGlyphSerial,
                                    loc(L"{com} stays empty in the arguments", L"Argümanlarda {com} boş kalır")));
        launcher_.show_list(repo, std::move(items), fmt(loc(L"Pick a COM port — {}", L"COM port seç — {}"), repo.title));
        return true;
    }
    if (action == L"edit-args") {
        const ProjectView& v = project_view(repo.path, true);
        const std::wstring root = repo.path;
        const Item parent = repo;
        launcher_.prompt(repo, loc(L"Program arguments", L"Program argümanları"), widen(v.args_template),
                         loc(L"Placeholders {com} {preset} {target} {config} {project}; empty uses dev.default_args",
                             L"{com} {preset} {target} {config} {project} yer tutucuları; boş bırakılırsa dev.default_args"),
                         [this, root, parent](const std::wstring& text) {
                             auto c = project_state_.get(root);
                             c.args = narrow(trim(std::wstring_view(text)));
                             project_state_.set(root, c);
                             project_state_.save(paths_.projects_file());
                             invalidate_project(root);
                             launcher_.reopen_actions(parent);
                         });
        return true;
    }
    if (action.rfind(L"set-preset:", 0) == 0) {
        choice.preset = narrow(action.substr(11));
        save_choice();
        const ProjectView& v = project_view(repo.path, true);
        if (const auto guess = guess_active_preset(v.presets); guess && v.presets[*guess].name != choice.preset)
            notify(fmt(loc(L"Preset changed: {}", L"Preset değişti: {}"), widen(choice.preset)),
                   fmt(loc(L"The preset configured last in VS is {}. VS builds its own selected preset: change the selection in VS too.",
                           L"VS'te son configure edilen preset {}. VS'in Build'i kendi seçili preset'ini derler; VS'teki seçimi de değiştirin."),
                       widen(v.presets[*guess].name)),
                   Tray::Balloon::Info);
        launcher_.reopen_actions(repo);
        return true;
    }
    if (action.rfind(L"set-target:", 0) == 0) {
        choice.target = narrow(action.substr(11));
        save_choice();
        launcher_.reopen_actions(repo);
        return true;
    }
    if (action.rfind(L"set-com:", 0) == 0) {
        const std::wstring value = action.substr(8);
        const size_t bar = value.find(L'|');
        choice.com_port = narrow(value.substr(0, bar));
        choice.com_hwid = bar == std::wstring::npos ? std::string() : narrow(value.substr(bar + 1));
        save_choice();
        launcher_.reopen_actions(repo);
        return true;
    }
    return false;
}

void App::run_project_exe(const Item& repo, const std::wstring& preset_override) {
    const ProjectView& v = project_view(repo.path, true);
    std::filesystem::path exe = v.exe;
    std::wstring preset = v.preset_name();
    if (!preset_override.empty()) {
        for (const auto& p : v.presets) {
            if (widen(p.name) != preset_override) continue;
            for (const auto& t : read_codemodel(p.binary_dir).targets)
                if (t.type == "EXECUTABLE" && t.name == narrow(v.target_name())) exe = first_exe(t);
            preset = preset_override;
        }
    }
    if (!v.problem.empty() && (v.executables.empty() || v.problem.find(L"COM") != std::wstring::npos)) {
        notify(loc(L"Could not run", L"Çalıştırılamadı"), v.problem, Tray::Balloon::Warning);
        return;
    }
    if (!file_exists(exe)) {
        notify(loc(L"Could not run", L"Çalıştırılamadı"),
               fmt(loc(L"Not built yet: {}\nBuild it in VS with Alt+B.", L"Henüz derlenmemiş: {}\nAlt+B ile VS'te derleyin."), exe.wstring()),
               Tray::Balloon::Warning);
        return;
    }
    if (store_.current()->get_string(keys::kRunIn) == "kamil") {
        const Item repo_copy = repo;
        const std::wstring preset_copy = preset_override;
        start_in_console(v.target_name() + L" [" + preset + L"]" + (v.com.empty() ? L"" : L" " + v.com), exe.wstring() + L"  " + v.args, repo.path,
                         exe.wstring(), v.args, exe.parent_path().wstring(), [this, repo_copy, preset_copy] { run_project_exe(repo_copy, preset_copy); });
        show_console(true);
        return;
    }
    const std::wstring title = L"Kamil ▸ " + v.target_name() + L" [" + preset + L"]" + (v.com.empty() ? L"" : L" " + v.com);
    std::wstring error;
    auto proc = launch_process(exe.wstring(), v.args, exe.parent_path().wstring(), title, false, &error);
    if (!proc) {
        notify(fmt(loc(L"{} could not be started", L"{} başlatılamadı"), v.target_name()), error, Tray::Balloon::Error);
        return;
    }
    CloseHandle(static_cast<HANDLE>(proc->thread));
    CloseHandle(static_cast<HANDLE>(proc->process));
}

void App::submit_vs(VsJob::Kind kind, const Item& repo) {
    for (const auto& j : jobs_) {
        if (j.root == repo.path && j.kind == kind && !j.running) {
            notify(fmt(loc(L"{} is already queued", L"{} zaten sırada"), job_title(j)),
                   loc(L"\"Kamil: VS jobs\" shows the queue and cancels jobs.", L"\"Kamil: VS işleri\" sırayı gösterir ve iş iptal eder."),
                   Tray::Balloon::Info);
            return;
        }
    }
    const ProjectView& v = project_view(repo.path, true);
    const auto s = store_.current();
    VsJob job;
    job.kind = kind;
    job.folder = repo.path;
    job.devenv = tools_.devenv(vs_choice());
    // The wanted version, unless only the other one is installed (Tools::devenv falls back).
    job.dte_version = vs_choice() == "vs2022" ? L"17.0" : L"18.0";
    if (!job.devenv.empty()) job.dte_version = job.devenv == tools_.devenv_2022 ? L"17.0" : L"18.0";
    job.label = v.preset_name();
    job.wait_ms = static_cast<uint32_t>(s->get_int(keys::kVsWaitSeconds) * 1000);
    job.build_first = s->get_bool(keys::kBuildBeforeDebug);
    job.exe = v.exe.wstring();
    job.args = v.args;
    job.cwd = v.exe.parent_path().wstring();
    job.console_title = L"Kamil ▸ " + v.target_name() + L" [" + v.preset_name() + L"]" + (v.com.empty() ? L"" : L" " + v.com);
    job.configure_command = widen(s->get_string(keys::kVsConfigureCommand));
    job.reconfigure_command = widen(s->get_string(keys::kVsReconfigureCommand));
    if (kind != VsJob::Kind::Configure && kind != VsJob::Kind::Reconfigure) {
        if (const auto guess = guess_active_preset(v.presets); guess && *guess != v.preset)
            notify(loc(L"Preset mismatch possible", L"Preset uyuşmazlığı olabilir"),
                   fmt(loc(L"Selected: {}, configured last in VS: {}. VS builds its own selected preset.",
                           L"Seçili: {}, VS'te son configure edilen: {}. VS kendi seçili preset'ini derler."),
                       v.preset_name(), widen(v.presets[*guess].name)),
                   Tray::Balloon::Warning);
    }
    Job entry;
    entry.kind = kind;
    entry.root = repo.path;
    entry.label = job.label;
    entry.queued = GetTickCount64();
    const size_t ahead = jobs_.size();
    entry.id = vs_->submit(std::move(job));
    entry.text = fmt(loc(L"{}: waiting in the queue…", L"{}: sırada bekliyor…"), job_title(entry));
    log_line(L"[app] queued #" + std::to_wstring(entry.id) + L" " + job_title(entry));
    if (ahead > 0)
        notify(fmt(loc(L"{} queued", L"{} sıraya alındı"), job_title(entry)),
               fmt(loc(L"{} job(s) ahead. \"Kamil: VS jobs\" shows the queue and cancels jobs.",
                       L"Önünde {} iş var. \"Kamil: VS işleri\" sırayı gösterir ve iş iptal eder."),
                   std::to_wstring(ahead)),
               Tray::Balloon::Info);
    jobs_.push_back(std::move(entry));
    update_job_timer();
}

void App::on_vs_event(const VsEvent& e) {
    if (e.kind == VsJob::Kind::GoTo) {
        auto it = pending_goto_.find(e.job);
        if (it == pending_goto_.end()) return;
        const auto [file, line] = it->second;
        pending_goto_.erase(it);
        if (!e.ok) open_in_code(file, line);  // no Visual Studio has the project open
        return;
    }
    on_vs_job_console(e);
    if (e.type == VsEvent::Type::Output) return;
    if (e.kind == VsJob::Kind::Diagnose) {
        if (e.type != VsEvent::Type::Finished) return;
        if (e.ok) {
            const auto file = e.report;
            executor_->post([file] { open_in_editor(file); });
        } else {
            notify(loc(L"VS connection test", L"VS bağlantı testi"), e.text, Tray::Balloon::Error);
        }
        return;
    }
    auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& j) { return j.id == e.job; });
    const bool showing_jobs = launcher_.action_parent() && launcher_.action_parent()->target == L"jobs";
    if (e.type == VsEvent::Type::Started) {
        if (it != jobs_.end()) {
            it->running = true;
            it->started = GetTickCount64();
        }
        if (showing_jobs) show_jobs();
        update_job_timer();
        launcher_.refresh_footer();
        return;
    }
    if (e.type == VsEvent::Type::Progress) {
        if (it != jobs_.end()) it->text = e.text;
        update_job_timer();
        launcher_.refresh_footer();
        return;
    }
    const bool was_running = it != jobs_.end() && it->running;
    std::wstring root;
    if (it != jobs_.end()) {
        root = it->root;
        jobs_.erase(it);
    }
    last_job_ = LastJob{e.text, GetTickCount64(), e.ok};
    invalidate_project(root);
    if (e.kind == VsJob::Kind::Configure || e.kind == VsJob::Kind::Reconfigure) warm_projects();
    update_job_timer();
    // A queued job cancelled from the list needs no balloon: the list already shows it.
    if (!e.cancelled || was_running) {
        const size_t nl = e.text.find(L'\n');
        notify(nl == std::wstring::npos ? e.text : e.text.substr(0, nl), nl == std::wstring::npos ? std::wstring() : e.text.substr(nl + 1),
               e.ok || e.cancelled ? Tray::Balloon::Info : Tray::Balloon::Error);
    }
    if (showing_jobs) show_jobs();
    launcher_.refresh_footer();
}

}  // namespace kamil
