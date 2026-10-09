// "Kamil: Diagnostics" report, crash dump notice and search timing.

#include <algorithm>
#include <fstream>
#include <sstream>

#include "app/app.h"
#include "core/i18n.h"
#include "core/log.h"
#include "core/text.h"
#include "platform/crash.h"

#ifndef KAMIL_VERSION
#define KAMIL_VERSION "dev"
#endif
#ifndef KAMIL_COMMIT
#define KAMIL_COMMIT ""
#endif

namespace kamil {

namespace {

constexpr size_t kQueryHistory = 512;

std::wstring registry_text(const wchar_t* value) {
    wchar_t buf[256];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", value, RRF_RT_REG_SZ, nullptr, buf, &size) !=
        ERROR_SUCCESS)
        return {};
    return buf;
}

bool elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD size = sizeof(e);
    const bool yes = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size) && e.TokenIsElevated;
    CloseHandle(token);
    return yes;
}

std::wstring ms_text(double ms) {
    wchar_t buf[32];
    swprintf(buf, 32, ms < 10 ? L"%.2f ms" : L"%.0f ms", ms);
    return buf;
}

}  // namespace

void App::record_query_time(double ms) {
    if (query_ms_.size() < kQueryHistory) query_ms_.push_back(static_cast<float>(ms));
    else query_ms_[query_next_++ % kQueryHistory] = static_cast<float>(ms);
    if (ms > 50) log_line("[search] slow query: " + std::to_string(static_cast<int>(ms)) + " ms");
}

void App::report_crashes() {
    const auto dumps = unreported_crash_dumps(paths_.cache_dir);
    if (dumps.empty()) return;
    crashed_last_time_ = true;
    for (const auto& d : dumps) log_line(L"[crash] dump from an earlier run: " + d.wstring());
    notify(loc(L"Kamil closed unexpectedly last time", L"Kamil geçen sefer beklenmedik şekilde kapandı"),
           fmt(loc(L"A crash dump was saved: {}\n\"Kamil: Diagnostics\" lists it; please send it.",
                   L"Çökme dökümü kaydedildi: {}\n\"Kamil: Tanılama\" listeler; lütfen gönderin."),
               dumps.front().filename().wstring()),
           Tray::Balloon::Warning);
}

void App::write_diagnostics() {
    std::wostringstream out;
    auto line = [&](std::wstring_view key, const std::wstring& value) { out << L"  " << key << L": " << value << L"\n"; };
    auto yes_no = [](bool b) { return std::wstring(b ? L"yes" : L"no"); };

    out << L"Kamil diagnostics\n=================\n\n";
    out << L"[Kamil]\n";
    line(L"Version", widen(KAMIL_VERSION) + (std::string_view(KAMIL_COMMIT).empty() ? L"" : L" (" + widen(KAMIL_COMMIT) + L")"));
    line(L"Executable", paths_.exe.wstring());
    line(L"Windows", registry_text(L"ProductName") + L" " + registry_text(L"DisplayVersion") + L" build " + registry_text(L"CurrentBuild"));
    line(L"Running as administrator", yes_no(elevated()));
    line(L"UI language", language() == Lang::Tr ? L"Turkish" : L"English");
    line(L"Hotkey", widen(hotkey_text_) + (hotkey_ok_ ? L"" : L"  (NOT registered: used by another program)"));
    line(L"Portable mode", yes_no(paths_.portable));
    line(L"Settings", paths_.settings_file().wstring());
    line(L"Cache", paths_.cache_dir.wstring());
    line(L"Log", log_file().wstring());
    out << L"\n";

    out << L"[Settings problems]\n";
    if (diagnostics_.empty()) out << L"  none\n";
    for (const auto& d : diagnostics_) out << L"  " << widen(d.to_string()) << L"\n";
    out << L"\n";

    out << L"[Search]\n";
    line(L"Apps", std::to_wstring(apps_.size()));
    size_t cmake = 0;
    for (const auto& r : repos_) cmake += r.cmake;
    line(L"Projects", std::to_wstring(repos_.size()) + L" (" + std::to_wstring(cmake) + L" CMake)");
    if (file_index_) {
        line(L"Indexed files and folders", std::to_wstring(file_index_->size()) + (file_index_->truncated() ? L" (truncated: raise search.max_files)" : L""));
        line(L"Search folders", std::to_wstring(index_roots_) + L", live-watched: " + std::to_wstring(watched_roots_));
        line(L"Last full scan", ms_text(static_cast<double>(files_scan_ms_)));
    } else {
        line(L"Indexed files and folders", L"none (no search.folders, or the first scan is running)");
    }
    if (!query_ms_.empty()) {
        std::vector<float> sorted = query_ms_;
        std::sort(sorted.begin(), sorted.end());
        auto pct = [&](double p) { return static_cast<double>(sorted[std::min(sorted.size() - 1, static_cast<size_t>(p * static_cast<double>(sorted.size())))]); };
        line(L"Search time per keystroke", L"p50 " + ms_text(pct(0.5)) + L", p95 " + ms_text(pct(0.95)) + L", max " +
                                               ms_text(static_cast<double>(sorted.back())) + L"  (" + std::to_wstring(sorted.size()) + L" queries)");
    }
    line(L"Project pre-read (presets + File API)", ms_text(static_cast<double>(warm_ms_.load())));
    out << L"\n";

    out << L"[Visual Studio]\n";
    out << L"  " << (tools_ready_ ? tools_.report : std::wstring(L"tools are still being discovered\n"));
    if (jobs_.empty()) out << L"  Jobs: none\n";
    for (const auto& j : jobs_) out << L"  Job #" << j.id << L": " << job_title(j) << (j.running ? L" (running) " : L" (queued) ") << j.text << L"\n";
    if (!last_job_.text.empty()) line(L"Last job", last_job_.text.substr(0, last_job_.text.find(L'\n')));
    out << L"\n";

    out << L"[Crash dumps]\n";
    const auto dumps = crash_dumps(paths_.cache_dir);
    if (dumps.empty()) out << L"  none\n";
    for (const auto& d : dumps) out << L"  " << d.wstring() << L"\n";

    const auto file = paths_.cache_dir / "diagnostics.txt";
    {
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        const std::string text = "\xEF\xBB\xBF" + narrow(out.str());
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    executor_->post([file] { open_in_editor(file); });
}

}  // namespace kamil
