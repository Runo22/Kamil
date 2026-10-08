// CMake projects: presets, targets, run arguments, COM ports, and build / debug through Visual
// Studio. Kamil never builds by itself: build-like actions are sent to VS over COM (VsBridge).

#include <algorithm>
#include <chrono>
#include <cstdlib>

#include "app/action_item.h"
#include "app/app.h"
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
    if (minutes < 1) return L"az önce derlendi";
    if (minutes < 60) return std::to_wstring(minutes) + L" dk önce derlendi";
    if (minutes < 24 * 60) return std::to_wstring(minutes / 60) + L" sa önce derlendi";
    return std::to_wstring(minutes / (24 * 60)) + L" gün önce derlendi";
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
        v.problem = presets.errors.empty() ? L"CMakePresets.json bulunamadı ve proje henüz VS'te configure edilmemiş"
                                           : L"CMakePresets.json okunamadı: " + widen(presets.errors.front());
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
        v.problem = v.model.valid ? L"Projede çalıştırılabilir hedef yok"
                                  : L"Hedef listesi yok: önce VS'te bu preset ile configure edin (" + widen(v.model.error) + L")";
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
        v.problem = ports.empty() ? L"Argümanlar {com} istiyor ama bağlı seri port yok" : L"COM port seçilmedi";
    return projects_[root] = std::move(v);
}

std::wstring App::job_status() const {
    if (job_.running) {
        const ULONGLONG s = (GetTickCount64() - job_.started) / 1000;
        wchar_t clock[16];
        swprintf(clock, 16, L"%llu:%02llu", s / 60, s % 60);
        return L"⚒ " + job_.text + L"  " + clock;
    }
    if (job_.finished && GetTickCount64() - job_.finished < 60'000) {
        const size_t nl = job_.text.find(L'\n');
        return nl == std::wstring::npos ? job_.text : job_.text.substr(0, nl);
    }
    return {};
}

std::wstring App::project_context(const std::wstring& root) {
    const ProjectView& v = project_view(root);
    std::wstring out;
    if (v.active()) out += L"⚙ " + v.preset_name();
    if (!v.executables.empty()) out += L" · " + v.target_name();
    if (!v.com.empty()) out += L"   ⇄ " + v.com;
    return out;
}

// ---------------------------------------------------------------------------------------------
// Actions

std::vector<Item> App::project_actions(const Item& repo) {
    std::vector<Item> list;
    const ProjectView& v = project_view(repo.path, true);
    const std::wstring vs_label = vs_choice() == "vs2022" ? L"VS 2022" : L"VS 2026";
    const std::wstring& devenv = tools_.devenv(vs_choice());
    const std::wstring preset = v.preset_name();
    const std::wstring target = v.target_name();
    const std::wstring problem = v.problem.empty() ? std::wstring() : L"⚠ " + v.problem;

    list.push_back(make_action(L"proj-debug", L"Debug  " + target + (preset.empty() ? L"" : L" · " + preset), devenv, kGlyphPlay,
                               !problem.empty() ? problem
                                                : (store_.current()->get_bool(keys::kBuildBeforeDebug) ? vs_label + L"'da derle → başlat → bağlan"
                                                                                                       : vs_label + L" ile başlat → bağlan") +
                                                      (v.args.empty() ? L"" : L"   " + v.args)));
    list.push_back(make_action(L"proj-build", L"Build  " + preset, devenv, kGlyphPlay, vs_label + L" · Build All"));
    {
        Item run = make_action(L"proj-run", L"Çalıştır  " + target + (preset.empty() ? L"" : L" · " + preset),
                               file_exists(v.exe) ? v.exe.wstring() : std::wstring(), kGlyphPlay,
                               !problem.empty() ? problem : (file_exists(v.exe) ? ago(v.exe) : L"derlenmemiş") + (v.args.empty() ? L"" : L"   " + v.args));
        list.push_back(std::move(run));
    }
    list.push_back(make_action(L"pick-preset", L"Preset: " + (preset.empty() ? L"—" : preset), {}, kGlyphPreset,
                               std::to_wstring(v.presets.size()) + L" preset" + (v.preset_guessed ? L" · VS'te son configure edilen" : L" · seçili")));
    if (v.executables.size() > 1)
        list.push_back(make_action(L"pick-target", L"Hedef: " + target, {}, kGlyphTarget, std::to_wstring(v.executables.size()) + L" çalıştırılabilir hedef"));
    list.push_back(make_action(L"pick-com", L"COM port: " + (v.com.empty() ? L"—" : v.com), {}, kGlyphSerial,
                               v.com.empty() ? std::to_wstring(com_ports().size()) + L" port bağlı" : v.com_label));
    list.push_back(make_action(L"edit-args", L"Argümanlar: " + (v.args_template.empty() ? L"—" : widen(v.args_template)), {}, kGlyphEdit,
                               v.args.empty() ? L"{com}, {preset}, {target}, {config} kullanılabilir" : L"→ " + v.args));

    // The same target as built by the other presets: run them side by side.
    for (size_t i = 0; i < v.presets.size(); ++i) {
        if (i == v.preset || target.empty()) continue;
        const CodeModel other = read_codemodel(v.presets[i].binary_dir);
        for (const auto& t : other.targets) {
            if (t.type != "EXECUTABLE" || widen(t.name) != target) continue;
            const auto exe = first_exe(t);
            if (!file_exists(exe)) continue;
            list.push_back(make_action(L"proj-run:" + widen(v.presets[i].name), L"Çalıştır  " + target + L" · " + widen(v.presets[i].name),
                                       exe.wstring(), kGlyphPlay, ago(exe)));
        }
    }
    list.push_back(make_action(L"proj-rebuild", L"Rebuild  " + preset, devenv, kGlyphRefresh, vs_label + L" · Rebuild All"));
    list.push_back(make_action(L"proj-configure", L"CMake configure", {}, kGlyphRefresh, vs_label + L" · Configure/Generate Cache"));
    list.push_back(make_action(L"proj-reconfigure", L"Önbelleği sil ve yeniden yapılandır", {}, kGlyphRefresh, vs_label + L" · Delete Cache and Reconfigure"));
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
        case L'D': return run_project_action(item, L"proj-debug");
        case L'B': return run_project_action(item, L"proj-build");
        case L'R': return run_project_action(item, L"proj-run");
        default: return false;
    }
}

bool App::run_project_action(const Item& repo, const std::wstring& action) {
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
            notify(L"Debug başlatılamadı", v.problem, Tray::Balloon::Warning);
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
        launcher_.show_list(repo, std::move(items), L"Preset seç (Tab tamamlar) — " + repo.title);
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
        launcher_.show_list(repo, std::move(items), L"Hedef seç — " + repo.title);
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
        items.push_back(make_action(L"set-com:|", L"COM port kullanma", {}, kGlyphSerial, L"Argümanlarda {com} boş kalır"));
        launcher_.show_list(repo, std::move(items), L"COM port seç — " + repo.title);
        return true;
    }
    if (action == L"edit-args") {
        const ProjectView& v = project_view(repo.path, true);
        const std::wstring root = repo.path;
        const Item parent = repo;
        launcher_.prompt(repo, L"Program argümanları", widen(v.args_template),
                         L"{com} {preset} {target} {config} {project} yer tutucuları; boş bırakılırsa dev.default_args",
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
            notify(L"Preset değişti: " + widen(choice.preset),
                   L"VS'te son configure edilen preset " + widen(v.presets[*guess].name) +
                       L". VS'in Build'i kendi seçili preset'ini derler; VS'teki preset seçimini de değiştirin.",
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
        notify(L"Çalıştırılamadı", v.problem, Tray::Balloon::Warning);
        return;
    }
    if (!file_exists(exe)) {
        notify(L"Çalıştırılamadı", L"Henüz derlenmemiş: " + exe.wstring() + L"\nAlt+B ile VS'te derleyin.", Tray::Balloon::Warning);
        return;
    }
    const std::wstring title = L"Kamil ▸ " + v.target_name() + L" [" + preset + L"]" + (v.com.empty() ? L"" : L" " + v.com);
    std::wstring error;
    auto proc = launch_process(exe.wstring(), v.args, exe.parent_path().wstring(), title, false, &error);
    if (!proc) {
        notify(v.target_name() + L" başlatılamadı", error, Tray::Balloon::Error);
        return;
    }
    CloseHandle(static_cast<HANDLE>(proc->thread));
    CloseHandle(static_cast<HANDLE>(proc->process));
}

void App::submit_vs(VsJob::Kind kind, const Item& repo) {
    const ProjectView& v = project_view(repo.path, true);
    const auto s = store_.current();
    VsJob job;
    job.kind = kind;
    job.folder = repo.path;
    job.devenv = tools_.devenv(vs_choice());
    job.dte_version = job.devenv == tools_.devenv_2022 && !job.devenv.empty() ? L"17.0" : L"18.0";
    job.label = v.preset_name();
    job.build_first = s->get_bool(keys::kBuildBeforeDebug);
    job.exe = v.exe.wstring();
    job.args = v.args;
    job.cwd = v.exe.parent_path().wstring();
    job.console_title = L"Kamil ▸ " + v.target_name() + L" [" + v.preset_name() + L"]" + (v.com.empty() ? L"" : L" " + v.com);
    job.configure_command = widen(s->get_string(keys::kVsConfigureCommand));
    job.reconfigure_command = widen(s->get_string(keys::kVsReconfigureCommand));
    if (kind != VsJob::Kind::Configure && kind != VsJob::Kind::Reconfigure) {
        if (const auto guess = guess_active_preset(v.presets); guess && *guess != v.preset)
            notify(L"Preset uyuşmazlığı olabilir",
                   L"Seçili: " + v.preset_name() + L", VS'te son configure edilen: " + widen(v.presets[*guess].name) +
                       L". VS kendi seçili preset'ini derler.",
                   Tray::Balloon::Warning);
    }
    const wchar_t* verb = kind == VsJob::Kind::Debug       ? L"Debug"
                          : kind == VsJob::Kind::Build     ? L"Build"
                          : kind == VsJob::Kind::Rebuild   ? L"Rebuild"
                          : kind == VsJob::Kind::Configure ? L"Configure"
                                                           : L"Reconfigure";
    job_ = Job{true, kind, job.label, std::wstring(verb) + L" " + job.label + L" kuyrukta…", GetTickCount64(), 0, false, repo.path};
    vs_->submit(std::move(job));
    tray_.set_tooltip(L"Kamil — " + job_.text);
    SetTimer(hwnd_, 4 /* kTimerJob */, 1000, nullptr);
}

void App::on_vs_event(const VsEvent& e) {
    if (e.type == VsEvent::Type::Progress) {
        job_.text = e.text;
        tray_.set_tooltip(L"Kamil — " + e.text);
        launcher_.refresh_footer();
        return;
    }
    if (e.kind == VsJob::Kind::Diagnose) {
        if (e.ok) {
            const auto file = e.report;
            executor_->post([file] { open_in_editor(file); });
        } else {
            notify(L"VS bağlantı testi", e.text, Tray::Balloon::Error);
        }
        return;
    }
    job_.running = false;
    job_.finished = GetTickCount64();
    job_.ok = e.ok;
    job_.text = e.text;
    KillTimer(hwnd_, 4);
    tray_.set_tooltip(L"Kamil — " + widen(hotkey_text_));
    invalidate_project(job_.root);
    const size_t nl = e.text.find(L'\n');
    notify(nl == std::wstring::npos ? e.text : e.text.substr(0, nl), nl == std::wstring::npos ? std::wstring() : e.text.substr(nl + 1),
           e.ok ? Tray::Balloon::Info : Tray::Balloon::Error);
    launcher_.refresh_footer();
}

}  // namespace kamil
