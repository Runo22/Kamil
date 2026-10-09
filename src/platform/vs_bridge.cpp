#include "platform/vs_bridge.h"

#include <objbase.h>  // after vs_bridge.h, which brings windows.h

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

Disp find_build_pane(Disp& dte) {
    Disp panes = dte.get(L"ToolWindows").get(L"OutputWindow").get(L"OutputWindowPanes");
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

// Whole pane text (read once, at the end of a build).
std::wstring pane_text(Disp& pane) {
    if (!pane.valid()) return {};
    try {
        Disp doc = pane.get(L"TextDocument");
        Disp start = doc.get(L"StartPoint").get(L"CreateEditPoint");
        const Variant end = as_variant(doc.get(L"EndPoint"));
        return start.invoke(L"GetText", {&end}, DISPATCH_METHOD).as_string();
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

std::wstring pane_tail(Disp& pane, long chars) {
    if (!pane.valid()) return {};
    try {
        Disp doc = pane.get(L"TextDocument");
        Disp from = doc.get(L"EndPoint").get(L"CreateEditPoint");
        const Variant count = Variant::i4(chars);
        from.invoke(L"CharLeft", {&count}, DISPATCH_METHOD);
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
// (in progress -> done). Fallback for project systems that do not report it: the Build pane
// text stops changing for a few seconds.
BuildSummary run_build(const Context& ctx, Disp& dte, VsJob::Kind kind, const std::wstring& verb) {
    Disp pane;
    try {
        pane = find_build_pane(dte);
    } catch (const ComError&) {
    }
    const size_t before = pane_text(pane).size();

    const std::wstring used = execute_command(ctx, dte, build_commands(kind), nullptr);
    const ULONGLONG start = GetTickCount64();
    ctx.progress(fmt(loc(L"{} running…", L"{} sürüyor…"), verb));

    bool saw_progress = false, saw_output = false;
    const long first_len = pane_length(pane);
    long last_len = first_len;
    ULONGLONG last_change = GetTickCount64();
    for (;;) {
        if (ctx.cancelled()) {
            try {
                execute_now(dte, L"Build.Cancel");
            } catch (const ComError&) {
            }
            ctx.check();
        }
        Sleep(300);
        const long state = build_state(dte);
        if (state == kInProgress) saw_progress = true;
        if (saw_progress && state == kDone) break;
        if (!pane.valid()) {
            try {
                pane = find_build_pane(dte);
            } catch (const ComError&) {
            }
        }
        const ULONGLONG now = GetTickCount64();
        const long len = pane_length(pane);
        if (len != last_len) {
            last_len = len;
            last_change = now;
            saw_output = true;
            if (!saw_progress) {
                const std::wstring tail = pane_tail(pane, 200);
                if (tail.find(L"All succeeded") != std::wstring::npos || tail.find(L"All failed") != std::wstring::npos) break;
            }
        }
        // Project systems that never report BuildState: treat 15 s without new output as done.
        if (!saw_progress && saw_output && now - start > 5000 && now - last_change > 15000) break;
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

    std::wstring text = pane_text(pane);
    if (text.size() >= before) text.erase(0, before);  // only this build's output (pane may also be cleared)
    return summarize_build_log(narrow(text));
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

// Finds a command whose canonical name contains one of `needles` (DTE command names are English
// regardless of the UI language). The table is large, so results are cached per instance.
std::wstring discover_command(Disp& dte, std::initializer_list<const wchar_t*> needles, std::map<std::wstring, std::wstring>& cache) {
    std::wstring cache_key;
    for (auto n : needles) cache_key += std::wstring(n) + L"|";
    if (auto it = cache.find(cache_key); it != cache.end()) return it->second;
    std::wstring found;
    Disp commands = dte.get(L"Commands");
    const long n = commands.get_long(L"Count");
    for (long i = 1; i <= n && found.empty(); ++i) {
        std::wstring name;
        try {
            name = commands.item(i).get_string(L"Name");
        } catch (const ComError&) {
            continue;
        }
        const std::wstring l = lower(name);
        for (auto needle : needles)
            if (l.find(needle) != std::wstring::npos) found = name;
    }
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
                Disp panes = inst.dte.get(L"ToolWindows").get(L"OutputWindow").get(L"OutputWindowPanes");
                const long n = panes.get_long(L"Count");
                out << loc(L"  Output panes:\n", L"  Output bölmeleri:\n");
                for (long i = 1; i <= n; ++i) {
                    Disp pane = panes.item(i);
                    out << L"    " << pane.get_string(L"Name") << L"  " << pane.get_string(L"Guid") << L"\n";
                }
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
        if (job.kind == VsJob::Kind::Diagnose) {
            // Own thread: the connection test must also work while a job waits for VS.
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
                    diagnose(ctx, *result);
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
    std::map<std::wstring, std::wstring> command_cache;
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
                    std::wstring command = re ? job.reconfigure_command : job.configure_command;
                    if (command.empty()) {
                        ctx.progress(loc(L"Looking up the CMake command in Visual Studio…", L"Visual Studio'da CMake komutu aranıyor…"));
                        command = re ? discover_command(dte, {L"deletecache"}, command_cache)
                                     : discover_command(dte, {L"generatecache", L"configurecache"}, command_cache);
                    }
                    if (command.empty())
                        throw ComError{E_FAIL, fmt(loc(L"Visual Studio has no CMake {} command. Put the name from the \"Kamil: Test VS connection\" "
                                                       L"report into {}.",
                                                       L"Visual Studio'da CMake {} komutu bulunamadı. \"Kamil: VS bağlantısını test et\" raporundaki "
                                                       L"adı {} ayarına yazın."),
                                                   re ? loc(L"delete-cache", L"önbellek silme") : L"configure",
                                                   re ? L"dev.vs_reconfigure_command" : L"dev.vs_configure_command")};
                    const std::wstring used = execute_command(ctx, dte, {command}, re ? L"dev.vs_reconfigure_command" : L"dev.vs_configure_command");
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
