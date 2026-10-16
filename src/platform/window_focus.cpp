#include "platform/window_focus.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <objectarray.h>

#include <cwctype>
#include <map>

namespace kamil {

namespace {

// ---------------------------------------------------------------------------------------------
// Virtual desktops. The documented IVirtualDesktopManager can tell whether a window is on the
// current desktop but can only move windows of the calling process. Moving another program's
// window needs the shell's internal interfaces, which differ between Windows builds; these are
// the Windows 10 (build 19041 - 19045) ones. On other builds the interface query simply fails
// and the window is focused where it is (Windows then switches to its desktop).

const CLSID kClsidImmersiveShell = {0xC2F03A33, 0x21F5, 0x47FA, {0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39}};
const CLSID kClsidVirtualDesktopManagerInternal = {0xC5E0CDCA, 0x7B6E, 0x41B2, {0x9F, 0xC4, 0xD9, 0x39, 0x75, 0xCC, 0x46, 0x7B}};
const IID kIidVirtualDesktopManagerInternalWin10 = {0xF31574D6, 0xB682, 0x4CDC, {0xBD, 0x56, 0x18, 0x27, 0x86, 0x0A, 0xBE, 0xC6}};
const IID kIidApplicationViewCollection = {0x1841C6D7, 0x4F9D, 0x42C0, {0xAF, 0x41, 0x87, 0x47, 0x53, 0x8F, 0x10, 0xE5}};

struct IVirtualDesktopManagerInternalWin10 : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCount(UINT* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveViewToDesktop(IUnknown* view, IUnknown* desktop) = 0;
    virtual HRESULT STDMETHODCALLTYPE CanViewMoveDesktops(IUnknown* view, int* can) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentDesktop(IUnknown** desktop) = 0;
};

struct IApplicationViewCollection : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetViews(IObjectArray** views) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByZOrder(IObjectArray** views) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByAppUserModelId(PCWSTR id, IObjectArray** views) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewForHwnd(HWND hwnd, IUnknown** view) = 0;
};

bool on_current_desktop(HWND hwnd) {
    ComPtr<IVirtualDesktopManager> vdm;
    BOOL on = TRUE;
    if (SUCCEEDED(CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&vdm))))
        vdm->IsWindowOnCurrentVirtualDesktop(hwnd, &on);
    return on != FALSE;
}

HRESULT move_to_current_desktop(HWND hwnd) {
    ComPtr<IServiceProvider> shell;
    HRESULT hr = CoCreateInstance(kClsidImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&shell));
    if (FAILED(hr)) return hr;
    ComPtr<IVirtualDesktopManagerInternalWin10> desktops;
    hr = shell->QueryService(kClsidVirtualDesktopManagerInternal, kIidVirtualDesktopManagerInternalWin10,
                             reinterpret_cast<void**>(desktops.GetAddressOf()));
    if (FAILED(hr)) return hr;
    ComPtr<IApplicationViewCollection> views;
    hr = shell->QueryService(kIidApplicationViewCollection, kIidApplicationViewCollection, reinterpret_cast<void**>(views.GetAddressOf()));
    if (FAILED(hr)) return hr;
    ComPtr<IUnknown> view, current;
    if (FAILED(hr = views->GetViewForHwnd(hwnd, &view))) return hr;
    if (FAILED(hr = desktops->GetCurrentDesktop(&current))) return hr;
    return desktops->MoveViewToDesktop(view.Get(), current.Get());
}

// ---------------------------------------------------------------------------------------------

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring image_name(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
        out.assign(buf, n);
        out = out.substr(out.find_last_of(L"\\/") + 1);
    }
    CloseHandle(h);
    return lower(out);
}

struct Search {
    std::wstring exe;  // lower case
    std::map<DWORD, bool> match;
    HWND found = nullptr;
};

// A window the user would call "the program's window": visible (also when minimized or on
// another virtual desktop), not owned, not a tool window, with a title.
BOOL CALLBACK check_window(HWND hwnd, LPARAM lp) {
    auto& s = *reinterpret_cast<Search*>(lp);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    if (GetWindowTextLengthW(hwnd) == 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    auto it = s.match.find(pid);
    if (it == s.match.end()) it = s.match.emplace(pid, image_name(pid) == s.exe).first;
    if (!it->second) return TRUE;
    s.found = hwnd;  // EnumWindows goes front to back: the first match is the one used last
    return FALSE;
}

// SetForegroundWindow is refused unless the caller "owns" the input; a synthetic Alt press
// gives Kamil (which just had the launcher in front) that right.
void force_foreground(HWND hwnd) {
    if (SetForegroundWindow(hwnd)) return;
    INPUT in[2]{};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = VK_MENU;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
    SetForegroundWindow(hwnd);
}

}  // namespace

HWND find_program_window(std::wstring_view exe_name) {
    if (exe_name.empty()) return nullptr;
    Search s;
    s.exe = lower(std::wstring(exe_name));
    EnumWindows(&check_window, reinterpret_cast<LPARAM>(&s));
    return s.found;
}

bool bring_window_here(HWND hwnd, std::wstring* note) {
    if (!IsWindow(hwnd)) return false;
    if (!on_current_desktop(hwnd)) {
        const HRESULT hr = move_to_current_desktop(hwnd);
        if (FAILED(hr) && note) {
            wchar_t code[16];
            swprintf(code, 16, L"0x%08lX", static_cast<unsigned long>(hr));
            *note = std::wstring(L"virtual desktop could not be changed (") + code + L")";
        }
    }
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    force_foreground(hwnd);
    BringWindowToTop(hwnd);
    return true;
}

std::wstring shortcut_target(const std::wstring& lnk) {
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return {};
    ComPtr<IPersistFile> file;
    if (FAILED(link.As(&file)) || FAILED(file->Load(lnk.c_str(), STGM_READ))) return {};
    wchar_t path[MAX_PATH] = {};
    if (FAILED(link->GetPath(path, MAX_PATH, nullptr, SLGP_RAWPATH))) return {};
    wchar_t expanded[MAX_PATH] = {};
    ExpandEnvironmentStringsW(path, expanded, MAX_PATH);
    return expanded;
}

}  // namespace kamil
