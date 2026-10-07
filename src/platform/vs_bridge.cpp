#include "platform/vs_bridge.h"

#include <objbase.h>  // after vs_bridge.h, which brings windows.h

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <vector>

#include "core/text.h"
#include "platform/dispatch.h"
#include "platform/process.h"

namespace kamil {

namespace {

// GUID of the Output window's "Build" pane (stable across VS versions and UI languages).
constexpr wchar_t kBuildPaneGuid[] = L"{1BD8A850-02D1-11D1-BEE7-00A0C913D1F8}";

enum BuildState : long { kNotStarted = 1, kInProgress = 2, kDone = 3 };

// Retries calls that Visual Studio rejects while it is busy (RPC_E_CALL_REJECTED), instead of
// failing immediately. Registered on the bridge thread only.
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
        if (reject_type == SERVERCALL_RETRYLATER && elapsed_ms < 60000) return 200;  // retry in 200 ms
        return static_cast<DWORD>(-1);
    }
    DWORD STDMETHODCALLTYPE MessagePending(HTASK, DWORD, DWORD) override { return PENDINGMSG_WAITDEFPROCESS; }
};

std::wstring normalize(std::wstring p) {
    for (auto& c : p) c = c == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(c));
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

bool same_or_inside(const std::wstring& a, const std::wstring& b) {
    return a == b || (a.size() > b.size() && a.compare(0, b.size(), b) == 0 && a[b.size()] == L'\\');
}

struct Instance {
    Disp dte;
    DWORD pid = 0;
    std::wstring solution;  // Solution.FullName (Open Folder: the folder, or a file in it)
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

struct Context {
    HWND target;
    uint64_t job_id;
    const VsJob& job;

    void progress(const std::wstring& text) const {
        auto* e = new VsEvent;
        e->type = VsEvent::Type::Progress;
        e->job = job_id;
        e->kind = job.kind;
        e->text = text;
        post_owned(target, WM_KAMIL_VS_EVENT, e);
    }
};

std::wstring vs_name(const std::wstring& version) { return version == L"17.0" ? L"VS 2022" : L"VS 2026"; }

// Finds the instance that has `folder` open, or opens the folder in a new instance.
Disp find_or_open(const Context& ctx) {
    const VsJob& job = ctx.job;
    const std::wstring folder = normalize(job.folder);
    // Instances Kamil opened itself (pid by folder), in case Solution.FullName stays empty in
    // Open Folder mode. Only the bridge thread touches this map.
    static std::map<std::wstring, DWORD> opened;
    std::wstring folder_name = std::filesystem::path(job.folder).filename().wstring();
    for (auto& c : folder_name) c = static_cast<wchar_t>(std::towlower(c));
    auto instances = running_instances(job.dte_version);
    for (auto& inst : instances) {
        const std::wstring sol = normalize(inst.solution);
        if (!sol.empty() && (same_or_inside(sol, folder) || same_or_inside(folder, sol))) return inst.dte;
    }
    if (auto it = opened.find(folder); it != opened.end())
        for (auto& inst : instances)
            if (inst.pid == it->second) return inst.dte;
    // Last resort: the main window caption starts with the folder name ("SensorUI - Microsoft Visual Studio").
    for (auto& inst : instances) {
        if (!inst.solution.empty()) continue;
        try {
            std::wstring caption = inst.dte.get(L"MainWindow").get_string(L"Caption");
            for (auto& c : caption) c = static_cast<wchar_t>(std::towlower(c));
            if (!folder_name.empty() && caption.compare(0, folder_name.size() + 2, folder_name + L" -") == 0) return inst.dte;
        } catch (const ComError&) {
        }
    }
    if (job.devenv.empty()) throw ComError{E_FAIL, vs_name(job.dte_version) + L" kurulu değil (vswhere bulamadı)"};

    ctx.progress(vs_name(job.dte_version) + L" açılıyor (Open Folder)…");
    std::wstring error;
    auto proc = launch_process(job.devenv, quote_arg(job.folder), job.folder, {}, false, &error);
    if (!proc) throw ComError{E_FAIL, L"devenv başlatılamadı: " + error};
    CloseHandle(static_cast<HANDLE>(proc->thread));
    CloseHandle(static_cast<HANDLE>(proc->process));
    opened[folder] = proc->pid;

    // Wait for the new instance to register in the ROT, then for the folder to be loaded.
    const ULONGLONG deadline = GetTickCount64() + 180'000;
    while (GetTickCount64() < deadline) {
        Sleep(500);
        for (auto& inst : running_instances(job.dte_version)) {
            if (inst.pid != proc->pid) continue;
            if (!inst.solution.empty() || GetTickCount64() + 150'000 > deadline) return inst.dte;  // registered for 30 s: use it
            ctx.progress(vs_name(job.dte_version) + L" klasörü yüklüyor…");
        }
    }
    throw ComError{E_FAIL, vs_name(job.dte_version) + L" zamanında hazır olmadı"};
}

// ExecuteCommand fails while the command is disabled (e.g. CMake is still generating):
// retry until it is accepted.
void execute_command(const Context& ctx, Disp& dte, const std::wstring& command, ULONGLONG timeout_ms) {
    const Variant name = Variant::str(command);
    const Variant args = Variant::str(L"");
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    bool reported = false;
    for (;;) {
        try {
            dte.invoke(L"ExecuteCommand", {&name, &args}, DISPATCH_METHOD);
            return;
        } catch (const ComError& e) {
            if (GetTickCount64() > deadline) throw ComError{e.hr, command + L" çalıştırılamadı: " + e.what};
            if (!reported) {
                ctx.progress(L"VS hazır değil (CMake hazırlanıyor olabilir), bekleniyor…");
                reported = true;
            }
            Sleep(1000);
        }
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
        std::wstring name = pane.get_string(L"Name");
        for (auto& c : name) c = static_cast<wchar_t>(std::towlower(c));
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

long build_state(Disp& dte) {
    try {
        return dte.get(L"Solution").get(L"SolutionBuild").get_long(L"BuildState");
    } catch (const ComError&) {
        return 0;
    }
}

// Starts a build command and waits for it to finish. Primary signal: SolutionBuild.BuildState
// (in progress -> done). Fallback for project systems that do not report it: the Build pane
// text stops changing for a few seconds.
BuildSummary run_build(const Context& ctx, Disp& dte, const std::wstring& command, const std::wstring& verb) {
    Disp pane;
    try {
        pane = find_build_pane(dte);
    } catch (const ComError&) {
    }
    const size_t before = pane_text(pane).size();

    execute_command(ctx, dte, command, 300'000);
    const ULONGLONG start = GetTickCount64();
    ctx.progress(verb + L" sürüyor…");

    bool saw_progress = false;
    long last_len = pane_length(pane);
    ULONGLONG last_change = GetTickCount64();
    for (;;) {
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
            if (!saw_progress) {
                const std::wstring tail = pane_tail(pane, 200);
                if (tail.find(L"All succeeded") != std::wstring::npos || tail.find(L"All failed") != std::wstring::npos) break;
            }
        }
        // Project systems that never report BuildState: treat 15 s without new output as done.
        if (!saw_progress && now - start > 5000 && now - last_change > 15000) break;
        if (now - start > 60ull * 60 * 1000) throw ComError{E_FAIL, verb + L" 60 dakikada bitmedi"};
    }

    std::wstring text = pane_text(pane);
    if (text.size() >= before) text.erase(0, before);  // only this build's output (pane may also be cleared)
    BuildSummary summary = summarize_build_log(narrow(text));
    return summary;
}

std::wstring summary_line(const std::wstring& verb, const BuildSummary& s, ULONGLONG ms) {
    std::wstring line = (s.failed() ? L"✗ " : L"✓ ") + verb + (s.failed() ? L" başarısız" : L" tamam") + L" — " +
                        std::to_wstring(s.errors) + L" hata, " + std::to_wstring(s.warnings) + L" uyarı, " +
                        std::to_wstring(ms / 1000) + L" sn";
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
        std::wstring lower = name;
        for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
        for (auto needle : needles)
            if (lower.find(needle) != std::wstring::npos) found = name;
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
    throw ComError{E_FAIL, L"süreç VS'in listesinde bulunamadı (pid " + std::to_wstring(pid) + L")"};
}

void activate(Disp& dte) {
    try {
        dte.get(L"MainWindow").invoke(L"Activate", {}, DISPATCH_METHOD);
    } catch (const ComError&) {
    }
}

void diagnose(const Context& ctx, VsEvent& result) {
    std::wostringstream out;
    out << L"Kamil — Visual Studio bağlantı raporu\n\n";
    for (const wchar_t* version : {L"18.0", L"17.0"}) {
        auto instances = running_instances(version);
        out << vs_name(version) << L" (DTE " << version << L"): " << instances.size() << L" açık örnek\n";
        for (auto& inst : instances) {
            out << L"  pid " << inst.pid << L"  Solution.FullName = \"" << inst.solution << L"\"\n";
            try {
                out << L"  Sürüm: " << inst.dte.get_string(L"Version") << L"\n";
                out << L"  BuildState: " << build_state(inst.dte) << L"  (1 başlamadı, 2 sürüyor, 3 bitti)\n";
                Disp panes = inst.dte.get(L"ToolWindows").get(L"OutputWindow").get(L"OutputWindowPanes");
                const long n = panes.get_long(L"Count");
                out << L"  Output bölmeleri:\n";
                for (long i = 1; i <= n; ++i) {
                    Disp pane = panes.item(i);
                    out << L"    " << pane.get_string(L"Name") << L"  " << pane.get_string(L"Guid") << L"\n";
                }
                out << L"  CMake / build / debug komutları (kullanılabilir = x):\n";
                Disp commands = inst.dte.get(L"Commands");
                const long count = commands.get_long(L"Count");
                ctx.progress(L"Komut tablosu okunuyor (" + std::to_wstring(count) + L")…");
                for (long i = 1; i <= count; ++i) {
                    try {
                        Disp c = commands.item(i);
                        const std::wstring name = c.get_string(L"Name");
                        std::wstring lower = name;
                        for (auto& ch : lower) ch = static_cast<wchar_t>(std::towlower(ch));
                        const bool interesting = lower.find(L"cmake") != std::wstring::npos || lower.find(L"cache") != std::wstring::npos ||
                                                 lower.find(L"preset") != std::wstring::npos || lower.find(L"configur") != std::wstring::npos ||
                                                 lower.rfind(L"build.", 0) == 0 || lower.rfind(L"debug.start", 0) == 0 ||
                                                 lower.find(L"startupitem") != std::wstring::npos;
                        if (!interesting) continue;
                        const bool available = c.invoke(L"IsAvailable").as_long() != 0;
                        out << L"    [" << (available ? L"x" : L" ") << L"] " << name << L"\n";
                    } catch (const ComError&) {
                    }
                }
            } catch (const ComError& e) {
                out << L"  HATA: " << e.what << L"\n";
            }
            out << L"\n";
        }
    }
    out << L"Not: Kamil yönetici olarak çalışmıyor; yönetici olarak açılmış bir VS burada görünmez.\n";
    std::ofstream f(ctx.job.report_file, std::ios::binary | std::ios::trunc);
    const std::string text = narrow(out.str());
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    result.ok = static_cast<bool>(f);
    result.report = ctx.job.report_file;
    result.text = L"VS bağlantı raporu hazır";
}

}  // namespace

// ---------------------------------------------------------------------------------------------

VsBridge::VsBridge(HWND target) : target_(target), thread_([this] { run(); }) {}

VsBridge::~VsBridge() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_one();
    thread_.join();
}

uint64_t VsBridge::submit(VsJob job) {
    uint64_t id;
    {
        std::lock_guard lock(mutex_);
        id = next_id_++;
        queue_.emplace_back(id, std::move(job));
    }
    cv_.notify_one();
    return id;
}

bool VsBridge::busy() const {
    std::lock_guard lock(mutex_);
    return running_job_ || !queue_.empty();
}

void VsBridge::run() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    RetryFilter filter;
    IMessageFilter* old_filter = nullptr;
    CoRegisterMessageFilter(&filter, &old_filter);
    std::map<std::wstring, std::wstring> command_cache;

    for (;;) {
        std::pair<uint64_t, VsJob> item;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) break;
            item = std::move(queue_.front());
            queue_.pop_front();
            running_job_ = true;
        }
        const VsJob& job = item.second;
        const Context ctx{target_, item.first, job};
        auto* result = new VsEvent;
        result->type = VsEvent::Type::Finished;
        result->job = item.first;
        result->kind = job.kind;
        const ULONGLONG started = GetTickCount64();
        try {
            if (job.kind == VsJob::Kind::Diagnose) {
                diagnose(ctx, *result);
            } else {
                ctx.progress(vs_name(job.dte_version) + L"'ya bağlanılıyor…");
                Disp dte = find_or_open(ctx);
                switch (job.kind) {
                    case VsJob::Kind::Build:
                    case VsJob::Kind::Rebuild:
                    case VsJob::Kind::Clean: {
                        const std::wstring command = job.kind == VsJob::Kind::Build     ? L"Build.BuildAll"
                                                     : job.kind == VsJob::Kind::Rebuild ? L"Build.RebuildAll"
                                                                                        : L"Build.CleanAll";
                        const std::wstring verb = job.kind == VsJob::Kind::Build ? L"Build " + job.label
                                                  : job.kind == VsJob::Kind::Rebuild ? L"Rebuild " + job.label
                                                                                     : L"Clean " + job.label;
                        result->build = run_build(ctx, dte, command, verb);
                        result->ok = !result->build.failed();
                        result->text = summary_line(verb, result->build, GetTickCount64() - started);
                        break;
                    }
                    case VsJob::Kind::Configure:
                    case VsJob::Kind::Reconfigure: {
                        const bool re = job.kind == VsJob::Kind::Reconfigure;
                        std::wstring command = re ? job.reconfigure_command : job.configure_command;
                        if (command.empty()) {
                            ctx.progress(L"VS komut tablosunda CMake komutu aranıyor…");
                            command = re ? discover_command(dte, {L"deletecache"}, command_cache)
                                         : discover_command(dte, {L"generatecache", L"configurecache"}, command_cache);
                        }
                        if (command.empty())
                            throw ComError{E_FAIL, L"VS'te CMake " + std::wstring(re ? L"önbellek silme" : L"configure") +
                                                       L" komutu bulunamadı. 'Kamil: VS bağlantısını test et' raporundaki adı "
                                                       L"settings.yaml → dev.vs_" + (re ? L"reconfigure" : L"configure") +
                                                       L"_command ayarına yazın."};
                        execute_command(ctx, dte, command, 120'000);
                        activate(dte);
                        result->ok = true;
                        result->text = command + L" gönderildi (ilerleme VS'in Output penceresinde)";
                        break;
                    }
                    case VsJob::Kind::Debug: {
                        if (job.build_first) {
                            const std::wstring verb = L"Build " + job.label;
                            result->build = run_build(ctx, dte, L"Build.BuildAll", verb);
                            if (result->build.failed()) {
                                result->text = summary_line(verb, result->build, GetTickCount64() - started) + L"\nDebug başlatılmadı.";
                                break;
                            }
                        }
                        if (GetFileAttributesW(job.exe.c_str()) == INVALID_FILE_ATTRIBUTES)
                            throw ComError{E_FAIL, L"Çalıştırılabilir dosya yok: " + job.exe};
                        ctx.progress(L"Başlatılıyor ve debugger bağlanıyor…");
                        std::wstring error;
                        auto proc = launch_process(job.exe, job.args, job.cwd, job.console_title, true, &error);
                        if (!proc) throw ComError{E_FAIL, L"Başlatılamadı: " + error};
                        try {
                            attach_debugger(dte, proc->pid);
                        } catch (const ComError& e) {
                            ResumeThread(static_cast<HANDLE>(proc->thread));  // never leave it frozen
                            CloseHandle(static_cast<HANDLE>(proc->thread));
                            CloseHandle(static_cast<HANDLE>(proc->process));
                            throw ComError{e.hr, L"Debugger bağlanamadı (program debugger'sız çalışıyor): " + e.what};
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
            }
        } catch (const ComError& e) {
            result->ok = false;
            result->text = e.what;
        }
        post_owned(target_, WM_KAMIL_VS_EVENT, result);
        std::lock_guard lock(mutex_);
        running_job_ = false;
    }
    CoRegisterMessageFilter(old_filter, nullptr);
    if (SUCCEEDED(hr)) CoUninitialize();
}

}  // namespace kamil
