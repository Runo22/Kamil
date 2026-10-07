#include "platform/system_theme.h"

#include <dwmapi.h>

#include "platform/win.h"

namespace kamil {

namespace {
bool read_dword(HKEY root, const wchar_t* path, const wchar_t* name, DWORD* out) {
    DWORD size = sizeof(DWORD);
    return RegGetValueW(root, path, name, RRF_RT_REG_DWORD, nullptr, out, &size) == ERROR_SUCCESS;
}
}  // namespace

SystemTheme read_system_theme() {
    SystemTheme t;
    DWORD light = 0;
    if (read_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                   L"AppsUseLightTheme", &light))
        t.apps_dark = light == 0;

    DWORD abgr = 0;
    if (read_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", &abgr)) {
        const uint32_t r = abgr & 0xFF, g = (abgr >> 8) & 0xFF, b = (abgr >> 16) & 0xFF;
        t.accent_rgb = (r << 16) | (g << 8) | b;
    } else {
        DWORD argb = 0;
        BOOL opaque = FALSE;
        if (SUCCEEDED(DwmGetColorizationColor(&argb, &opaque))) t.accent_rgb = argb & 0xFFFFFF;
    }
    return t;
}

bool is_windows11() {
    // GetVersionEx lies without a manifest entry for newer versions; RtlGetVersion does not.
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    static const bool result = [] {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr;
        if (!fn) return false;
        OSVERSIONINFOW v{};
        v.dwOSVersionInfoSize = sizeof(v);
        if (fn(&v) != 0) return false;
        return v.dwMajorVersion > 10 || (v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000);
    }();
    return result;
}

}  // namespace kamil
