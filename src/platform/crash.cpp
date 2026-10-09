#include "platform/crash.h"

#include "platform/win.h"  // before dbghelp.h

#include <dbghelp.h>

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <fstream>

namespace kamil {

namespace {

wchar_t g_dir[MAX_PATH];

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

struct DumpRequest {
    EXCEPTION_POINTERS* exception;
    DWORD thread_id;
};

// Runs on its own thread: the crashing thread may have no stack left (stack overflow).
DWORD WINAPI write_dump(void* param) {
    const auto* req = static_cast<DumpRequest*>(param);
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (!dbghelp) return 1;
    auto fn = reinterpret_cast<MiniDumpWriteDumpFn>(reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
    if (!fn) return 1;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t path[MAX_PATH + 64];
    swprintf(path, MAX_PATH + 64, L"%ls\\crash-%04u%02u%02u-%02u%02u%02u.dmp", g_dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 1;
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = req->thread_id;
    info.ExceptionPointers = req->exception;
    info.ClientPointers = FALSE;
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory);
    fn(GetCurrentProcess(), GetCurrentProcessId(), file, type, req->exception ? &info : nullptr, nullptr, nullptr);
    CloseHandle(file);
    return 0;
}

void dump_now(EXCEPTION_POINTERS* exception) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1) != 0) return;  // a second crash while dumping: give up
    DumpRequest req{exception, GetCurrentThreadId()};
    HANDLE thread = CreateThread(nullptr, 0, &write_dump, &req, 0, nullptr);
    if (thread) {
        WaitForSingleObject(thread, 60'000);
        CloseHandle(thread);
    }
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS* exception) {
    dump_now(exception);
    return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] void on_terminate() {
    dump_now(nullptr);
    std::abort();
}

}  // namespace

void install_crash_handler(const std::filesystem::path& dump_dir) {
    std::error_code ec;
    std::filesystem::create_directories(dump_dir, ec);
    lstrcpynW(g_dir, dump_dir.c_str(), MAX_PATH);
    SetUnhandledExceptionFilter(&on_unhandled);
    std::set_terminate(&on_terminate);
}

std::vector<std::filesystem::path> crash_dumps(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const auto name = it->path().filename().wstring();
        if (name.rfind(L"crash-", 0) == 0 && it->path().extension() == L".dmp") out.push_back(it->path());
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.filename() > b.filename(); });  // names sort by time
    return out;
}

std::vector<std::filesystem::path> unreported_crash_dumps(const std::filesystem::path& dir) {
    auto dumps = crash_dumps(dir);
    std::error_code ec;
    for (size_t i = 5; i < dumps.size(); ++i) std::filesystem::remove(dumps[i], ec);
    if (dumps.size() > 5) dumps.resize(5);

    const auto marker = dir / "crash-reported.txt";
    std::wstring last;  // file name of the newest dump already reported
    {
        std::wifstream in(marker);
        std::getline(in, last);
    }
    std::vector<std::filesystem::path> fresh;
    for (const auto& d : dumps)
        if (d.filename().wstring() > last) fresh.push_back(d);
    if (!fresh.empty()) {
        std::wofstream out(marker, std::ios::trunc);
        out << fresh.front().filename().wstring();
    }
    return fresh;
}

}  // namespace kamil
