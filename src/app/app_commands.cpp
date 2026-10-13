// Custom commands from settings.yaml (`commands:`): search items, global hotkeys, placeholders,
// and actions on the CMake project used last.

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>
#include <thread>

#include "app/action_item.h"
#include "app/app.h"
#include "core/hotkey.h"
#include "core/i18n.h"
#include "core/log.h"
#include "core/settings_schema.h"
#include "core/text.h"
#include "platform/process.h"

namespace kamil {

namespace {

std::wstring field_text(const Object& o, const char* name) {
    const Value* v = o.find(name);
    return v && std::holds_alternative<std::string>(v->data) ? widen(v->as_string()) : std::wstring();
}

bool field_bool(const Object& o, const char* name) {
    const Value* v = o.find(name);
    return v && std::holds_alternative<bool>(v->data) && v->as_bool();
}

// First program of a command line, resolved through PATH ("py script.py" -> C:\Windows\py.exe).
std::wstring program_of(const std::wstring& line) {
    std::wstring_view rest = trim(std::wstring_view(line));
    std::wstring first;
    if (!rest.empty() && rest[0] == L'"') {
        const size_t close = rest.find(L'"', 1);
        first = std::wstring(rest.substr(1, close == std::wstring_view::npos ? std::wstring_view::npos : close - 1));
    } else {
        first = std::wstring(rest.substr(0, rest.find(L' ')));
    }
    if (first.empty() || first.find(L'{') != std::wstring::npos) return {};
    wchar_t buf[MAX_PATH];
    const DWORD n = SearchPathW(nullptr, first.c_str(), L".exe", MAX_PATH, buf, nullptr);
    return n && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

std::wstring action_description(const std::wstring& action) {
    if (action == L"jobs") return loc(L"Show Visual Studio jobs", L"Visual Studio işlerini göster");
    if (action == L"cancel-jobs") return loc(L"Cancel all Visual Studio jobs", L"Tüm Visual Studio işlerini iptal et");
    const std::wstring verb = action == L"configure"     ? L"Configure"
                              : action == L"reconfigure" ? L"Reconfigure"
                              : action == L"rebuild"     ? L"Rebuild"
                              : action == L"debug"       ? L"Debug"
                              : action == L"run"         ? L"Run"
                                                         : L"Build";
    return fmt(loc(L"{}: selected project (from the search: the one used last)", L"{}: seçili proje (aramadan: son kullanılan)"), verb);
}

std::string now_text(const char* format) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), format, &tm);
    return buf;
}

}  // namespace

void App::load_user_commands(const Settings& s) {
    user_commands_.clear();
    const auto main = parse_hotkey(hotkey_text_);
    for (const auto& o : s.get_objects(keys::kCommands)) {
        UserCommand c;
        c.name = field_text(o, "name");
        c.run = field_text(o, "run");
        c.action = field_text(o, "action");
        c.keyword = field_text(o, "keyword");
        c.hotkey = field_text(o, "hotkey");
        c.dir = field_text(o, "dir");
        c.console = field_text(o, "console");
        c.admin = field_bool(o, "admin");
        c.confirm = field_bool(o, "confirm");
        if (c.action == L"none") c.action.clear();
        if (c.name.empty() || (c.run.empty() && c.action.empty())) continue;
        if (!c.hotkey.empty()) {
            c.key = parse_hotkey(narrow(c.hotkey));
            if (c.key && main && *c.key == *main) {  // the open/close hotkey always wins
                log_line(L"[commands] " + c.name + L": hotkey is the same as general.hotkey, ignored");
                c.key.reset();
            }
        }
        user_commands_.push_back(std::move(c));
    }
}

bool App::run_command_hotkey(uint32_t mods, uint32_t vk, const Item* selected) {
    for (size_t i = 0; i < user_commands_.size(); ++i) {
        const UserCommand& c = user_commands_[i];
        if (!c.key || c.key->mods != mods || c.key->vk != vk) continue;
        const bool project_action = !c.action.empty() && c.action != L"jobs" && c.action != L"cancel-jobs";
        if (project_action) {
            // Build / configure / ... only on the project selected in the launcher.
            if (!selected || selected->kind != ItemKind::Repo || !is_project(selected->path)) {
                notify(c.name, loc(L"Select a CMake project in the list first.", L"Önce listede bir CMake projesi seçin."), Tray::Balloon::Info);
                return true;
            }
            const std::wstring project = selected->path;
            run_user_command(i, nullptr, &project);
            return true;
        }
        const std::wstring project = selected && selected->kind == ItemKind::Repo && is_project(selected->path) ? selected->path : std::wstring();
        run_user_command(i, nullptr, project.empty() ? nullptr : &project);
        return true;
    }
    return false;
}

void App::add_user_command_items(std::vector<Item>& items) const {
    for (size_t i = 0; i < user_commands_.size(); ++i) {
        const UserCommand& c = user_commands_[i];
        Item it;
        it.kind = ItemKind::Command;
        it.key = L"cmd:" + c.name;
        it.target = L"user:" + std::to_wstring(i);
        it.title = c.name;
        const std::wstring what = c.run.empty() ? action_description(c.action) : c.run;
        it.subtitle = c.hotkey.empty() ? what : c.hotkey + L"     " + what;
        if (!c.run.empty()) it.icon_source = program_of(c.run);
        else if (c.action != L"jobs" && c.action != L"cancel-jobs") it.icon_source = tools_.devenv(vs_choice());
        it.prepare();
        items.push_back(std::move(it));
    }
}

void App::set_last_project(const std::wstring& root) {
    if (root == last_project_) return;
    last_project_ = root;
    std::ofstream out(paths_.config_dir / "last-project.txt", std::ios::binary | std::ios::trunc);
    out << narrow(root);
}

void App::load_last_project() {
    std::ifstream in(paths_.config_dir / "last-project.txt", std::ios::binary);
    std::string line;
    std::getline(in, line);
    last_project_ = widen(trim(std::string_view(line)));
}

std::map<std::string, std::string> App::placeholder_values(const std::wstring& input, const std::wstring& project) {
    std::map<std::string, std::string> v{{"input", narrow(input)},
                                         {"clip", narrow(read_clipboard_text(hwnd_))},
                                         {"date", now_text("%Y-%m-%d")},
                                         {"time", now_text("%H-%M-%S")}};
    if (!project.empty() && is_project(project)) {
        const ProjectView& p = project_view(project);
        v["project"] = narrow(p.name);
        v["project_dir"] = narrow(p.root);
        v["preset"] = narrow(p.preset_name());
        v["target"] = narrow(p.target_name());
        v["config"] = p.model.configuration;
        v["exe"] = narrow(p.exe.wstring());
        v["exe_dir"] = narrow(p.exe.parent_path().wstring());
        v["com"] = narrow(p.com);
        v["args"] = narrow(p.args);
    }
    return v;
}

void App::run_user_command(size_t index, const std::wstring* input, const std::wstring* project_override) {
    if (index >= user_commands_.size()) return;
    const UserCommand c = user_commands_[index];  // copy: the prompt callback may outlive a settings reload
    log_line(L"[commands] " + c.name);

    if (c.action == L"jobs") {
        show_jobs();
        return;
    }
    // {input}: ask in the launcher first, then come back here with the text.
    if (!input && (c.run.find(L"{input}") != std::wstring::npos || c.dir.find(L"{input}") != std::wstring::npos)) {
        Item parent;
        parent.kind = ItemKind::Command;
        parent.key = L"cmd:" + c.name;
        parent.target = L"user:" + std::to_wstring(index);
        parent.title = c.name;
        if (!launcher_.visible()) launcher_.show();
        launcher_.prompt(parent, c.name, {}, loc(L"Value for {input}; Enter runs the command", L"{input} değeri; Enter komutu çalıştırır"),
                         [this, index, project = project_override ? *project_override : std::wstring()](const std::wstring& text) {
                             run_user_command(index, &text, project.empty() ? nullptr : &project);
                         });
        return;
    }
    launcher_.hide();
    if (c.confirm && MessageBoxW(hwnd_, fmt(loc(L"Run \"{}\"?", L"\"{}\" çalıştırılsın mı?"), c.name).c_str(), L"Kamil",
                                 MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;

    if (c.action == L"cancel-jobs") {
        run_job_action(Item{}, L"cancel-all-jobs");
        return;
    }
    if (!c.action.empty()) {
        const std::wstring root = project_override ? *project_override : last_project_;
        if (root.empty() || !is_project(root)) {
            notify(fmt(loc(L"{}: no project yet", L"{}: henüz proje yok"), c.name),
                   loc(L"Build, debug or configure a CMake project once from the launcher; this command then uses that project.",
                       L"Başlatıcıdan bir CMake projesini bir kez derleyin, debug edin ya da configure edin; bu komut sonra o projeyi kullanır."),
                   Tray::Balloon::Info);
            return;
        }
        Item item;
        item.kind = ItemKind::Repo;
        item.key = L"repo:" + root;
        item.title = std::filesystem::path(root).filename().wstring();
        item.path = root;
        item.target = root;
        run_project_action(item, L"proj-" + c.action);
        return;
    }

    const std::wstring project = project_override ? *project_override : last_project_;
    const auto values = placeholder_values(input ? *input : std::wstring(), project);
    const std::wstring line = widen(expand_placeholders(narrow(c.run), values));
    std::wstring dir = expand_env(widen(expand_placeholders(narrow(c.dir), values)));
    if (dir.empty()) dir = !project.empty() && is_project(project) ? project : expand_env(L"%USERPROFILE%");
    const std::wstring cmd = tools_.cmd.empty() ? std::wstring(L"cmd.exe") : tools_.cmd;

    if (c.console == L"hidden" && !c.admin) {
        // No window: the output goes into a console tab (not shown) and the result is a notification.
        const std::wstring typed = input ? *input : std::wstring();
        start_in_console(c.name, line, dir, cmd, L"/S /C \"" + line + L"\"", dir,
                         [this, index, typed] { run_user_command(index, &typed); }, c.name);
        return;
    }
    const std::wstring args = std::wstring(c.console == L"close" || c.console == L"hidden" ? L"/S /C \"" : L"/S /K \"") + line + L"\"";
    run_detached(cmd, args, dir, c.admin, c.name);
}

}  // namespace kamil
