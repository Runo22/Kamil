#include "platform/process.h"

#include <atomic>
#include <thread>
#include <vector>

#include "platform/win.h"

namespace kamil {

std::wstring quote_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') out.append(backslashes * 2 + 1, L'\\');
        else out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::optional<LaunchedProcess> launch_process(const std::wstring& exe, const std::wstring& args, const std::wstring& working_dir,
                                              const std::wstring& console_title, bool suspended, std::wstring* error) {
    std::wstring cmd = quote_arg(exe);
    if (!args.empty()) cmd += L" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    std::wstring title = console_title;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpTitle = title.empty() ? nullptr : title.data();
    PROCESS_INFORMATION pi{};
    const DWORD flags = CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT | (suspended ? CREATE_SUSPENDED : 0);
    if (!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, flags, nullptr,
                        working_dir.empty() ? nullptr : working_dir.c_str(), &si, &pi)) {
        if (error) *error = win32_error_message(GetLastError());
        return std::nullopt;
    }
    return LaunchedProcess{pi.hProcess, pi.hThread, pi.dwProcessId};
}

std::optional<ProcessResult> run_capture(const std::wstring& command_line, const std::wstring& working_dir,
                                         uint32_t timeout_ms, bool merge_stderr) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return std::nullopt;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    HANDLE null_err = merge_stderr ? nullptr
                                   : CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = null_in;
    si.hStdOutput = write_end;
    si.hStdError = merge_stderr ? write_end : null_err;

    std::vector<wchar_t> cmd(command_line.begin(), command_line.end());
    cmd.push_back(L'\0');
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                   nullptr, working_dir.empty() ? nullptr : working_dir.c_str(), &si, &pi);
    CloseHandle(write_end);
    if (null_in != INVALID_HANDLE_VALUE) CloseHandle(null_in);
    if (null_err && null_err != INVALID_HANDLE_VALUE) CloseHandle(null_err);
    if (!ok) {
        CloseHandle(read_end);
        return std::nullopt;
    }

    ProcessResult result;
    std::atomic<bool> finished{false};
    std::atomic<HANDLE> reader_handle{nullptr};
    // Read on a helper thread so a silent, hanging child cannot block us past the timeout.
    std::thread reader([&] {
        reader_handle = OpenThread(THREAD_TERMINATE, FALSE, GetCurrentThreadId());  // for CancelSynchronousIo
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(read_end, buf, sizeof(buf), &n, nullptr) && n > 0) {
            if (result.output.size() < (8u << 20)) result.output.append(buf, n);
        }
        finished = true;
    });
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_TIMEOUT) {
        result.timed_out = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exit_code = static_cast<int>(code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    // Normally the pipe closes with the process. A grandchild that inherited the write end could
    // keep it open: give the reader a moment to drain, then cancel its blocking read.
    for (int i = 0; i < 50 && !finished; ++i) Sleep(10);
    while (!finished) {
        if (HANDLE h = reader_handle.load()) CancelSynchronousIo(h);
        Sleep(5);
    }
    reader.join();
    if (HANDLE h = reader_handle.load()) CloseHandle(h);
    CloseHandle(read_end);
    return result;
}

}  // namespace kamil
