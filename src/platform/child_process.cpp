#include "platform/child_process.h"

#include <thread>
#include <vector>

#include "core/console_buffer.h"
#include "platform/process.h"

namespace kamil {

namespace {

std::wstring to_wide(std::string_view bytes, UINT cp) {
    if (bytes.empty()) return {};
    const int n = MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(), n);
    return out;
}

// The current environment plus variables that make output appear while the program runs.
std::vector<wchar_t> child_environment() {
    std::vector<wchar_t> block;
    if (wchar_t* env = GetEnvironmentStringsW()) {
        const wchar_t* p = env;
        while (*p) {
            const size_t len = wcslen(p);
            block.insert(block.end(), p, p + len + 1);
            p += len + 1;
        }
        FreeEnvironmentStringsW(env);
    }
    for (const wchar_t* extra : {L"PYTHONUNBUFFERED=1", L"PYTHONIOENCODING=utf-8"}) {
        const std::wstring_view e = extra;
        block.insert(block.end(), e.begin(), e.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

}  // namespace

std::wstring decode_output(std::string& carry, std::string_view chunk, bool* oem) {
    std::string data = carry + std::string(chunk);
    carry.clear();
    if (*oem) return to_wide(data, CP_OEMCP);
    const size_t tail = incomplete_utf8_tail(data);
    const std::string_view body(data.data(), data.size() - tail);
    if (is_valid_utf8(body)) {
        carry = data.substr(data.size() - tail);
        return to_wide(body, CP_UTF8);
    }
    *oem = true;
    return to_wide(data, CP_OEMCP);
}

bool ChildProcess::start(uint64_t tab, const std::wstring& exe, const std::wstring& args, const std::wstring& dir, HWND target,
                         std::wstring* error) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
        *error = win32_error_message(GetLastError());
        return false;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = null_in;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    std::wstring line = quote_arg(exe) + (args.empty() ? L"" : L" " + args);
    std::vector<wchar_t> cmd(line.begin(), line.end());
    cmd.push_back(L'\0');
    auto env = child_environment();
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, env.data(),
                                   dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    const DWORD err = GetLastError();
    CloseHandle(write_end);
    if (null_in != INVALID_HANDLE_VALUE) CloseHandle(null_in);
    if (!ok) {
        CloseHandle(read_end);
        *error = win32_error_message(err);
        return false;
    }
    CloseHandle(pi.hThread);
    state_ = std::make_shared<State>();
    state_->process = pi.hProcess;
    state_->pid = pi.dwProcessId;

    std::thread([state = state_, read_end, tab, target] {
        std::string carry;
        bool oem = false;
        char buf[8192];
        DWORD n = 0;
        while (ReadFile(read_end, buf, sizeof(buf), &n, nullptr) && n > 0) {
            auto* out = new ConsoleOutput;
            out->tab = tab;
            out->text = decode_output(carry, std::string_view(buf, n), &oem);
            post_owned(target, WM_KAMIL_CONSOLE, out);
        }
        CloseHandle(read_end);
        WaitForSingleObject(state->process, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(state->process, &code);
        state->running = false;
        auto* out = new ConsoleOutput;
        out->tab = tab;
        out->exited = true;
        out->exit_code = code;
        post_owned(target, WM_KAMIL_CONSOLE, out);
    }).detach();
    return true;
}

void ChildProcess::stop() {
    if (state_ && state_->running) TerminateProcess(state_->process, 1);
}

}  // namespace kamil
