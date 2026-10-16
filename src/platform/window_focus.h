#pragma once

// "Bring the running window here": find the main window of an already running program and show
// it on the current virtual desktop, in front, without moving it to another monitor.

#include <string>
#include <string_view>

#include "platform/win.h"

namespace kamil {

// The most recently used top-level window of a running program whose exe file name is
// `exe_name` (e.g. "Code.exe", case-insensitive). nullptr when it is not running or has no window.
HWND find_program_window(std::wstring_view exe_name);

// Moves `hwnd` to the current virtual desktop if it is on another one, restores it if minimized
// and makes it the foreground window. Its monitor and position stay as they are. `note`
// receives what could not be done (e.g. the virtual desktop could not be changed).
bool bring_window_here(HWND hwnd, std::wstring* note);

// Target of a .lnk shortcut (empty if it is not a file system shortcut).
std::wstring shortcut_target(const std::wstring& lnk);

}  // namespace kamil
