// Files and folders under search.folders: background indexing, merging file hits into the
// result list, running scripts (Enter) and editing them (Alt+E).

#include <algorithm>
#include <cwctype>

#include "app/action_item.h"
#include "app/app.h"
#include "core/settings_schema.h"
#include "core/text.h"
#include "platform/process.h"

namespace kamil {

namespace {

constexpr wchar_t kGlyphOpenWith = 0xE7AC;  // "OpenWith"

std::wstring lower_ext(const std::wstring& path) {
    std::wstring ext = std::filesystem::path(path).extension().wstring();
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext;
}

bool own_icon(const std::wstring& ext) {
    // Files whose icon is specific to the file itself, not to its type.
    return ext == L".exe" || ext == L".lnk" || ext == L".ico" || ext == L".url" || ext == L".appref-ms";
}

}  // namespace

std::wstring expand_env(const std::wstring& raw) {
    std::wstring expanded(32768, L'\0');
    const DWORD n = ExpandEnvironmentStringsW(raw.c_str(), expanded.data(), static_cast<DWORD>(expanded.size()));
    if (n == 0 || n > expanded.size()) return raw;
    expanded.resize(n - 1);
    return expanded;
}

// ---------------------------------------------------------------------------------------------
// Indexing

void App::scan_files() {
    const auto s = store_.current();
    std::vector<IndexRoot> roots;
    auto add_root = [&](const std::wstring& raw, int bonus, size_t depth, std::vector<std::wstring> include) {
        IndexRoot r;
        r.path = expand_env(raw);
        r.bonus = bonus;
        r.max_depth = depth;
        r.include = std::move(include);
        const std::wstring norm = normalize_folder(r.path.wstring());
        for (const auto& existing : roots)
            if (normalize_folder(existing.path.wstring()) == norm) return;
        roots.push_back(std::move(r));
    };
    for (const auto& o : s->get_objects(keys::kSearchFolders)) {
        const Value* path = o.find("path");
        if (!path) continue;
        std::vector<std::wstring> include;
        if (const Value* inc = o.find("include"))
            for (const auto& g : inc->as_list()) include.push_back(fold(widen(g)));
        add_root(widen(path->as_string()), static_cast<int>(o.find("priority")->as_int()), static_cast<size_t>(o.find("depth")->as_int()),
                 std::move(include));
    }
    // Folders given only a priority are searched as well: that is what they are for.
    for (const auto& o : s->get_objects(keys::kFolderPriority)) {
        const Value* path = o.find("path");
        const Value* prio = o.find("priority");
        if (path && prio) add_root(widen(path->as_string()), static_cast<int>(prio->as_int()), 6, {});
    }
    IndexOptions opt;
    for (const auto& g : s->get_list(keys::kExcludeDirs)) opt.exclude_dirs.push_back(fold(widen(g)));
    opt.max_entries = static_cast<size_t>(s->get_int(keys::kMaxFiles));

    if (file_scan_.joinable()) {
        file_scan_cancel_ = true;
        file_scan_.join();
    }
    file_scan_cancel_ = false;
    files_scanned_at_ = GetTickCount64();
    const HWND target = hwnd_;
    file_scan_ = std::thread([this, roots = std::move(roots), opt = std::move(opt), target] {
        auto* index = new FileIndex(FileIndex::build(roots, opt, &file_scan_cancel_));
        if (file_scan_cancel_) {
            delete index;
            return;
        }
        post_owned(target, WM_KAMIL_FILES_READY, index);
    });
}

void App::on_files_ready(std::unique_ptr<FileIndex> index) {
    file_index_ = std::move(index);
    // A visible list keeps its selection; the new index is used from the next keystroke on.
    if (!launcher_.visible()) run_query(launcher_.query());
}

// ---------------------------------------------------------------------------------------------
// Search

Item App::make_file_item(const std::wstring& path, bool is_dir) const {
    Item it;
    it.kind = is_dir ? ItemKind::Folder : ItemKind::File;
    it.key = L"file:" + path;
    const std::filesystem::path p(path);
    it.title = p.filename().wstring();
    it.subtitle = p.parent_path().wstring();
    it.target = path;
    it.path = path;
    it.icon_source = path;
    const std::wstring ext = lower_ext(path);
    if (is_dir) it.icon_key = L"icon:folder";
    else if (!own_icon(ext)) it.icon_key = L"icon:ext" + (ext.empty() ? std::wstring(L"-none") : ext);  // one icon per type
    it.prepare();
    return it;
}

void App::add_file_hits(const std::wstring& query, std::vector<Hit>& hits) {
    query_items_.clear();
    const size_t limit = search_options_.limit;
    query_items_.reserve(limit);  // hits point into query_items_: it must never reallocate
    FuzzyMatcher matcher(query);

    if (matcher.empty()) {
        // Frequently used files show up in the empty list just like apps.
        if (!search_options_.frequent_when_empty) return;
        for (const auto& key : usage_.top(limit, search_options_.now_unix)) {
            if (key.rfind(L"file:", 0) != 0 || query_items_.size() >= limit) continue;
            const std::wstring path = key.substr(5);
            std::error_code ec;
            const auto status = std::filesystem::status(path, ec);
            if (ec || !std::filesystem::exists(status)) continue;
            query_items_.push_back(make_file_item(path, std::filesystem::is_directory(status)));
            hits.push_back(Hit{&query_items_.back(), static_cast<int>(usage_.frecency(key, search_options_.now_unix) * 100), {}});
        }
        std::sort(hits.begin(), hits.end(), hit_better);
        if (hits.size() > limit) hits.resize(limit);
        return;
    }
    if (!file_index_) return;

    const std::wstring folded_query = matcher.folded_query();
    for (const auto& m : file_index_->search(matcher, limit)) {
        query_items_.push_back(make_file_item(file_index_->path(m.entry), file_index_->is_dir(m.entry)));
        const Item& item = query_items_.back();
        int score = m.score;
        if (search_options_.learning)
            score += learning_bonus(usage_.frecency(item.key, search_options_.now_unix),
                                    usage_.affinity(folded_query, item.key, search_options_.now_unix));
        Hit hit{&item, score, {}};
        matcher.match(item.title, item.title_folded, &hit.positions);
        hits.push_back(std::move(hit));
    }
    std::sort(hits.begin(), hits.end(), hit_better);
    if (hits.size() > limit) hits.resize(limit);
}

// ---------------------------------------------------------------------------------------------
// Running and editing

void App::run_detached(std::wstring exe, std::wstring args, std::wstring dir, bool admin, std::wstring title, const wchar_t* verb) {
    if (exe.empty()) {
        notify(title + L" çalıştırılamadı", L"Gereken program bulunamadı.", Tray::Balloon::Warning);
        return;
    }
    AllowSetForegroundWindow(ASFW_ANY);
    launcher_.hide();
    const HWND target = hwnd_;
    const std::wstring verb_text = verb ? verb : L"";
    executor_->post([exe, args, dir, admin, title, target, verb_text] {
        const std::wstring err = run_program(exe, args, dir, admin, verb_text.empty() ? nullptr : verb_text.c_str());
        if (!err.empty()) post_owned(target, WM_KAMIL_NOTIFY, new Notification{Notification::Level::Error, title + L" çalıştırılamadı", err});
    });
}

std::wstring App::python_for(bool windowed) const {
    const std::wstring configured = expand_env(widen(store_.current()->get_string(keys::kPython)));
    if (!configured.empty()) {
        if (!windowed) return configured;
        std::filesystem::path w = std::filesystem::path(configured).parent_path() / L"pythonw.exe";
        std::error_code ec;
        return std::filesystem::exists(w, ec) ? w.wstring() : configured;
    }
    if (windowed) return !tools_.pyw.empty() ? tools_.pyw : !tools_.pythonw.empty() ? tools_.pythonw : tools_.py;
    return !tools_.py.empty() ? tools_.py : tools_.python;
}

void App::run_script(const Item& item, bool admin) {
    const std::wstring& path = item.path;
    const std::wstring ext = lower_ext(path);
    const std::wstring dir = std::filesystem::path(path).parent_path().wstring();
    const bool keep = store_.current()->get_bool(keys::kKeepConsole);
    const std::wstring cmd = tools_.cmd.empty() ? L"cmd.exe" : tools_.cmd;
    // cmd /K ""C:\path\tool.exe" "C:\path\script"" keeps the window open after the script ends.
    auto via_cmd = [&](const std::wstring& exe, const std::wstring& args) {
        const std::wstring inner = (exe.empty() ? std::wstring() : quote_arg(exe) + L" ") + args;
        return std::wstring(keep ? L"/K \"" : L"/C \"") + inner + L"\"";
    };

    if (ext == L".bat" || ext == L".cmd") {
        run_detached(cmd, via_cmd({}, quote_arg(path)), dir, admin, item.title);
    } else if (ext == L".ps1") {
        const std::wstring ps = tools_.powershell.empty() ? L"powershell.exe" : tools_.powershell;
        run_detached(ps, std::wstring(L"-NoProfile -ExecutionPolicy Bypass ") + (keep ? L"-NoExit " : L"") + L"-File " + quote_arg(path), dir,
                     admin, item.title);
    } else if (ext == L".py") {
        const std::wstring py = python_for(false);
        if (py.empty()) {
            notify(L"Python bulunamadı", L"py launcher veya python.exe yok. settings.yaml → scripts.python ile yolu verin.", Tray::Balloon::Warning);
            return;
        }
        if (keep) run_detached(cmd, via_cmd(py, quote_arg(path)), dir, admin, item.title);
        else run_detached(py, quote_arg(path), dir, admin, item.title);
    } else if (ext == L".pyw") {
        const std::wstring pyw = python_for(true);
        if (pyw.empty()) {
            notify(L"Python bulunamadı", L"pyw.exe / pythonw.exe yok. settings.yaml → scripts.python ile yolu verin.", Tray::Balloon::Warning);
            return;
        }
        run_detached(pyw, quote_arg(path), dir, admin, item.title);
    } else if (ext == L".sh") {
        if (tools_.bash.empty()) {
            notify(L"bash bulunamadı", L".sh çalıştırmak için Git for Windows gerekli.", Tray::Balloon::Warning);
            return;
        }
        if (keep) run_detached(cmd, via_cmd(tools_.bash, quote_arg(path)), dir, admin, item.title);
        else run_detached(tools_.bash, quote_arg(path), dir, admin, item.title);
    } else {
        // .exe .lnk .vbs .js and anything else: what Explorer would do on double click.
        run_detached(path, {}, dir, admin, item.title);
    }
}

void App::edit_file(const Item& item) {
    const std::string editor = store_.current()->get_string(keys::kScriptEditor);
    const std::wstring notepad = expand_env(L"%WINDIR%\\notepad.exe");
    const bool folder = item.kind == ItemKind::Folder;
    if (editor == "default" && !folder) {
        AllowSetForegroundWindow(ASFW_ANY);
        launcher_.hide();
        const std::wstring path = item.path;
        const HWND target = hwnd_;
        executor_->post([path, notepad, target] {
            if (!run_program(path, {}, {}, false, L"edit").empty()) run_program(notepad, quote_arg(path));
        });
        return;
    }
    std::wstring exe = editor == "notepad" ? notepad : tools_.code;
    if (exe.empty()) exe = folder ? tools_.explorer : notepad;
    run_detached(exe, quote_arg(item.path), std::filesystem::path(item.path).parent_path().wstring(), false, item.title);
}

void App::open_file(const Item& item, LaunchMode mode) {
    if (mode == LaunchMode::OpenLocation || item.kind == ItemKind::Folder) {
        if (item.kind == ItemKind::Folder && mode != LaunchMode::OpenLocation) {
            run_detached(tools_.explorer.empty() ? L"explorer.exe" : tools_.explorer, quote_arg(item.path), {}, false, item.title);
            return;
        }
        AllowSetForegroundWindow(ASFW_ANY);
        launcher_.hide();
        const Item copy = item;
        executor_->post([copy] { launch_item(copy, LaunchMode::OpenLocation); });
        return;
    }
    const std::wstring ext = lower_ext(item.path);
    const bool script = is_script_extension(ext);
    if (script) {
        const bool edit_first = store_.current()->get_string(keys::kScriptAction) == "edit" && ext != L".exe" && ext != L".lnk";
        if (edit_first && mode == LaunchMode::Normal) edit_file(item);
        else run_script(item, mode == LaunchMode::Admin);
        return;
    }
    run_detached(item.path, {}, std::filesystem::path(item.path).parent_path().wstring(), mode == LaunchMode::Admin, item.title);
}

// ---------------------------------------------------------------------------------------------
// Actions

std::vector<Item> App::file_actions(const Item& item) const {
    std::vector<Item> list;
    const std::string editor = store_.current()->get_string(keys::kScriptEditor);
    const std::wstring notepad = expand_env(L"%WINDIR%\\notepad.exe");
    const std::wstring editor_exe = editor == "notepad" ? notepad : (!tools_.code.empty() ? tools_.code : notepad);
    const std::wstring editor_name = editor_exe == notepad ? L"Not Defteri" : L"VS Code";
    const std::wstring dir = std::filesystem::path(item.path).parent_path().wstring();
    auto with_icon_of_item = [&](Item a) {
        a.icon_source = item.icon_source;
        a.icon_key = item.icon_cache_key();
        return a;
    };

    if (item.kind == ItemKind::Folder) {
        list.push_back(make_action(L"dir-open", L"Gezgin'de aç", tools_.explorer, 0));
        if (!tools_.code.empty()) list.push_back(make_action(L"dir-code", L"VS Code'da aç", tools_.code, 0));
        const std::wstring& devenv = tools_.devenv(vs_choice());
        if (!devenv.empty()) list.push_back(make_action(L"dir-vs", L"Visual Studio'da aç (Open Folder)", devenv, 0));
        list.push_back(make_action(L"file-terminal", L"Terminalde aç", {}, kGlyphTerminal, item.path));
        list.push_back(make_action(L"file-copy", L"Yolu kopyala", {}, kGlyphCopy, item.path));
        return list;
    }

    const std::wstring ext = lower_ext(item.path);
    const bool script = is_script_extension(ext);
    Item run = with_icon_of_item(make_action(script ? L"file-run" : L"file-open", script ? L"Çalıştır" : L"Aç", {}, kGlyphPlay, item.path));
    Item edit = make_action(L"file-edit", L"Düzenle — " + editor_name, editor_exe, 0, item.path);
    if (script && store_.current()->get_string(keys::kScriptAction) == "edit") {
        list.push_back(std::move(edit));
        list.push_back(std::move(run));
    } else {
        list.push_back(std::move(run));
        list.push_back(std::move(edit));
    }
    if (script) list.push_back(make_action(L"file-admin", L"Yönetici olarak çalıştır", {}, kGlyphAdmin));
    else list.push_back(make_action(L"file-openas", L"Birlikte aç…", {}, kGlyphOpenWith));
    list.push_back(make_action(L"file-location", L"Dosya konumunu göster", tools_.explorer, 0, dir));
    list.push_back(make_action(L"file-terminal", L"Bu klasörde terminal", {}, kGlyphTerminal, dir));
    list.push_back(make_action(L"file-copy", L"Yolu kopyala", {}, kGlyphCopy, item.path));
    return list;
}

bool App::run_file_action(const Item& item, const std::wstring& action) {
    if (item.kind != ItemKind::File && item.kind != ItemKind::Folder) return false;
    // Folder-level actions reuse the repository implementations on the right directory.
    auto as_dir_item = [&](const std::wstring& dir) {
        Item d;
        d.kind = ItemKind::Repo;
        d.title = std::filesystem::path(dir).filename().wstring();
        d.path = dir;
        d.target = dir;
        return d;
    };
    const std::wstring dir = item.kind == ItemKind::Folder ? item.path : std::filesystem::path(item.path).parent_path().wstring();
    if (action == L"file-run") run_script(item, false);
    else if (action == L"file-admin") run_script(item, true);
    else if (action == L"file-open") open_file(item, LaunchMode::Normal);
    else if (action == L"file-edit") edit_file(item);
    else if (action == L"file-location") open_file(item, LaunchMode::OpenLocation);
    else if (action == L"file-openas") run_detached(item.path, {}, dir, false, item.title, L"openas");
    else if (action == L"file-copy") {
        copy_to_clipboard(hwnd_, item.path);
        launcher_.hide();
    } else if (action == L"dir-open") open_file(item, LaunchMode::Normal);
    else if (action == L"file-terminal") run_action(as_dir_item(dir), L"terminal");
    else if (action == L"dir-code") run_action(as_dir_item(dir), L"code");
    else if (action == L"dir-vs") run_action(as_dir_item(dir), L"vs");
    else return false;
    return true;
}

}  // namespace kamil
