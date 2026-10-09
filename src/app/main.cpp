#include <cwchar>

#include "app/app.h"
#include "platform/crash.h"
#include "platform/win.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmd_line, int) {
    const bool autostart = cmd_line && std::wcsstr(cmd_line, L"--autostart") != nullptr;

    // One Kamil per user session: a second start just opens the launcher of the running one.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\Kamil.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kamil::kAppWindowClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            AllowSetForegroundWindow(pid);
            if (!autostart) PostMessageW(existing, kamil::WM_KAMIL_SHOW, 0, 0);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    kamil::install_crash_handler(kamil::resolve_paths().cache_dir);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // also set in the manifest
    if (FAILED(OleInitialize(nullptr))) return 1;  // STA + clipboard

    int rc = 0;
    {
        kamil::App app;
        rc = app.run(instance, autostart);
    }

    OleUninitialize();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return rc;
}
