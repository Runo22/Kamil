#include "platform/tray.h"

#include <shellapi.h>

#include <algorithm>
#include <cwchar>

namespace kamil {

namespace {
constexpr UINT kTrayId = 1;

template <size_t N>
void copy_text(wchar_t (&dst)[N], const std::wstring& src) {
    const size_t n = std::min(src.size(), N - 1);
    wmemcpy(dst, src.c_str(), n);
    dst[n] = L'\0';
}
}  // namespace

UINT Tray::taskbar_created_message() {
    static const UINT msg = RegisterWindowMessageW(L"TaskbarCreated");
    return msg;
}

NOTIFYICONDATAW Tray::base() const {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner_;
    nid.uID = kTrayId;
    return nid;
}

bool Tray::create(HWND owner, HICON icon, const std::wstring& tooltip) {
    owner_ = owner;
    icon_ = icon;
    tooltip_ = tooltip;
    NOTIFYICONDATAW nid = base();
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_KAMIL_TRAY;
    nid.hIcon = icon_;
    copy_text(nid.szTip, tooltip_);
    added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (added_) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
    return added_;
}

void Tray::destroy() {
    if (!added_) return;
    NOTIFYICONDATAW nid = base();
    Shell_NotifyIconW(NIM_DELETE, &nid);
    added_ = false;
}

void Tray::recreate() {
    added_ = false;
    create(owner_, icon_, tooltip_);
}

void Tray::set_tooltip(const std::wstring& tooltip) {
    tooltip_ = tooltip;
    if (!added_) return;
    NOTIFYICONDATAW nid = base();
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    copy_text(nid.szTip, tooltip_);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Tray::balloon(const std::wstring& title, const std::wstring& text, Balloon kind) {
    if (!added_) return;
    NOTIFYICONDATAW nid = base();
    nid.uFlags = NIF_INFO;
    copy_text(nid.szInfoTitle, title);
    copy_text(nid.szInfo, text);
    nid.dwInfoFlags = kind == Balloon::Error ? NIIF_ERROR : kind == Balloon::Warning ? NIIF_WARNING : NIIF_INFO;
    nid.dwInfoFlags |= NIIF_RESPECT_QUIET_TIME;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

}  // namespace kamil
