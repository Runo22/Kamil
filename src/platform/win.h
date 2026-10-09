#pragma once

// Common Win32 include and helpers. Only included by Windows-specific sources.

#include <windows.h>
#include <wrl/client.h>

#include <string>

namespace kamil {

using Microsoft::WRL::ComPtr;

// Messages posted to the application window.
enum : UINT {
    WM_KAMIL_SHOW = WM_APP + 1,         // second instance / tray: show the launcher
    WM_KAMIL_TRAY = WM_APP + 2,         // tray icon callback
    WM_KAMIL_APPS_READY = WM_APP + 3,   // lParam: std::vector<Item>* (ownership transferred)
    WM_KAMIL_ICON_READY = WM_APP + 4,   // lParam: IconResult* (ownership transferred)
    WM_KAMIL_FILE_CHANGED = WM_APP + 5, // a watched settings file changed
    WM_KAMIL_NOTIFY = WM_APP + 6,       // lParam: Notification* (ownership transferred)
    WM_KAMIL_TOOLS_READY = WM_APP + 7,  // lParam: Tools* (ownership transferred)
    WM_KAMIL_REPOS_READY = WM_APP + 8,  // lParam: std::vector<RepoInfo>* (ownership transferred)
    WM_KAMIL_GIT_STATUS = WM_APP + 9,   // lParam: RepoState* (ownership transferred)
    WM_KAMIL_VS_EVENT = WM_APP + 10,    // lParam: VsEvent* (ownership transferred)
    WM_KAMIL_FILES_READY = WM_APP + 11, // lParam: FileIndex* (ownership transferred)
    WM_KAMIL_FS_CHANGES = WM_APP + 12,  // lParam: std::vector<FsChange>* (ownership transferred)
    WM_KAMIL_CONSOLE = WM_APP + 13,     // lParam: ConsoleOutput* (ownership transferred)
};

inline constexpr wchar_t kAppWindowClass[] = L"Kamil.AppWindow";
inline constexpr wchar_t kLauncherWindowClass[] = L"Kamil.Launcher";

std::wstring win32_error_message(DWORD error);

// Posts a heap object to a window; deletes it if the post fails.
template <typename T>
void post_owned(HWND hwnd, UINT msg, T* payload) {
    if (!PostMessageW(hwnd, msg, 0, reinterpret_cast<LPARAM>(payload))) delete payload;
}

}  // namespace kamil
