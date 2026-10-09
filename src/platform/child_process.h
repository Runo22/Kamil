#pragma once

// A program started by Kamil whose stdout + stderr go into a console tab instead of a console
// window. Output chunks and the exit are posted to a window as WM_KAMIL_CONSOLE
// (lParam: ConsoleOutput*). The reader thread owns the pipe; the object can go away while the
// program keeps running.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "platform/win.h"

namespace kamil {

struct ConsoleOutput {
    uint64_t tab = 0;
    std::wstring text;
    bool exited = false;
    unsigned long exit_code = 0;
};

class ChildProcess {
public:
    // Returns false (with `error`) if the program cannot start.
    bool start(uint64_t tab, const std::wstring& exe, const std::wstring& args, const std::wstring& dir, HWND target,
               std::wstring* error);
    void stop();  // ends the program (it was started by Kamil for this tab)
    bool running() const { return state_ && state_->running.load(); }
    unsigned long pid() const { return state_ ? state_->pid : 0; }

private:
    struct State {
        HANDLE process = nullptr;
        unsigned long pid = 0;
        std::atomic<bool> running{true};
        ~State() {
            if (process) CloseHandle(process);
        }
    };
    std::shared_ptr<State> state_;
};

// Decodes program output: UTF-8 when it is valid UTF-8, else the OEM code page (what console
// programs on Turkish / English Windows write). `carry` keeps an incomplete UTF-8 sequence for
// the next chunk; `oem` sticks once a chunk was not UTF-8.
std::wstring decode_output(std::string& carry, std::string_view chunk, bool* oem);

}  // namespace kamil
