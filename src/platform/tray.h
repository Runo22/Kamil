#pragma once

#include <string>

#include "platform/win.h"

#include <shellapi.h>

namespace kamil {

class Tray {
public:
    bool create(HWND owner, HICON icon, const std::wstring& tooltip);
    void destroy();
    void recreate();  // after Explorer restarts (TaskbarCreated)
    void set_tooltip(const std::wstring& tooltip);

    enum class Balloon { Info, Warning, Error };
    void balloon(const std::wstring& title, const std::wstring& text, Balloon kind = Balloon::Info);

    static UINT taskbar_created_message();

private:
    NOTIFYICONDATAW base() const;

    HWND owner_ = nullptr;
    HICON icon_ = nullptr;
    std::wstring tooltip_;
    bool added_ = false;
};

}  // namespace kamil
