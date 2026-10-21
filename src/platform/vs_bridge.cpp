#include "platform/vs_bridge.h"

#include <objbase.h>  // after vs_bridge.h, which brings windows.h
#include <tlhelp32.h>
#include <uiautomation.h>

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <vector>

#include "core/i18n.h"
#include "core/log.h"
#include "core/text.h"
#include "platform/dispatch.h"
#include "platform/process.h"

namespace kamil {

const wchar_t* vs_job_verb(VsJob::Kind kind) {
    switch (kind) {
        case VsJob::Kind::Build: return L"Build";
        case VsJob::Kind::Rebuild: return L"Rebuild";
        case VsJob::Kind::Clean: return L"Clean";
        case VsJob::Kind::Configure: return L"Configure";
        case VsJob::Kind::Reconfigure: return L"Reconfigure";
        case VsJob::Kind::Debug: return L"Debug";
        case VsJob::Kind::Diagnose: return L"Diagnose";
        case VsJob::Kind::GoTo: return L"Go to";
        case VsJob::Kind::OpenSolution: return L"Open";
    }
    return L"";
}

namespace {

// GUID of the Output window's "Build" pane (stable across VS versions and UI languages).
constexpr wchar_t kBuildPaneGuid[] = L"{1BD8A850-02D1-11D1-BEE7-00A0C913D1F8}";

enum BuildState : long { kNotStarted = 1, kInProgress = 2, kDone = 3 };

// Retries calls that Visual Studio rejects while it is busy (RPC_E_CALL_REJECTED), instead of
// failing immediately. Registered on the bridge threads only.
class RetryFilter final : public IMessageFilter {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IMessageFilter) {
            *out = static_cast<IMessageFilter*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    DWORD STDMETHODCALLTYPE HandleInComingCall(DWORD, HTASK, DWORD, LPINTERFACEINFO) override { return SERVERCALL_ISHANDLED; }
    DWORD STDMETHODCALLTYPE RetryRejectedCall(HTASK, DWORD elapsed_ms, DWORD reject_type) override {
        if (reject_type == SERVERCALL_RETRYLATER && elapsed_ms < 30000) return 200;  // retry in 200 ms
        return static_cast<DWORD>(-1);
    }
    DWORD STDMETHODCALLTYPE MessagePending(HTASK, DWORD, DWORD) override { return PENDINGMSG_WAITDEFPROCESS; }
};

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring normalize(std::wstring p) {
    for (auto& c : p) c = c == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(c));
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

bool same_or_inside(const std::wstring& a, const std::wstring& b) {
    return a == b || (a.size() > b.size() && a.compare(0, b.size(), b) == 0 && a[b.size()] == L'\\');
}

std::wstring clock_text(ULONGLONG ms) {
    const ULONGLONG s = ms / 1000;
    wchar_t buf[16];
    swprintf(buf, 16, L"%llu:%02llu", s / 60, s % 60);
    return buf;
}

std::wstring vs_name(const std::wstring& version) { return version == L"17.0" ? L"VS 2022" : L"VS 2026"; }
std::wstring other_version(const std::wstring& version) { return version == L"17.0" ? L"18.0" : L"17.0"; }

// ---------------------------------------------------------------------------------------------
// Processes and windows (no COM: also works for instances Kamil cannot reach)

std::wstring process_image(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) out.assign(buf, n);
    CloseHandle(h);
    return out;
}

// True when the process is elevated, or when its token cannot be read from here, which for a
// process of the same user means the same thing.
bool process_elevated(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return true;
    bool elevated = true;
    HANDLE token = nullptr;
    if (OpenProcessToken(h, TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION e{};
        DWORD size = sizeof(e);
        elevated = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size) && e.TokenIsElevated;
        CloseHandle(token);
    }
    CloseHandle(h);
    return elevated;
}

// A build tool under Visual Studio that is still running: compiler, linker or build driver whose
// parent chain reaches devenv. A CMake build can be quiet for a long time (AUTOMOC, linking, a
// regenerate step) and can even report its build state as done in between, so the build only
// counts as finished once none of these is left. Empty when none runs.
std::wstring running_build_tool(DWORD devenv_pid) {
    if (!devenv_pid) return {};
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return {};
    std::map<DWORD, DWORD> parent;
    std::vector<std::pair<DWORD, std::wstring>> tools;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        parent[e.th32ProcessID] = e.th32ParentProcessID;
        const std::wstring name = lower(e.szExeFile);
        for (const wchar_t* tool : {L"ninja.exe", L"cmake.exe", L"msbuild.exe", L"cl.exe", L"link.exe", L"lib.exe", L"rc.exe", L"mt.exe",
                                    L"clang-cl.exe", L"lld-link.exe", L"moc.exe", L"uic.exe", L"rcc.exe"})
            if (name == tool) tools.emplace_back(e.th32ProcessID, name);
    }
    CloseHandle(snap);
    for (const auto& [pid, name] : tools) {
        DWORD p = pid;
        for (int depth = 0; depth < 24; ++depth) {
            const auto it = parent.find(p);
            if (it == parent.end() || it->second == p) break;
            p = it->second;
            if (p == devenv_pid) return name;
        }
    }
    return {};
}

bool self_elevated() {
    static const bool elevated = process_elevated(GetCurrentProcessId());
    return elevated;
}

struct VsWindow {
    DWORD pid = 0;
    std::wstring title;
};

struct WindowScan {
    std::vector<VsWindow> out;
    std::map<DWORD, bool> is_devenv;
};

BOOL CALLBACK collect_window(HWND hwnd, LPARAM lp) {
    auto& scan = *reinterpret_cast<WindowScan*>(lp);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    wchar_t title[512];
    const int n = GetWindowTextW(hwnd, title, 512);
    if (n <= 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    auto it = scan.is_devenv.find(pid);
    if (it == scan.is_devenv.end()) {
        const std::wstring image = lower(process_image(pid));
        it = scan.is_devenv.emplace(pid, image.size() > 11 && image.compare(image.size() - 11, 11, L"\\devenv.exe") == 0).first;
    }
    if (it->second) scan.out.push_back({pid, std::wstring(title, static_cast<size_t>(n))});
    return TRUE;
}

// Visible main windows of all devenv.exe processes ("SensorUI - Microsoft Visual Studio").
std::vector<VsWindow> devenv_windows() {
    WindowScan scan;
    EnumWindows(&collect_window, reinterpret_cast<LPARAM>(&scan));
    return scan.out;
}

bool title_matches(const std::wstring& title, const std::wstring& folder_name_lower) {
    if (folder_name_lower.empty()) return false;
    const std::wstring t = lower(title);
    if (t.compare(0, folder_name_lower.size(), folder_name_lower) != 0) return false;
    if (t.size() == folder_name_lower.size()) return true;
    const std::wstring rest = t.substr(folder_name_lower.size(), 2);
    return rest == L" -" || rest == L" (" || rest == L" —";
}

// ---------------------------------------------------------------------------------------------
// Instances in the Running Object Table

struct Instance {
    Disp dte;
    DWORD pid = 0;
    std::wstring version;   // "18.0" / "17.0"
    std::wstring solution;  // Solution.FullName (Open Folder: the folder, a file in it, or empty)
};

std::vector<Instance> running_instances(const std::wstring& version) {
    std::vector<Instance> out;
    ComPtr<IRunningObjectTable> rot;
    if (FAILED(GetRunningObjectTable(0, &rot))) return out;
    ComPtr<IEnumMoniker> en;
    if (FAILED(rot->EnumRunning(&en))) return out;
    const std::wstring prefix = L"!VisualStudio.DTE." + version + L":";
    ComPtr<IMoniker> moniker;
    while (en->Next(1, &moniker, nullptr) == S_OK) {
        ComPtr<IBindCtx> ctx;
        LPOLESTR raw = nullptr;
        if (SUCCEEDED(CreateBindCtx(0, &ctx)) && SUCCEEDED(moniker->GetDisplayName(ctx.Get(), nullptr, &raw)) && raw) {
            const std::wstring name = raw;
            CoTaskMemFree(raw);
            if (name.compare(0, prefix.size(), prefix) == 0) {
                ComPtr<IUnknown> unk;
                ComPtr<IDispatch> disp;
                if (SUCCEEDED(rot->GetObject(moniker.Get(), &unk)) && SUCCEEDED(unk.As(&disp))) {
                    Instance inst;
                    inst.dte = Disp(disp);
                    inst.pid = static_cast<DWORD>(_wtoi(name.c_str() + prefix.size()));
                    inst.version = version;
                    try {
                        inst.solution = inst.dte.get(L"Solution").get_string(L"FullName");
                    } catch (const ComError&) {
                    }
                    out.push_back(std::move(inst));
                }
            }
        }
        moniker.Reset();
    }
    return out;
}

// The wanted version first: a folder that is open in the other version is still used rather
// than opening it a second time.
std::vector<Instance> all_instances(const std::wstring& version) {
    auto out = running_instances(version);
    for (auto& inst : running_instances(other_version(version))) out.push_back(std::move(inst));
    return out;
}

std::wstring window_title(const std::vector<VsWindow>& windows, DWORD pid) {
    for (const auto& w : windows)
        if (w.pid == pid) return w.title;
    return {};
}

// Index of the instance that has `folder` open, or -1.
int match_instance(const std::vector<Instance>& instances, const std::wstring& folder, const std::wstring& folder_name,
                   const std::map<std::wstring, DWORD>& opened, const std::vector<VsWindow>& windows, std::wstring* how) {
    for (size_t i = 0; i < instances.size(); ++i) {
        const std::wstring sol = normalize(instances[i].solution);
        if (!sol.empty() && (same_or_inside(sol, folder) || same_or_inside(folder, sol))) {
            *how = L"Solution.FullName";
            return static_cast<int>(i);
        }
    }
    if (auto it = opened.find(folder); it != opened.end()) {
        for (size_t i = 0; i < instances.size(); ++i) {
            if (instances[i].pid == it->second) {
                *how = L"opened by Kamil";
                return static_cast<int>(i);
            }
        }
    }
    // Open Folder may leave Solution.FullName empty: the window title starts with the folder name.
    for (size_t i = 0; i < instances.size(); ++i) {
        if (!instances[i].solution.empty()) continue;
        std::wstring title = window_title(windows, instances[i].pid);
        if (title.empty()) {
            try {
                title = instances[i].dte.get(L"MainWindow").get_string(L"Caption");
            } catch (const ComError&) {
            }
        }
        if (title_matches(title, folder_name)) {
            *how = L"window title \"" + title + L"\"";
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------------------------

struct Context {
    HWND target;
    uint64_t job_id;
    const VsJob& job;
    const std::atomic<uint64_t>& cancel_id;
    mutable std::wstring last_progress;

    bool cancelled() const { return cancel_id.load() == job_id; }
    void check() const {
        if (cancelled()) throw ComError{E_ABORT, loc(L"Cancelled", L"İptal edildi")};
    }
    // Sleeps in small steps so that a cancel takes effect quickly.
    void wait(DWORD ms) const {
        for (DWORD slept = 0; slept < ms; slept += 100) {
            check();
            Sleep(std::min<DWORD>(100, ms - slept));
        }
        check();
    }
    void progress(const std::wstring& text) const {
        if (text == last_progress) return;
        last_progress = text;
        log_line(L"[vs] #" + std::to_wstring(job_id) + L" " + text);
        auto* e = new VsEvent;
        e->type = VsEvent::Type::Progress;
        e->job = job_id;
        e->kind = job.kind;
        e->text = text;
        post_owned(target, WM_KAMIL_VS_EVENT, e);
    }
    void output(std::wstring text) const {
        if (text.empty()) return;
        auto* e = new VsEvent;
        e->type = VsEvent::Type::Output;
        e->job = job_id;
        e->kind = job.kind;
        e->text = std::move(text);
        post_owned(target, WM_KAMIL_VS_EVENT, e);
    }
};

long build_state(Disp& dte) {
    try {
        return dte.get(L"Solution").get(L"SolutionBuild").get_long(L"BuildState");
    } catch (const ComError&) {
        return 0;
    }
}

// Waits until the instance with `pid` appears in the ROT (or the folder shows up in another
// instance, when devenv handed it over), bounded by the job's wait limit.
Disp wait_for_instance(const Context& ctx, DWORD pid, HANDLE process, const std::wstring& folder, const std::wstring& folder_name,
                       std::map<std::wstring, DWORD>& opened) {
    const std::wstring name = vs_name(ctx.job.dte_version);
    const ULONGLONG start = GetTickCount64();
    ULONGLONG registered_at = 0, exited_at = 0;
    for (;;) {
        ctx.wait(500);
        const ULONGLONG now = GetTickCount64();
        const auto instances = all_instances(ctx.job.dte_version);
        const auto windows = devenv_windows();
        for (const auto& inst : instances) {
            if (inst.pid != pid) continue;
            if (!registered_at) {
                registered_at = now;
                log_line(L"[vs] pid " + std::to_wstring(pid) + L" registered after " + clock_text(now - start));
            }
            const std::wstring sol = normalize(inst.solution);
            const bool loaded = (!sol.empty() && (same_or_inside(sol, folder) || same_or_inside(folder, sol))) ||
                                title_matches(window_title(windows, pid), folder_name);
            // Open Folder may never fill Solution.FullName: 15 s after registering is long enough;
            // the command wait that follows covers the rest of the CMake preparation.
            if (loaded || now - registered_at > 15000) {
                opened[folder] = pid;
                return inst.dte;
            }
        }
        if (!registered_at) {
            std::wstring how;
            if (const int i = match_instance(instances, folder, folder_name, opened, windows, &how); i >= 0) {
                log_line(L"[vs] folder found in pid " + std::to_wstring(instances[static_cast<size_t>(i)].pid) + L" via " + how);
                opened[folder] = instances[static_cast<size_t>(i)].pid;
                return instances[static_cast<size_t>(i)].dte;
            }
            if (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
                if (!exited_at) {
                    exited_at = now;
                } else if (now - exited_at > 5000) {
                    DWORD code = 0;
                    GetExitCodeProcess(process, &code);
                    throw ComError{E_FAIL, fmt(loc(L"devenv.exe exited (code {}) without opening the folder. If Visual Studio opened anyway, it "
                                                   L"probably runs as administrator, which Kamil cannot reach.",
                                                   L"devenv.exe klasörü açmadan kapandı (kod {}). Visual Studio yine de açıldıysa büyük olasılıkla "
                                                   L"yönetici olarak çalışıyor; Kamil ona ulaşamaz."),
                                               std::to_wstring(code))};
                }
            }
        }
        const ULONGLONG waited = now - start;
        if (waited > ctx.job.wait_ms)
            throw ComError{E_FAIL, fmt(loc(L"{} did not answer within {} s (dev.vs_wait_seconds).", L"{} {} sn içinde yanıt vermedi (dev.vs_wait_seconds)."),
                                       name, std::to_wstring(ctx.job.wait_ms / 1000))};
        ctx.progress(registered_at ? fmt(loc(L"{} is loading the folder…", L"{} klasörü yüklüyor…"), name)
                                   : fmt(loc(L"Waiting for {} to start (limit {})…", L"{} açılması bekleniyor (sınır {})…"), name,
                                         clock_text(ctx.job.wait_ms)));
    }
}

// Finds the instance that has the job's folder open, or opens the folder in a new instance.
Disp find_or_open(const Context& ctx, std::map<std::wstring, DWORD>& opened) {
    const VsJob& job = ctx.job;
    const std::wstring folder = normalize(job.folder);
    const std::wstring display_name = std::filesystem::path(job.folder).filename().wstring();
    const std::wstring folder_name = lower(display_name);
    const std::wstring name = vs_name(job.dte_version);

    const auto instances = all_instances(job.dte_version);
    const auto windows = devenv_windows();
    log_line(L"[vs] " + std::to_wstring(instances.size()) + L" instance(s) in the ROT, " + std::to_wstring(windows.size()) +
             L" devenv window(s)");
    std::wstring how;
    if (const int i = match_instance(instances, folder, folder_name, opened, windows, &how); i >= 0) {
        const Instance& inst = instances[static_cast<size_t>(i)];
        log_line(L"[vs] using pid " + std::to_wstring(inst.pid) + L" (" + vs_name(inst.version) + L") via " + how);
        return inst.dte;
    }

    // A Visual Studio window shows the folder but the process is not reachable over COM.
    for (const auto& w : windows) {
        if (std::any_of(instances.begin(), instances.end(), [&](const Instance& inst) { return inst.pid == w.pid; })) continue;
        if (!title_matches(w.title, folder_name)) continue;
        if (!self_elevated() && process_elevated(w.pid)) {
            log_line(L"[vs] pid " + std::to_wstring(w.pid) + L" has the folder open but is elevated");
            throw ComError{E_ACCESSDENIED,
                           fmt(loc(L"Visual Studio (pid {}) has {} open but runs as administrator, so Kamil cannot control it. Start Visual "
                                   L"Studio without \"Run as administrator\", or run Kamil as administrator too.",
                                   L"Visual Studio (pid {}) {} klasörünü açmış ama yönetici olarak çalışıyor; Kamil onu yönetemez. Visual "
                                   L"Studio'yu \"Yönetici olarak çalıştır\" olmadan açın ya da Kamil'i de yönetici olarak çalıştırın."),
                               std::to_wstring(w.pid), display_name)};
        }
        // Most likely still starting up: wait for it instead of opening the folder twice.
        log_line(L"[vs] pid " + std::to_wstring(w.pid) + L" shows the folder but is not in the ROT yet: waiting");
        return wait_for_instance(ctx, w.pid, nullptr, folder, folder_name, opened);
    }

    if (job.devenv.empty())
        throw ComError{E_FAIL, fmt(loc(L"{} is not installed (no devenv.exe found). Set dev.devenv_path in settings.yaml.",
                                       L"{} kurulu değil (devenv.exe bulunamadı). settings.yaml → dev.devenv_path ile yolu verin."),
                                   name)};

    ctx.progress(fmt(loc(L"Opening {} in {}…", L"{} klasörü {} ile açılıyor…"), display_name, name));
    std::wstring error;
    auto proc = launch_process(job.devenv, quote_arg(job.folder), job.folder, {}, false, &error);
    if (!proc) throw ComError{E_FAIL, fmt(loc(L"devenv.exe could not be started: {}", L"devenv.exe başlatılamadı: {}"), error)};
    CloseHandle(static_cast<HANDLE>(proc->thread));
    log_line(L"[vs] started " + job.devenv + L" pid " + std::to_wstring(proc->pid));
    struct Closer {
        HANDLE h;
        ~Closer() { CloseHandle(h); }
    } closer{static_cast<HANDLE>(proc->process)};
    return wait_for_instance(ctx, proc->pid, closer.h, folder, folder_name, opened);
}

void execute_now(Disp& dte, const std::wstring& command) {
    const Variant name = Variant::str(command);
    const Variant args = Variant::str(L"");
    dte.invoke(L"ExecuteCommand", {&name, &args}, DISPATCH_METHOD);
}

// Runs the first of `candidates` that exists in this Visual Studio. A command that exists but is
// disabled (CMake still generating, another build running) is waited for, up to the job's
// limit; a command that does not exist fails at once instead of being waited for.
std::wstring execute_command(const Context& ctx, Disp& dte, const std::vector<std::wstring>& candidates, const wchar_t* setting) {
    std::vector<std::pair<std::wstring, Disp>> commands;
    std::wstring names;
    Disp table = dte.get(L"Commands");
    for (const auto& name : candidates) {
        if (name.empty()) continue;
        names += (names.empty() ? L"" : L" / ") + name;
        try {
            const Variant n = Variant::str(name);
            Disp c(table.invoke(L"Item", {&n}).as_dispatch());
            if (c.valid()) commands.emplace_back(name, c);
        } catch (const ComError&) {
        }
    }
    if (commands.empty())
        throw ComError{E_FAIL, fmt(loc(L"Visual Studio has no command named {}. \"Kamil: Test VS connection\" lists the available names{}.",
                                       L"Visual Studio'da {} adlı komut yok. Kullanılabilir adları \"Kamil: VS bağlantısını test et\" listeler{}."),
                                   names,
                                   setting ? fmt(loc(L"; put the right one into {}", L"; doğru adı {} ayarına yazın"), setting) : std::wstring())};

    const ULONGLONG start = GetTickCount64();
    std::wstring last_error;
    for (int attempt = 0;; ++attempt) {
        ctx.check();
        for (auto& [name, command] : commands) {
            bool available = false;
            try {
                available = command.invoke(L"IsAvailable").as_long() != 0;
            } catch (const ComError&) {
            }
            // IsAvailable can lag behind the real state: try the command itself now and then too.
            if (!available && attempt % 5 != 4) continue;
            try {
                execute_now(dte, name);
                log_line(L"[vs] #" + std::to_wstring(ctx.job_id) + L" executed " + name + L" after " + clock_text(GetTickCount64() - start));
                return name;
            } catch (const ComError& e) {
                last_error = e.what;
            }
        }
        const ULONGLONG waited = GetTickCount64() - start;
        if (waited > ctx.job.wait_ms)
            throw ComError{E_FAIL, fmt(loc(L"{} stayed disabled in Visual Studio for {} s. Is the CMake project loaded and no other build "
                                           L"running? (limit: dev.vs_wait_seconds){}",
                                           L"{} Visual Studio'da {} sn boyunca devre dışı kaldı. CMake projesi yüklü mü, başka bir build "
                                           L"sürüyor mu? (sınır: dev.vs_wait_seconds){}"),
                                       commands.front().first, std::to_wstring(ctx.job.wait_ms / 1000),
                                       last_error.empty() ? std::wstring() : L"\n" + last_error)};
        if (build_state(dte) == kInProgress)
            ctx.progress(fmt(loc(L"Waiting: Visual Studio is busy with another build (limit {})", L"Bekleniyor: Visual Studio başka bir build yapıyor (sınır {})"),
                             clock_text(ctx.job.wait_ms)));
        else
            ctx.progress(fmt(loc(L"Waiting: {} is disabled in Visual Studio, CMake may still be generating (limit {})",
                                 L"Bekleniyor: {} Visual Studio'da devre dışı, CMake hazırlanıyor olabilir (sınır {})"),
                             commands.front().first, clock_text(ctx.job.wait_ms)));
        ctx.wait(1000);
    }
}

// The Output window's pane collection. DTE2.ToolWindows is not always reachable through the
// IDispatch Visual Studio puts in the ROT (it can expose only the older _DTE members), so the
// window is also looked up by its kind GUID through DTE.Windows, which every version has.
Disp output_panes(Disp& dte) {
    try {
        return dte.get(L"ToolWindows").get(L"OutputWindow").get(L"OutputWindowPanes");
    } catch (const ComError&) {
    }
    const Variant kind = Variant::str(L"{34E76E81-EE4A-11D0-AE2E-00A0C90FFFC3}");  // vsWindowKindOutput
    Disp window(dte.get(L"Windows").invoke(L"Item", {&kind}).as_dispatch());
    return window.get(L"Object").get(L"OutputWindowPanes");
}

Disp find_build_pane(Disp& dte) {
    Disp panes = output_panes(dte);
    const long n = panes.get_long(L"Count");
    Disp by_name;
    for (long i = 1; i <= n; ++i) {
        Disp pane = panes.item(i);
        std::wstring guid = pane.get_string(L"Guid");
        for (auto& c : guid) c = static_cast<wchar_t>(std::towupper(c));
        if (guid == kBuildPaneGuid) return pane;
        const std::wstring name = lower(pane.get_string(L"Name"));
        if (!by_name.valid() && (name.find(L"build") != std::wstring::npos || name.find(L"derle") != std::wstring::npos ||
                                 name.find(L"oluştur") != std::wstring::npos))
            by_name = pane;
    }
    return by_name;
}

Variant as_variant(const Disp& d) {
    Variant v;
    v.v.vt = VT_DISPATCH;
    v.v.pdispVal = d.raw();
    if (v.v.pdispVal) v.v.pdispVal->AddRef();
    return v;
}

// The first characters of the pane: they change when VS clears it for a new build.
std::wstring pane_head(Disp& pane) {
    if (!pane.valid()) return {};
    try {
        Disp start = pane.get(L"TextDocument").get(L"StartPoint").get(L"CreateEditPoint");
        const Variant count = Variant::i4(80);
        return start.invoke(L"GetText", {&count}, DISPATCH_METHOD).as_string();
    } catch (const ComError&) {
        return {};
    }
}

// Cheap progress probes, polled while a build runs.
long pane_length(Disp& pane) {
    if (!pane.valid()) return -1;
    try {
        return pane.get(L"TextDocument").get(L"EndPoint").get_long(L"AbsoluteCharOffset");
    } catch (const ComError&) {
        return -1;
    }
}

// Sets the value of a toolbar combo command (like choosing it in the drop-down).
void execute_combo(Disp& dte, const std::wstring& command, const std::wstring& value) {
    const Variant name = Variant::str(command);
    const Variant arg = Variant::str(value);
    dte.invoke(L"ExecuteCommand", {&name, &arg}, DISPATCH_METHOD);
}

// Pane text from a 1-based character offset to the end.
std::wstring pane_text_from(Disp& pane, long offset) {
    if (!pane.valid()) return {};
    try {
        Disp doc = pane.get(L"TextDocument");
        Disp from = doc.get(L"StartPoint").get(L"CreateEditPoint");
        const Variant at = Variant::i4(std::max(1L, offset));
        from.invoke(L"MoveToAbsoluteOffset", {&at}, DISPATCH_METHOD);
        const Variant end = as_variant(doc.get(L"EndPoint"));
        return from.invoke(L"GetText", {&end}, DISPATCH_METHOD).as_string();
    } catch (const ComError&) {
        return {};
    }
}

std::vector<std::wstring> build_commands(VsJob::Kind kind) {
    // Open Folder (CMake) has the *All commands; solutions have the *Solution ones.
    switch (kind) {
        case VsJob::Kind::Rebuild: return {L"Build.RebuildAll", L"Build.RebuildSolution"};
        case VsJob::Kind::Clean: return {L"Build.CleanAll", L"Build.CleanSolution"};
        default: return {L"Build.BuildAll", L"Build.BuildSolution"};
    }
}

// Starts a build command and waits for it to finish. Primary signal: SolutionBuild.BuildState
// (in progress -> done) or the end-of-build line; fallback for project systems that report
// neither: the Build pane text stops changing. Either way only once no build tool runs under VS.
void log_output_panes(Disp& dte);

BuildSummary run_build(const Context& ctx, Disp& dte, VsJob::Kind kind, const std::wstring& verb) {
    Disp pane;
    try {
        pane = find_build_pane(dte);
    } catch (const ComError&) {
    }
    if (!pane.valid()) log_output_panes(dte);
    // Output is streamed to the console from `streamed` (a 1-based pane offset) on. VS clears
    // the pane when the build starts: a changed beginning of the pane means "read from the top".
    long streamed = pane.valid() ? pane_length(pane) : 1;
    std::wstring head = pane_head(pane);
    std::wstring log;  // this build's output, for the summary
    auto stream = [&] {
        if (!pane.valid()) return;
        const long len = pane_length(pane);
        const std::wstring now_head = pane_head(pane);
        if (len < streamed || now_head != head) {
            streamed = 1;
            head = now_head;
        }
        if (len <= streamed) return;
        std::wstring chunk = pane_text_from(pane, streamed);
        streamed = len;
        log += chunk;
        ctx.output(std::move(chunk));
    };

    const std::wstring used = execute_command(ctx, dte, build_commands(kind), nullptr);
    const ULONGLONG start = GetTickCount64();
    ctx.progress(fmt(loc(L"{} running…", L"{} sürüyor…"), verb));

    DWORD pid = 0;
    try {
        GetWindowThreadProcessId(reinterpret_cast<HWND>(static_cast<intptr_t>(dte.get(L"MainWindow").get_long(L"HWnd"))), &pid);
    } catch (const ComError&) {
    }
    bool saw_progress = false, saw_output = false, end_line = false;
    std::wstring waited_for;  // build tool that kept the build going after VS looked done
    long last_len = pane_length(pane);
    ULONGLONG last_change = GetTickCount64();
    for (;;) {
        if (ctx.cancelled()) {
            try {
                execute_now(dte, L"Build.Cancel");
            } catch (const ComError&) {
            }
            stream();
            ctx.check();
        }
        Sleep(300);
        const long state = build_state(dte);
        if (state == kInProgress) saw_progress = true;
        if (!pane.valid()) {
            try {
                pane = find_build_pane(dte);  // created by the first build of the session
                streamed = 1;
            } catch (const ComError&) {
            }
        }
        const ULONGLONG now = GetTickCount64();
        const long len = pane_length(pane);
        if (len != last_len) {
            last_len = len;
            last_change = now;
            saw_output = true;
            stream();
            // The end-of-build line at the start of one of this build's last lines: "Build All
            // succeeded." (CMake), "========== Build: 1 succeeded" (MSBuild).
            end_line = false;
            std::wistringstream lines(log.substr(log.size() > 300 ? log.size() - 300 : 0));
            for (std::wstring l; std::getline(lines, l) && !end_line;) {
                const std::wstring_view t = trim(std::wstring_view(l));
                for (const wchar_t* end : {L"Build All succeeded", L"Build All failed", L"Rebuild All succeeded", L"Rebuild All failed",
                                           L"Clean All succeeded", L"Clean All failed", L"Build succeeded", L"Build failed",
                                           L"Rebuild succeeded", L"Rebuild failed", L"========== Build:", L"========== Rebuild",
                                           L"========== Clean"})
                    end_line |= t.starts_with(end);
            }
        }
        // Done when VS says so (build state, end line, or for project systems that never report a
        // build state: 15 s without new output), but only once no build tool runs under VS.
        const wchar_t* reason = saw_progress && state == kDone ? L"build state done"
                                : end_line                     ? L"end line in the Build pane"
                                : !saw_progress && saw_output && now - start > 5000 && now - last_change > 15000 ? L"15 s without output"
                                                                                                              : nullptr;
        if (reason) {
            const std::wstring tool = running_build_tool(pid);
            if (tool.empty()) {
                log_line(L"[vs] #" + std::to_wstring(ctx.job_id) + L" build end: " + reason +
                         (waited_for.empty() ? std::wstring() : L" (waited for " + waited_for + L")"));
                break;
            }
            waited_for = tool;
        }
        if (!saw_progress && !saw_output && now - start > 20000) {
            if (pane.valid())
                throw ComError{E_FAIL, fmt(loc(L"Visual Studio accepted {} but no build started (no build state change, no output in the Build pane).",
                                               L"Visual Studio {} komutunu kabul etti ama build başlamadı (build durumu değişmedi, Build bölmesinde çıktı yok)."),
                                           used)};
            throw ComError{E_FAIL, fmt(loc(L"{} was sent, but Kamil cannot follow it: Visual Studio reports no build state and has no Build pane. Check the result in VS.",
                                           L"{} gönderildi ama Kamil izleyemiyor: Visual Studio build durumu bildirmiyor ve Build bölmesi yok. Sonucu VS'te kontrol edin."),
                                       used)};
        }
        if (now - start > 60ull * 60 * 1000)
            throw ComError{E_FAIL, fmt(loc(L"{} did not finish within 60 minutes", L"{} 60 dakikada bitmedi"), verb)};
    }
    Sleep(200);  // the last lines ("Build All succeeded") may land just after the state change
    stream();
    return summarize_build_log(narrow(log));
}

std::wstring summary_line(const std::wstring& verb, const BuildSummary& s, ULONGLONG ms) {
    std::wstring line = s.failed() ? fmt(loc(L"✗ {} failed — {} errors, {} warnings, {} s", L"✗ {} başarısız — {} hata, {} uyarı, {} sn"), verb,
                                         std::to_wstring(s.errors), std::to_wstring(s.warnings), std::to_wstring(ms / 1000))
                                   : fmt(loc(L"✓ {} succeeded — {} errors, {} warnings, {} s", L"✓ {} tamam — {} hata, {} uyarı, {} sn"), verb,
                                         std::to_wstring(s.errors), std::to_wstring(s.warnings), std::to_wstring(ms / 1000));
    if (s.failed() && !s.issues.empty() && s.issues.front().error) {
        const auto& i = s.issues.front();
        line += L"\n" + widen(std::filesystem::path(i.file).filename().string()) + L"(" + std::to_wstring(i.line) + L"): " +
                widen(i.code) + L" " + widen(i.message);
    }
    return line;
}

// All commands whose canonical name contains one of `needles` (DTE command names are English
// regardless of the UI language), best first: menu commands ("Project.…") before the
// Solution Explorer context-menu variants ("OtherContextMenus.…"), which are only enabled
// while that menu is open. The table is large, so results are cached per needle set.
std::vector<std::wstring> discover_commands(Disp& dte, std::initializer_list<const wchar_t*> needles,
                                            std::map<std::wstring, std::vector<std::wstring>>& cache) {
    std::wstring cache_key;
    for (auto n : needles) cache_key += std::wstring(n) + L"|";
    if (auto it = cache.find(cache_key); it != cache.end()) return it->second;
    std::vector<std::wstring> found;
    Disp commands = dte.get(L"Commands");
    const long n = commands.get_long(L"Count");
    for (long i = 1; i <= n; ++i) {
        std::wstring name;
        try {
            name = commands.item(i).get_string(L"Name");
        } catch (const ComError&) {
            continue;
        }
        const std::wstring l = lower(name);
        for (auto needle : needles) {
            if (l.find(needle) == std::wstring::npos) continue;
            found.push_back(name);
            break;
        }
    }
    auto rank = [](const std::wstring& name) {
        const std::wstring l = lower(name);
        if (l.find(L"contextmenu") != std::wstring::npos) return 2;
        return l.rfind(L"project.", 0) == 0 ? 0 : 1;
    };
    std::stable_sort(found.begin(), found.end(), [&](const auto& a, const auto& b) { return rank(a) < rank(b); });
    log_line(L"[vs] CMake commands for " + cache_key + L": " + std::to_wstring(found.size()));
    for (const auto& f : found) log_line(L"[vs]   " + f);
    cache[cache_key] = found;
    return found;
}

void attach_debugger(Disp& dte, DWORD pid) {
    Disp processes = dte.get(L"Debugger").get(L"LocalProcesses");
    const long n = processes.get_long(L"Count");
    for (long i = 1; i <= n; ++i) {
        Disp p = processes.item(i);
        if (static_cast<DWORD>(p.get_long(L"ProcessID")) != pid) continue;
        try {
            const Variant engine = Variant::str(L"Native");
            p.invoke(L"Attach2", {&engine}, DISPATCH_METHOD);
        } catch (const ComError&) {
            p.invoke(L"Attach", {}, DISPATCH_METHOD);
        }
        return;
    }
    throw ComError{E_FAIL, fmt(loc(L"process not in Visual Studio's process list (pid {})", L"süreç VS'in listesinde yok (pid {})"), std::to_wstring(pid))};
}

void activate(Disp& dte) {
    try {
        dte.get(L"MainWindow").invoke(L"Activate", {}, DISPATCH_METHOD);
    } catch (const ComError&) {
    }
}

std::vector<std::pair<std::wstring, std::wstring>> toolbar_combos(Disp& dte);

void diagnose(const Context& ctx, VsEvent& result) {
    std::wostringstream out;
    out << loc(L"Kamil — Visual Studio connection report\n\n", L"Kamil — Visual Studio bağlantı raporu\n\n");
    out << loc(L"Kamil runs as administrator: ", L"Kamil yönetici olarak çalışıyor: ") << (self_elevated() ? loc(L"yes", L"evet") : loc(L"no", L"hayır"))
        << L"\n\n" << ctx.job.tools_report << L"\n";

    const auto windows = devenv_windows();
    out << loc(L"devenv.exe windows:\n", L"devenv.exe pencereleri:\n");
    if (windows.empty()) out << loc(L"  (none)\n", L"  (yok)\n");
    for (const auto& w : windows)
        out << L"  pid " << w.pid << (process_elevated(w.pid) ? loc(L"  [administrator]", L"  [yönetici]") : L"") << L"  \"" << w.title << L"\"\n";
    out << L"\n";

    for (const wchar_t* version : {L"18.0", L"17.0"}) {
        auto instances = running_instances(version);
        out << vs_name(version) << L" (DTE " << version << L"): " << instances.size() << loc(L" instance(s) reachable over COM\n", L" örnek COM ile erişilebilir\n");
        for (auto& inst : instances) {
            out << L"  pid " << inst.pid << L"  Solution.FullName = \"" << inst.solution << L"\"\n";
            try {
                out << loc(L"  Version: ", L"  Sürüm: ") << inst.dte.get_string(L"Version") << L"\n";
                out << L"  BuildState: " << build_state(inst.dte) << loc(L"  (1 not started, 2 in progress, 3 done)\n", L"  (1 başlamadı, 2 sürüyor, 3 bitti)\n");
                out << loc(L"  Output panes:\n", L"  Output bölmeleri:\n");
                try {
                    Disp panes = output_panes(inst.dte);
                    const long n = panes.get_long(L"Count");
                    for (long i = 1; i <= n; ++i) {
                        Disp pane = panes.item(i);
                        out << L"    " << pane.get_string(L"Name") << L"  " << pane.get_string(L"Guid") << L"\n";
                    }
                } catch (const ComError& e) {
                    out << L"    " << e.what << L"\n";
                }
                out << loc(L"  Toolbar drop-downs (UI Automation: name = current value):\n", L"  Araç çubuğu açılır listeleri (UI Automation: ad = değer):\n");
                for (const auto& [name, value] : toolbar_combos(inst.dte)) out << L"    \"" << name << L"\" = \"" << value << L"\"\n";
                out << loc(L"  CMake / build / debug commands (x = enabled now):\n", L"  CMake / build / debug komutları (x = şu an etkin):\n");
                Disp commands = inst.dte.get(L"Commands");
                const long count = commands.get_long(L"Count");
                ctx.progress(fmt(loc(L"Reading the command table ({})…", L"Komut tablosu okunuyor ({})…"), std::to_wstring(count)));
                for (long i = 1; i <= count; ++i) {
                    try {
                        Disp c = commands.item(i);
                        const std::wstring name = c.get_string(L"Name");
                        const std::wstring l = lower(name);
                        const bool interesting = l.find(L"cmake") != std::wstring::npos || l.find(L"cache") != std::wstring::npos ||
                                                 l.find(L"preset") != std::wstring::npos || l.find(L"configur") != std::wstring::npos ||
                                                 l.rfind(L"build.", 0) == 0 || l.rfind(L"debug.start", 0) == 0 ||
                                                 l.find(L"startupitem") != std::wstring::npos;
                        if (!interesting) continue;
                        const bool available = c.invoke(L"IsAvailable").as_long() != 0;
                        out << L"    [" << (available ? L"x" : L" ") << L"] " << name << L"\n";
                    } catch (const ComError&) {
                    }
                }
            } catch (const ComError& e) {
                out << loc(L"  ERROR: ", L"  HATA: ") << e.what << L"\n";
            }
            out << L"\n";
        }
    }
    if (!self_elevated())
        out << loc(L"Note: Kamil is not elevated, so a Visual Studio started as administrator is not reachable over COM (it is listed above as [administrator]).\n",
                   L"Not: Kamil yönetici değil; yönetici olarak açılmış bir Visual Studio COM ile erişilemez (yukarıda [yönetici] olarak listelenir).\n");
    out << loc(L"Log: ", L"Günlük: ") << log_file().wstring() << L"\n";
    std::ofstream f(ctx.job.report_file, std::ios::binary | std::ios::trunc);
    const std::string text = "\xEF\xBB\xBF" + narrow(out.str());
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    result.ok = static_cast<bool>(f);
    result.report = ctx.job.report_file;
    result.text = loc(L"VS connection report ready", L"VS bağlantı raporu hazır");
}

// Opens job.file at job.line in the instance that has job.folder open. Never starts Visual
// Studio: without such an instance the caller opens the file elsewhere.
void goto_location(const Context& ctx, VsEvent& result) {
    const VsJob& job = ctx.job;
    const std::wstring folder = normalize(job.folder);
    const std::wstring folder_name = lower(std::filesystem::path(job.folder).filename().wstring());
    const auto instances = all_instances(job.dte_version);
    const auto windows = devenv_windows();
    std::wstring how;
    const std::map<std::wstring, DWORD> none;
    const int i = job.folder.empty() ? -1 : match_instance(instances, folder, folder_name, none, windows, &how);
    if (i < 0) {
        result.text = L"no Visual Studio instance has the folder open";
        return;
    }
    Disp dte = instances[static_cast<size_t>(i)].dte;
    const Variant file = Variant::str(job.file);
    dte.get(L"ItemOperations").invoke(L"OpenFile", {&file}, DISPATCH_METHOD);
    if (job.line > 0) {
        const Variant line = Variant::i4(job.line);
        const Variant select = Variant::i4(0);
        dte.get(L"ActiveDocument").get(L"Selection").invoke(L"GotoLine", {&line, &select}, DISPATCH_METHOD);
    }
    activate(dte);
    result.ok = true;
}

// Brings the instance (either version) that has the solution job.file open to the front. Never
// starts Visual Studio: without such an instance the caller starts devenv with the solution.
void open_solution(const Context& ctx, VsEvent& result) {
    const std::wstring sln = normalize(ctx.job.file);
    for (auto& inst : all_instances(ctx.job.dte_version)) {
        if (normalize(inst.solution) != sln) continue;
        activate(inst.dte);
        log_line(L"[vs] " + ctx.job.file + L" is already open in pid " + std::to_wstring(inst.pid));
        result.ok = true;
        return;
    }
}

// Command object by name, or invalid when Visual Studio has no such command.
Disp command_object(Disp& dte, const std::wstring& name) {
    try {
        const Variant n = Variant::str(name);
        return Disp(dte.get(L"Commands").invoke(L"Item", {&n}).as_dispatch());
    } catch (const ComError&) {
        return {};
    }
}

bool command_available(Disp& command) {
    try {
        return command.valid() && command.invoke(L"IsAvailable").as_long() != 0;
    } catch (const ComError&) {
        return false;
    }
}

// ---------------------------------------------------------------------------------------------
// Toolbar drop-downs through UI Automation. In Visual Studio 2026's Open Folder (CMake) mode the
// configure preset list is not a command (the log showed no command with "preset" in its name),
// so it is driven like a user would: find the drop-down that shows one of the project's presets,
// open it, select the wanted item, close it.

const CLSID kClsidCUIAutomation = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};

std::wstring take_bstr(BSTR b) {
    std::wstring s = b ? std::wstring(b, SysStringLen(b)) : std::wstring();
    SysFreeString(b);
    return s;
}

struct Uia {
    ComPtr<IUIAutomation> uia;
    ComPtr<IUIAutomationElement> root;  // Visual Studio's main window
};

bool open_uia(Disp& dte, Uia* out) {
    HWND hwnd = nullptr;
    try {
        hwnd = reinterpret_cast<HWND>(static_cast<intptr_t>(dte.get(L"MainWindow").get_long(L"HWnd")));
    } catch (const ComError&) {
        return false;
    }
    if (!hwnd) return false;
    if (FAILED(CoCreateInstance(kClsidCUIAutomation, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                                reinterpret_cast<void**>(out->uia.GetAddressOf()))))
        return false;
    return SUCCEEDED(out->uia->ElementFromHandle(hwnd, &out->root)) && out->root;
}

ComPtr<IUIAutomationElementArray> find_all(Uia& u, IUIAutomationElement* from, int control_type) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = control_type;
    ComPtr<IUIAutomationCondition> cond;
    ComPtr<IUIAutomationElementArray> found;
    if (SUCCEEDED(u.uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond))) from->FindAll(TreeScope_Descendants, cond.Get(), &found);
    return found;
}

int count_of(IUIAutomationElementArray* a) {
    int n = 0;
    if (!a || FAILED(a->get_Length(&n))) return 0;
    return n;
}

ComPtr<IUIAutomationElement> element_at(IUIAutomationElementArray* a, int i) {
    ComPtr<IUIAutomationElement> e;
    a->GetElement(i, &e);
    return e;
}

std::wstring element_name(IUIAutomationElement* e) {
    BSTR b = nullptr;
    return SUCCEEDED(e->get_CurrentName(&b)) ? take_bstr(b) : std::wstring();
}

std::wstring combo_value(IUIAutomationElement* e) {
    ComPtr<IUIAutomationValuePattern> value;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_ValuePatternId, __uuidof(IUIAutomationValuePattern), reinterpret_cast<void**>(value.GetAddressOf()))) &&
        value) {
        BSTR b = nullptr;
        if (SUCCEEDED(value->get_CurrentValue(&b))) {
            std::wstring s = take_bstr(b);
            if (!s.empty()) return s;
        }
    }
    ComPtr<IUIAutomationSelectionPattern> selection;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_SelectionPatternId, __uuidof(IUIAutomationSelectionPattern),
                                         reinterpret_cast<void**>(selection.GetAddressOf()))) &&
        selection) {
        ComPtr<IUIAutomationElementArray> items;
        if (SUCCEEDED(selection->GetCurrentSelection(&items)) && count_of(items.Get()) > 0) return element_name(element_at(items.Get(), 0).Get());
    }
    return {};
}

bool same_text(const std::wstring& a, const std::wstring& b) { return !a.empty() && lower(a) == lower(b); }

// Every drop-down of the main window as (name, current value), for the connection report.
std::vector<std::pair<std::wstring, std::wstring>> toolbar_combos(Disp& dte) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    Uia u;
    if (!open_uia(dte, &u)) return out;
    auto combos = find_all(u, u.root.Get(), UIA_ComboBoxControlTypeId);
    for (int i = 0; i < count_of(combos.Get()); ++i) {
        auto e = element_at(combos.Get(), i);
        if (e) out.emplace_back(element_name(e.Get()), combo_value(e.Get()));
    }
    return out;
}

// Selects the job's preset in the drop-down that currently shows one of the project's presets
// (a "configure" one is preferred over build / test preset drop-downs).
bool select_preset_ui(Disp& dte, const VsJob& job, std::wstring* detail) {
    Uia u;
    if (!open_uia(dte, &u)) {
        *detail = L"UI Automation is not available";
        return false;
    }
    const std::wstring wanted[] = {job.preset_display, job.preset};
    auto is_wanted = [&](const std::wstring& s) { return same_text(s, wanted[0]) || same_text(s, wanted[1]); };
    auto is_known = [&](const std::wstring& s) {
        if (is_wanted(s)) return true;
        for (const auto& p : job.preset_names)
            if (same_text(s, p)) return true;
        return false;
    };
    auto combos = find_all(u, u.root.Get(), UIA_ComboBoxControlTypeId);
    ComPtr<IUIAutomationElement> combo;
    int best = 3;
    std::wstring seen;
    for (int i = 0; i < count_of(combos.Get()); ++i) {
        auto e = element_at(combos.Get(), i);
        if (!e) continue;
        const std::wstring name = element_name(e.Get()), value = combo_value(e.Get());
        seen += (seen.empty() ? L"" : L", ") + name + L"=" + value;
        if (!is_known(value)) continue;
        const std::wstring l = lower(name);
        const int rank = l.find(L"configure") != std::wstring::npos                                     ? 0
                         : l.find(L"build") != std::wstring::npos || l.find(L"test") != std::wstring::npos ? 2
                                                                                                         : 1;
        if (rank < best) {
            best = rank;
            combo = e;
        }
    }
    if (!combo) {
        *detail = L"no drop-down shows a preset of this project (" + seen + L")";
        return false;
    }
    if (is_wanted(combo_value(combo.Get()))) return true;  // already selected

    ComPtr<IUIAutomationExpandCollapsePattern> expand;
    combo->GetCurrentPatternAs(UIA_ExpandCollapsePatternId, __uuidof(IUIAutomationExpandCollapsePattern), reinterpret_cast<void**>(expand.GetAddressOf()));
    if (expand) {
        expand->Expand();
        Sleep(300);  // the list is filled when it opens
    }
    bool picked = false;
    auto items = find_all(u, combo.Get(), UIA_ListItemControlTypeId);
    for (int i = 0; i < count_of(items.Get()) && !picked; ++i) {
        auto item = element_at(items.Get(), i);
        if (!item || !is_wanted(element_name(item.Get()))) continue;
        ComPtr<IUIAutomationSelectionItemPattern> select;
        if (SUCCEEDED(item->GetCurrentPatternAs(UIA_SelectionItemPatternId, __uuidof(IUIAutomationSelectionItemPattern),
                                                reinterpret_cast<void**>(select.GetAddressOf()))) &&
            select && SUCCEEDED(select->Select())) {
            picked = true;
            continue;
        }
        ComPtr<IUIAutomationInvokePattern> invoke;
        if (SUCCEEDED(item->GetCurrentPatternAs(UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern), reinterpret_cast<void**>(invoke.GetAddressOf()))) &&
            invoke && SUCCEEDED(invoke->Invoke()))
            picked = true;
    }
    if (expand) expand->Collapse();
    Sleep(200);
    if (!picked) {
        *detail = L"\"" + element_name(combo.Get()) + L"\" has no item " + (wanted[0].empty() ? wanted[1] : wanted[0]) + L" (" +
                  std::to_wstring(count_of(items.Get())) + L" items)";
        return false;
    }
    if (!is_wanted(combo_value(combo.Get()))) {
        *detail = L"selected, but \"" + element_name(combo.Get()) + L"\" still shows " + combo_value(combo.Get());
        return false;
    }
    return true;
}

// Selects the configure preset in Visual Studio: first a command (a solution's
// "Build.SolutionConfigurations", or a preset command if the version has one), then the toolbar
// drop-down through UI Automation. Nothing is waited for. Returns a warning for the result text
// when VS did not switch, empty on success.
std::wstring select_preset(const Context& ctx, Disp& dte, std::map<std::wstring, std::vector<std::wstring>>& cache) {
    const VsJob& job = ctx.job;
    ctx.progress(fmt(loc(L"Selecting preset {} in Visual Studio…", L"Visual Studio'da {} preset'i seçiliyor…"), job.preset));
    std::vector<std::wstring> candidates;
    for (const auto& name : discover_commands(dte, {L"preset"}, cache)) {
        const std::wstring l = lower(name);
        if (l.find(L"contextmenu") != std::wstring::npos || l.find(L"build") != std::wstring::npos || l.find(L"test") != std::wstring::npos)
            continue;  // build / test preset drop-downs and context menus are not the configure preset
        candidates.push_back(name);
    }
    std::stable_partition(candidates.begin(), candidates.end(), [](const std::wstring& n) { return lower(n).find(L"configure") != std::wstring::npos; });
    candidates.push_back(L"Build.SolutionConfigurations");

    std::wstring last_error, tried;
    for (const auto& name : candidates) {
        Disp command = command_object(dte, name);
        const bool available = command_available(command);
        tried += (tried.empty() ? L"" : L", ") + name + (command.valid() ? (available ? L" [enabled]" : L" [disabled]") : L" [missing]");
        if (!available) continue;
        for (const std::wstring& value : {job.preset_display, job.preset}) {
            if (value.empty()) continue;
            try {
                execute_combo(dte, name, value);
                log_line(L"[vs] #" + std::to_wstring(ctx.job_id) + L" preset selected with " + name + L": " + value);
                return {};
            } catch (const ComError& e) {
                last_error = e.what;
            }
        }
    }
    std::wstring detail;
    if (select_preset_ui(dte, job, &detail)) {
        log_line(L"[vs] #" + std::to_wstring(ctx.job_id) + L" preset selected via toolbar: " + job.preset);
        return {};
    }
    log_line(L"[vs] #" + std::to_wstring(ctx.job_id) + L" preset not selected; tried: " + tried + (last_error.empty() ? L"" : L"; " + last_error) +
             L"; toolbar: " + detail);
    return fmt(loc(L"⚠ Visual Studio did not switch to preset {}; it used the preset selected in VS (toolbar: {}).",
                   L"⚠ Visual Studio {} preset'ine geçmedi; VS'te seçili preset kullanıldı (araç çubuğu: {})."),
               job.preset, detail);
}

// Lists the Output panes in the log once per session: which pane a build writes to differs
// between Visual Studio versions and project types.
void log_output_panes(Disp& dte) {
    static bool done = false;
    if (done) return;
    done = true;
    try {
        Disp panes = output_panes(dte);
        const long n = panes.get_long(L"Count");
        for (long i = 1; i <= n; ++i) {
            Disp pane = panes.item(i);
            log_line(L"[vs] output pane: " + pane.get_string(L"Name") + L"  " + pane.get_string(L"Guid"));
        }
        if (n == 0) log_line("[vs] no output panes");
    } catch (const ComError& e) {
        log_line(L"[vs] output panes not readable: " + e.what);
    }
}

struct ComScope {
    HRESULT hr;
    RetryFilter filter;
    IMessageFilter* old_filter = nullptr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) { CoRegisterMessageFilter(&filter, &old_filter); }
    ~ComScope() {
        CoRegisterMessageFilter(old_filter, nullptr);
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

void post_finished(HWND target, uint64_t id, VsJob::Kind kind, std::wstring text, bool cancelled) {
    auto* e = new VsEvent;
    e->type = VsEvent::Type::Finished;
    e->job = id;
    e->kind = kind;
    e->text = std::move(text);
    e->cancelled = cancelled;
    post_owned(target, WM_KAMIL_VS_EVENT, e);
}

}  // namespace

// ---------------------------------------------------------------------------------------------

VsBridge::VsBridge(HWND target) : target_(target), thread_([this] { run(); }) {}

VsBridge::~VsBridge() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        queue_.clear();
        if (running_id_) cancel_id_ = running_id_;
    }
    cv_.notify_one();
    thread_.join();
    for (auto& t : side_threads_) t.join();
}

uint64_t VsBridge::submit(VsJob job) {
    uint64_t id;
    {
        std::lock_guard lock(mutex_);
        id = next_id_++;
        if (job.kind == VsJob::Kind::Diagnose || job.kind == VsJob::Kind::GoTo || job.kind == VsJob::Kind::OpenSolution) {
            // Own thread: these must also work while a build runs or a job waits for VS.
            const HWND target = target_;
            side_threads_.emplace_back([target, id, job = std::move(job)] {
                ComScope com;
                static const std::atomic<uint64_t> never{0};
                const Context ctx{target, id, job, never, {}};
                auto* result = new VsEvent;
                result->type = VsEvent::Type::Finished;
                result->job = id;
                result->kind = job.kind;
                try {
                    if (job.kind == VsJob::Kind::GoTo) goto_location(ctx, *result);
                    else if (job.kind == VsJob::Kind::OpenSolution) open_solution(ctx, *result);
                    else diagnose(ctx, *result);
                } catch (const ComError& e) {
                    result->text = e.what;
                }
                post_owned(target, WM_KAMIL_VS_EVENT, result);
            });
            return id;
        }
        queue_.emplace_back(id, std::move(job));
    }
    cv_.notify_one();
    return id;
}

bool VsBridge::cancel(uint64_t id) {
    std::lock_guard lock(mutex_);
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (it->first != id) continue;
        const VsJob::Kind kind = it->second.kind;
        const std::wstring label = it->second.label;
        queue_.erase(it);
        log_line(L"[vs] #" + std::to_wstring(id) + L" removed from the queue");
        post_finished(target_, id, kind, fmt(loc(L"⨯ {} {} cancelled", L"⨯ {} {} iptal edildi"), vs_job_verb(kind), label), true);
        return true;
    }
    if (running_id_ == id) {
        log_line(L"[vs] #" + std::to_wstring(id) + L" cancel requested");
        cancel_id_ = id;
        return true;
    }
    return false;
}

void VsBridge::run() {
    ComScope com;
    std::map<std::wstring, std::vector<std::wstring>> command_cache;
    std::map<std::wstring, DWORD> opened;  // folder -> pid of the instance Kamil opened for it

    for (;;) {
        std::pair<uint64_t, VsJob> item;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) break;
            item = std::move(queue_.front());
            queue_.pop_front();
            running_id_ = item.first;
        }
        const VsJob& job = item.second;
        const Context ctx{target_, item.first, job, cancel_id_, {}};
        const std::wstring verb = std::wstring(vs_job_verb(job.kind)) + (job.label.empty() ? L"" : L" " + job.label);
        log_line(L"[vs] #" + std::to_wstring(item.first) + L" start: " + verb + L"  " + job.folder + L"  (" + vs_name(job.dte_version) + L")");
        {
            auto* e = new VsEvent;
            e->type = VsEvent::Type::Started;
            e->job = item.first;
            e->kind = job.kind;
            post_owned(target_, WM_KAMIL_VS_EVENT, e);
        }
        auto* result = new VsEvent;
        result->type = VsEvent::Type::Finished;
        result->job = item.first;
        result->kind = job.kind;
        const ULONGLONG started = GetTickCount64();
        try {
            ctx.progress(fmt(loc(L"{}: connecting to Visual Studio…", L"{}: Visual Studio'ya bağlanılıyor…"), verb));
            Disp dte = find_or_open(ctx, opened);
            const std::wstring preset_warning = job.preset.empty() ? std::wstring() : select_preset(ctx, dte, command_cache);
            switch (job.kind) {
                case VsJob::Kind::Build:
                case VsJob::Kind::Rebuild:
                case VsJob::Kind::Clean: {
                    result->build = run_build(ctx, dte, job.kind, verb);
                    result->ok = !result->build.failed();
                    result->text = summary_line(verb, result->build, GetTickCount64() - started);
                    break;
                }
                case VsJob::Kind::Configure:
                case VsJob::Kind::Reconfigure: {
                    const bool re = job.kind == VsJob::Kind::Reconfigure;
                    const std::wstring configured = re ? job.reconfigure_command : job.configure_command;
                    std::vector<std::wstring> commands;
                    if (!configured.empty()) {
                        commands.push_back(configured);
                    } else {
                        ctx.progress(loc(L"Looking up the CMake command in Visual Studio…", L"Visual Studio'da CMake komutu aranıyor…"));
                        commands = re ? discover_commands(dte, {L"deletecache"}, command_cache)
                                      : discover_commands(dte, {L"generatecache", L"configurecache"}, command_cache);
                    }
                    if (commands.empty())
                        throw ComError{E_FAIL, fmt(loc(L"Visual Studio has no CMake {} command. Put the name from the \"Kamil: Test VS connection\" "
                                                       L"report into {}.",
                                                       L"Visual Studio'da CMake {} komutu bulunamadı. \"Kamil: VS bağlantısını test et\" raporundaki "
                                                       L"adı {} ayarına yazın."),
                                                   re ? loc(L"delete-cache", L"önbellek silme") : L"configure",
                                                   re ? L"dev.vs_reconfigure_command" : L"dev.vs_configure_command")};
                    // Every candidate is tried; the first one Visual Studio has enabled runs.
                    const std::wstring used = execute_command(ctx, dte, commands, re ? L"dev.vs_reconfigure_command" : L"dev.vs_configure_command");
                    activate(dte);
                    result->ok = true;
                    result->text = fmt(loc(L"✓ {} sent (progress in Visual Studio's Output window)", L"✓ {} gönderildi (ilerleme VS'in Output penceresinde)"), used);
                    break;
                }
                case VsJob::Kind::Debug: {
                    if (job.build_first) {
                        const std::wstring build_verb = L"Build " + job.label;
                        result->build = run_build(ctx, dte, VsJob::Kind::Build, build_verb);
                        if (result->build.failed()) {
                            result->text = summary_line(build_verb, result->build, GetTickCount64() - started) +
                                           loc(L"\nDebug was not started.", L"\nDebug başlatılmadı.");
                            break;
                        }
                    }
                    ctx.check();
                    if (GetFileAttributesW(job.exe.c_str()) == INVALID_FILE_ATTRIBUTES)
                        throw ComError{E_FAIL, fmt(loc(L"Executable not found: {}", L"Çalıştırılabilir dosya yok: {}"), job.exe)};
                    ctx.progress(loc(L"Starting and attaching the debugger…", L"Başlatılıyor ve debugger bağlanıyor…"));
                    std::wstring error;
                    auto proc = launch_process(job.exe, job.args, job.cwd, job.console_title, true, &error);
                    if (!proc) throw ComError{E_FAIL, fmt(loc(L"Could not start: {}", L"Başlatılamadı: {}"), error)};
                    try {
                        attach_debugger(dte, proc->pid);
                    } catch (const ComError& e) {
                        ResumeThread(static_cast<HANDLE>(proc->thread));  // never leave it frozen
                        CloseHandle(static_cast<HANDLE>(proc->thread));
                        CloseHandle(static_cast<HANDLE>(proc->process));
                        throw ComError{e.hr, fmt(loc(L"Debugger could not attach (the program runs without it): {}",
                                                     L"Debugger bağlanamadı (program debugger'sız çalışıyor): {}"),
                                                 e.what)};
                    }
                    ResumeThread(static_cast<HANDLE>(proc->thread));
                    CloseHandle(static_cast<HANDLE>(proc->thread));
                    CloseHandle(static_cast<HANDLE>(proc->process));
                    activate(dte);
                    result->ok = true;
                    result->text = L"✓ Debug: " + std::filesystem::path(job.exe).filename().wstring() + L" " + job.args;
                    break;
                }
                default:
                    break;
            }
            if (!preset_warning.empty()) result->text += L"\n" + preset_warning;
        } catch (const ComError& e) {
            result->ok = false;
            if (e.hr == E_ABORT && ctx.cancelled()) {
                result->cancelled = true;
                result->text = fmt(loc(L"⨯ {} cancelled", L"⨯ {} iptal edildi"), verb);
            } else {
                result->text = e.what;
            }
        }
        log_line(L"[vs] #" + std::to_wstring(item.first) + L" finished in " + clock_text(GetTickCount64() - started) + L": " + result->text);
        post_owned(target_, WM_KAMIL_VS_EVENT, result);
        std::lock_guard lock(mutex_);
        running_id_ = 0;
        cancel_id_ = 0;
    }
}

}  // namespace kamil
