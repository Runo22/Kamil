#pragma once

// Drives Visual Studio 2022/2026 over COM (DTE) for CMake "Open Folder" projects:
// find or open the instance that has the project folder open, trigger Build/Configure,
// wait for the build to finish and read its errors, and debug by "launch suspended + attach".
//
// All COM calls run on one dedicated STA thread; jobs are queued and results are posted to the
// application window as WM_KAMIL_VS_EVENT (lParam: VsEvent*).

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "core/cmake.h"
#include "platform/win.h"

namespace kamil {

struct VsJob {
    enum class Kind { Build, Rebuild, Clean, Configure, Reconfigure, Debug, Diagnose };
    Kind kind = Kind::Build;
    std::wstring folder;       // project root (Open Folder)
    std::wstring devenv;       // devenv.exe of the wanted version
    std::wstring dte_version;  // "18.0" (VS 2026) / "17.0" (VS 2022)
    std::wstring label;        // shown in progress texts, e.g. "x64-debug"

    // Debug
    bool build_first = true;
    std::wstring exe, args, cwd, console_title;

    // Command names for Configure / Reconfigure; empty: discover from the DTE command table.
    std::wstring configure_command, reconfigure_command;

    std::filesystem::path report_file;  // Diagnose
};

struct VsEvent {
    enum class Type { Progress, Finished };
    Type type = Type::Progress;
    uint64_t job = 0;
    VsJob::Kind kind = VsJob::Kind::Build;
    std::wstring text;    // progress / result line
    bool ok = false;
    BuildSummary build;   // for build-like jobs
    std::filesystem::path report;
};

class VsBridge {
public:
    explicit VsBridge(HWND target);
    ~VsBridge();
    VsBridge(const VsBridge&) = delete;
    VsBridge& operator=(const VsBridge&) = delete;

    uint64_t submit(VsJob job);
    bool busy() const;

private:
    void run();

    HWND target_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::pair<uint64_t, VsJob>> queue_;
    uint64_t next_id_ = 1;
    bool running_job_ = false;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace kamil
