// Files and folders under search.folders: background indexing, merging file hits into the
// result list, running scripts (Enter) and editing them (Alt+E).

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>

#include "app/action_item.h"
#include "app/app.h"
#include "core/i18n.h"
#include "core/log.h"
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
    // New settings (or the first scan): show the index saved for these settings right away and
    // watch the roots; the scan below then brings it up to date.
    const uint64_t hash = FileIndex::config_hash(roots, opt);
    if (hash != index_hash_) {
        index_hash_ = hash;
        file_index_.reset();
        std::ifstream in(paths_.cache_dir / "files.bin", std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            if (auto cached = FileIndex::deserialize(ss.str(), hash, roots)) {
                file_index_ = std::make_unique<FileIndex>(std::move(*cached));
                log_line("[files] cached index loaded: " + std::to_string(file_index_->size()) + " entries");
            }
        }
        std::vector<std::filesystem::path> paths;
        for (const auto& r : roots) paths.push_back(r.path);
        index_roots_ = roots.size();
        watched_roots_ = fs_watcher_.start(paths, hwnd_);
        log_line("[files] watching " + std::to_string(watched_roots_) + " of " + std::to_string(index_roots_) + " folders");
    }
    file_scan_cancel_ = false;
    files_scanned_at_ = GetTickCount64();
    const HWND target = hwnd_;
    file_scan_ = std::thread([this, roots = std::move(roots), opt = std::move(opt), target] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        auto* index = new FileIndex(FileIndex::build(roots, opt, &file_scan_cancel_));
        if (file_scan_cancel_) {
            delete index;
            return;
        }
        post_owned(target, WM_KAMIL_FILES_READY, index);
    });
}

void App::on_fs_changes(const std::vector<FsChange>& changes) {
    if (!file_index_) return;
    bool changed = false, rescan = false;
    for (const auto& c : changes) {
        switch (c.type) {
            case FsChange::Type::Overflow:
                rescan = true;
                break;
            case FsChange::Type::Added: {
                const DWORD attr = GetFileAttributesW(c.path.c_str());
                if (attr == INVALID_FILE_ATTRIBUTES) break;  // already gone again
                if (attr & FILE_ATTRIBUTE_DIRECTORY) {
                    // A new folder (possibly with content, e.g. moved in): rescan if it is inside the index.
                    if (file_index_->is_indexed_dir(std::filesystem::path(c.path).parent_path().wstring())) rescan = true;
                } else {
                    changed |= file_index_->add_file(c.path);
                }
                break;
            }
            case FsChange::Type::Removed:
                if (file_index_->is_indexed_dir(c.path)) rescan = true;
                changed |= file_index_->remove(c.path);
                break;
        }
    }
    if (rescan) SetTimer(hwnd_, kTimerFileRescan, 2000, nullptr);  // debounced: folder operations come in bursts
    if (changed) SetTimer(hwnd_, kTimerSaveIndex, 30'000, nullptr);
}

void App::save_index_cache() {
    if (!file_index_) return;
    auto blob = std::make_shared<std::string>(file_index_->serialize(index_hash_));
    const auto file = paths_.cache_dir / "files.bin";
    executor_->post([blob, file] {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        auto tmp = file;
        tmp += L".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(blob->data(), static_cast<std::streamsize>(blob->size()));
            if (!out) return;
        }
        std::filesystem::rename(tmp, file, ec);  // atomic replace: a crash never leaves half a cache
    });
}

void App::on_files_ready(std::unique_ptr<FileIndex> index) {
    files_scan_ms_ = GetTickCount64() - files_scanned_at_;
    log_line("[files] scan: " + std::to_string(index->size()) + " entries in " + std::to_string(files_scan_ms_) + " ms" +
             (index->truncated() ? " (truncated at search.max_files)" : ""));
    file_index_ = std::move(index);
    SetTimer(hwnd_, kTimerSaveIndex, 5000, nullptr);
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
    for (const auto& m : file_index_->search(matcher, limit, &file_search_)) {
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
        notify(fmt(loc(L"{} could not be run", L"{} çalıştırılamadı"), title), loc(L"The program it needs was not found.", L"Gereken program bulunamadı."),
               Tray::Balloon::Warning);
        return;
    }
    AllowSetForegroundWindow(ASFW_ANY);
    launcher_.hide();
    const HWND target = hwnd_;
    const std::wstring verb_text = verb ? verb : L"";
    executor_->post([exe, args, dir, admin, title, target, verb_text] {
        const std::wstring err = run_program(exe, args, dir, admin, verb_text.empty() ? nullptr : verb_text.c_str());
        if (!err.empty())
            post_owned(target, WM_KAMIL_NOTIFY,
                       new Notification{Notification::Level::Error, fmt(loc(L"{} could not be run", L"{} çalıştırılamadı"), title), err});
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
            notify(loc(L"Python not found", L"Python bulunamadı"),
                   loc(L"No py launcher or python.exe. Set the path in settings.yaml → scripts.python.",
                       L"py launcher veya python.exe yok. settings.yaml → scripts.python ile yolu verin."),
                   Tray::Balloon::Warning);
            return;
        }
        if (keep) run_detached(cmd, via_cmd(py, quote_arg(path)), dir, admin, item.title);
        else run_detached(py, quote_arg(path), dir, admin, item.title);
    } else if (ext == L".pyw") {
        const std::wstring pyw = python_for(true);
        if (pyw.empty()) {
            notify(loc(L"Python not found", L"Python bulunamadı"),
                   loc(L"No pyw.exe / pythonw.exe. Set the path in settings.yaml → scripts.python.",
                       L"pyw.exe / pythonw.exe yok. settings.yaml → scripts.python ile yolu verin."),
                   Tray::Balloon::Warning);
            return;
        }
        run_detached(pyw, quote_arg(path), dir, admin, item.title);
    } else if (ext == L".sh") {
        if (tools_.bash.empty()) {
            notify(loc(L"bash not found", L"bash bulunamadı"), loc(L"Running .sh files needs Git for Windows.", L".sh çalıştırmak için Git for Windows gerekli."),
                   Tray::Balloon::Warning);
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
    const std::wstring editor_name = editor_exe == notepad ? loc(L"Notepad", L"Not Defteri") : L"VS Code";
    const std::wstring dir = std::filesystem::path(item.path).parent_path().wstring();
    auto with_icon_of_item = [&](Item a) {
        a.icon_source = item.icon_source;
        a.icon_key = item.icon_cache_key();
        return a;
    };

    if (item.kind == ItemKind::Folder) {
        list.push_back(make_action(L"dir-open", loc(L"Open in Explorer", L"Gezgin'de aç"), tools_.explorer, 0));
        if (!tools_.code.empty()) list.push_back(make_action(L"dir-code", loc(L"Open in VS Code", L"VS Code'da aç"), tools_.code, 0));
        const std::wstring& devenv = tools_.devenv(vs_choice());
        if (!devenv.empty()) list.push_back(make_action(L"dir-vs", loc(L"Open in Visual Studio (Open Folder)", L"Visual Studio'da aç (Open Folder)"), devenv, 0));
        list.push_back(make_action(L"file-terminal", loc(L"Open in terminal", L"Terminalde aç"), {}, kGlyphTerminal, item.path));
        list.push_back(make_action(L"file-copy", loc(L"Copy path", L"Yolu kopyala"), {}, kGlyphCopy, item.path));
        return list;
    }

    const std::wstring ext = lower_ext(item.path);
    const bool script = is_script_extension(ext);
    Item run = with_icon_of_item(
        make_action(script ? L"file-run" : L"file-open", script ? loc(L"Run", L"Çalıştır") : loc(L"Open", L"Aç"), {}, kGlyphPlay, item.path));
    Item edit = make_action(L"file-edit", fmt(loc(L"Edit — {}", L"Düzenle — {}"), editor_name), editor_exe, 0, item.path);
    if (script && store_.current()->get_string(keys::kScriptAction) == "edit") {
        list.push_back(std::move(edit));
        list.push_back(std::move(run));
    } else {
        list.push_back(std::move(run));
        list.push_back(std::move(edit));
    }
    if (script) list.push_back(make_action(L"file-admin", loc(L"Run as administrator", L"Yönetici olarak çalıştır"), {}, kGlyphAdmin));
    else list.push_back(make_action(L"file-openas", loc(L"Open with…", L"Birlikte aç…"), {}, kGlyphOpenWith));
    list.push_back(make_action(L"file-location", loc(L"Show file location", L"Dosya konumunu göster"), tools_.explorer, 0, dir));
    list.push_back(make_action(L"file-terminal", loc(L"Terminal in this folder", L"Bu klasörde terminal"), {}, kGlyphTerminal, dir));
    list.push_back(make_action(L"file-copy", loc(L"Copy path", L"Yolu kopyala"), {}, kGlyphCopy, item.path));
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
