#pragma once

// Global hotkeys that contain the Windows key (Win+Space, Win+K ...). Windows keeps many Win
// combinations for itself (Win+Space switches the keyboard layout) and RegisterHotKey cannot
// take them over, so these are caught by a low-level keyboard hook. The hook runs on its own
// thread with its own message loop, so a busy UI thread never delays typing system-wide.
//
// A matching key press is swallowed and posted to the target window as WM_HOTKEY (wParam: id),
// exactly like RegisterHotKey would. Note: like every hook, it does not see keys while a window
// of a program running as administrator has the focus (unless Kamil runs as administrator too).

#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "platform/win.h"

namespace kamil {

class KeyHook {
public:
    struct Binding {
        uint32_t mods = 0;  // kModAlt | kModCtrl | kModShift | kModWin
        uint32_t vk = 0;
        WPARAM id = 0;
    };

    ~KeyHook();
    // Replaces all bindings; starts the hook thread on the first binding, stops it when empty.
    // Returns false if the hook could not be installed.
    bool set(std::vector<Binding> bindings, HWND target);

private:
    static LRESULT CALLBACK proc(int code, WPARAM wp, LPARAM lp);
    LRESULT handle(int code, WPARAM wp, LPARAM lp);
    void stop();

    std::mutex mutex_;
    std::vector<Binding> bindings_;
    std::vector<bool> held_;  // per binding: its key is down (key repeat and key up are swallowed)
    HWND target_ = nullptr;
    std::thread thread_;
    DWORD thread_id_ = 0;
    bool installed_ = false;
};

}  // namespace kamil
