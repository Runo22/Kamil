#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace kamil {

struct ProcessResult {
    int exit_code = -1;
    bool timed_out = false;
    std::string output;  // stdout (+ stderr when merged), raw bytes
};

// Runs a console program without showing a window and captures its output. `command_line` is
// passed to CreateProcess as is (quote arguments yourself). Returns nullopt if it cannot start.
std::optional<ProcessResult> run_capture(const std::wstring& command_line, const std::wstring& working_dir,
                                         uint32_t timeout_ms, bool merge_stderr = false);

struct LaunchedProcess {
    void* process = nullptr;  // HANDLE, owned by the caller (CloseHandle)
    void* thread = nullptr;   // HANDLE of the main thread, owned by the caller
    unsigned long pid = 0;
};

// Starts `exe` with an argument string. Console programs get their own console window whose
// title is `console_title`. With `suspended` the main thread is not started yet (attach a
// debugger, then ResumeThread).
std::optional<LaunchedProcess> launch_process(const std::wstring& exe, const std::wstring& args, const std::wstring& working_dir,
                                              const std::wstring& console_title, bool suspended, std::wstring* error);

// Quotes one argument following the CommandLineToArgvW rules.
std::wstring quote_arg(const std::wstring& arg);

}  // namespace kamil
