// Kamil Console: tabs for programs run by Kamil, Visual Studio build output and hidden custom
// commands; clicking an error opens the line in Visual Studio (or VS Code).

#include <algorithm>

#include "../res/resource.h"
#include "app/app.h"

#include <commctrl.h>  // after app.h, which brings windows.h

#include "core/i18n.h"
#include "core/log.h"
#include "core/settings_schema.h"
#include "core/text.h"
#include "platform/process.h"
#include "platform/system_theme.h"

namespace kamil {

namespace {

bool is_build_kind(VsJob::Kind k) {
    return k == VsJob::Kind::Build || k == VsJob::Kind::Rebuild || k == VsJob::Kind::Clean || k == VsJob::Kind::Debug;
}

std::wstring clock_text(ULONGLONG ms) {
    const ULONGLONG s = ms / 1000;
    wchar_t buf[16];
    swprintf(buf, 16, L"%llu:%02llu", s / 60, s % 60);
    return buf;
}

}  // namespace

void App::create_console() {
    HICON big = nullptr;
    if (FAILED(LoadIconMetric(instance_, MAKEINTRESOURCEW(IDI_KAMIL), LIM_LARGE, &big))) big = icon_;
    ConsoleWindow::Callbacks cb;
    cb.stop = [this](uint64_t tab) {
        auto it = console_runs_.find(tab);
        if (it == console_runs_.end()) return;
        if (it->second.vs_job) cancel_job(it->second.vs_job);
        else if (it->second.proc) it->second.proc->stop();
    };
    cb.rerun = [this](uint64_t tab) {
        auto it = console_runs_.find(tab);
        if (it == console_runs_.end() || !it->second.rerun) return;
        auto rerun = it->second.rerun;  // the call opens a new run (and may reuse this tab)
        rerun();
    };
    cb.open_location = [this](const ConsoleTab& tab, const std::wstring& file, int line) { open_location(tab, file, line); };
    cb.closed = [this](uint64_t tab) { console_runs_.erase(tab); };
    console_.create(instance_, big, std::move(cb), paths_.cache_dir / "console-window.txt");
}

void App::update_console_theme() {
    const auto s = store_.current();
    const std::string theme = s->get_string(keys::kTheme);
    const SystemTheme sys = read_system_theme();
    const bool dark = theme == "dark" || (theme == "auto" && sys.apps_dark);
    uint32_t accent = sys.accent_rgb;
    const std::string a = s->get_string(keys::kAccent);
    if (a.size() == 7 && a[0] == '#') accent = static_cast<uint32_t>(std::strtoul(a.c_str() + 1, nullptr, 16));
    console_.set_theme(dark, accent);
}

void App::show_console(bool activate) {
    if (activate) AllowSetForegroundWindow(ASFW_ANY);
    console_.show(activate);
}

uint64_t App::start_in_console(const std::wstring& title, const std::wstring& detail, const std::wstring& root, const std::wstring& exe,
                               const std::wstring& args, const std::wstring& dir, std::function<void()> rerun, const std::wstring& notify_name) {
    const uint64_t tab = console_.open_tab(title, detail, root, true, static_cast<bool>(rerun));
    ConsoleRun run;
    run.rerun = std::move(rerun);
    run.notify_name = notify_name;
    run.proc = std::make_unique<ChildProcess>();
    console_.note(tab, L"▶ " + quote_arg(exe) + (args.empty() ? L"" : L" " + args) + L"    (" + dir + L")");
    std::wstring error;
    if (!run.proc->start(tab, exe, args, dir, hwnd_, &error)) {
        console_.note(tab, fmt(loc(L"Could not start: {}", L"Başlatılamadı: {}"), error));
        console_.finish(tab, false, {});
        run.proc.reset();
        if (!notify_name.empty())
            notify(fmt(loc(L"{} could not be started", L"{} başlatılamadı"), notify_name), error, Tray::Balloon::Error);
    }
    log_line(L"[console] " + title + L": " + exe + L" " + args);
    console_runs_[tab] = std::move(run);
    return tab;
}

void App::on_console_output(const ConsoleOutput& out) {
    if (!out.exited) {
        console_.append(out.tab, out.text);
        return;
    }
    ConsoleTab* tab = console_.tab(out.tab);
    const bool ok = out.exit_code == 0;
    const std::wstring elapsed = tab ? clock_text(GetTickCount64() - tab->started) : std::wstring();
    console_.finish(out.tab, ok, fmt(loc(L"■ exit code {}   {}", L"■ çıkış kodu {}   {}"), std::to_wstring(static_cast<long>(out.exit_code)), elapsed));
    auto it = console_runs_.find(out.tab);
    if (it == console_runs_.end()) return;
    it->second.proc.reset();
    if (!it->second.notify_name.empty()) {
        // The last output line usually says what happened.
        std::wstring last;
        if (tab)
            for (size_t i = tab->buffer.size(); i-- > 0 && last.empty();)
                if (tab->buffer.line(i).kind != ConsoleLine::Kind::Note) last = std::wstring(trim(std::wstring_view(tab->buffer.line(i).text)));
        notify(ok ? fmt(loc(L"✓ {} done", L"✓ {} tamam"), it->second.notify_name)
                  : fmt(loc(L"✗ {} failed (exit code {})", L"✗ {} başarısız (çıkış kodu {})"), it->second.notify_name,
                        std::to_wstring(static_cast<long>(out.exit_code))),
               last + (last.empty() ? L"" : L"\n") + loc(L"Output: Kamil: Console", L"Çıktı: Kamil: Console"),
               ok ? Tray::Balloon::Info : Tray::Balloon::Error);
    }
}

// Visual Studio jobs with a build: their Build pane output goes into a console tab.
void App::on_vs_job_console(const VsEvent& e) {
    if (!is_build_kind(e.kind)) return;
    const std::string when = store_.current()->get_string(keys::kConsoleOnBuild);
    if (e.type == VsEvent::Type::Started) {
        auto j = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& x) { return x.id == e.job; });
        if (j == jobs_.end()) return;
        const std::wstring root = j->root;
        const VsJob::Kind kind = j->kind;
        const std::wstring vs = vs_choice() == "vs2022" ? L"Visual Studio 2022" : L"Visual Studio 2026";
        const uint64_t tab = console_.open_tab(job_title(*j), fmt(loc(L"{}: Build pane", L"{}: Build bölmesi"), vs), root, true, true);
        ConsoleRun run;
        run.vs_job = e.job;
        run.rerun = [this, root, kind] {
            for (const auto& r : repos_) {
                if (r.path != root) continue;
                Item item;
                item.kind = ItemKind::Repo;
                item.key = L"repo:" + r.path;
                item.title = r.name;
                item.path = item.target = r.path;
                submit_vs(kind, item);
            }
        };
        console_runs_[tab] = std::move(run);
        job_tabs_[e.job] = tab;
        if (when == "always") show_console(false);
        return;
    }
    auto t = job_tabs_.find(e.job);
    if (t == job_tabs_.end()) return;
    if (e.type == VsEvent::Type::Output) {
        console_.append(t->second, e.text);
        return;
    }
    if (e.type != VsEvent::Type::Finished) return;
    const uint64_t tab = t->second;
    job_tabs_.erase(t);
    if (auto run = console_runs_.find(tab); run != console_runs_.end()) run->second.vs_job = 0;
    const size_t nl = e.text.find(L'\n');
    console_.finish(tab, e.ok, nl == std::wstring::npos ? e.text : e.text.substr(0, nl));
    if (!e.ok && !e.cancelled && when != "never") {
        show_console(true);
        if (ConsoleTab* ct = console_.tab(tab); ct && ct->buffer.errors() > 0) {
            // Select the first error so F8 / Enter continue from there.
            if (auto first = ct->buffer.next_issue(ct->buffer.size() - 1, false)) {
                ct->sel_first = ct->sel_last = *first;
            }
        }
    }
}

void App::open_in_code(const std::wstring& file, int line) {
    const std::wstring code = tools_.code;
    executor_->post([code, file, line] {
        if (!code.empty()) run_program(code, L"-g " + quote_arg(file + L":" + std::to_wstring(std::max(1, line))));
        else open_in_editor(file);
    });
}

void App::open_location(const ConsoleTab& tab, const std::wstring& file, int line) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        notify(loc(L"File not found", L"Dosya bulunamadı"), file, Tray::Balloon::Warning);
        return;
    }
    // Project output: the Visual Studio that has the project open; otherwise (or if none has it) VS Code.
    if (!tab.root.empty() && is_project(tab.root)) {
        VsJob job;
        job.kind = VsJob::Kind::GoTo;
        job.folder = tab.root;
        job.dte_version = vs_choice() == "vs2022" ? L"17.0" : L"18.0";
        job.file = file;
        job.line = line;
        const uint64_t id = vs_->submit(std::move(job));
        pending_goto_[id] = {file, line};
        return;
    }
    open_in_code(file, line);
}

}  // namespace kamil
