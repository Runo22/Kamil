#pragma once

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "core/item.h"
#include "platform/win.h"

namespace kamil {

enum class LaunchMode { Normal, Admin, OpenLocation };

// Runs shell operations on a dedicated STA thread so that slow shell extensions or unreachable
// network paths never block the UI thread.
class Executor {
public:
    Executor();
    ~Executor();
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    void post(std::function<void()> task);

private:
    void run();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    bool stop_ = false;
    std::thread thread_;
};

struct Notification {
    enum class Level { Info, Warning, Error } level = Level::Info;
    std::wstring title;
    std::wstring text;
};

// Executes an item. Must run on an STA thread (the Executor). Returns an error text, empty on success.
std::wstring launch_item(const Item& item, LaunchMode mode);

// Starts a program with arguments (quote them with quote_arg) in `dir`. Returns an error text.
// `admin` uses the "runas" verb (UAC prompt); `verb` overrides it (e.g. "edit", "openas").
std::wstring run_program(const std::wstring& exe, const std::wstring& args, const std::wstring& dir = {}, bool admin = false,
                         const wchar_t* verb = nullptr);

// Opens a text file in the user's editor: the .yaml association, otherwise Notepad.
std::wstring open_in_editor(const std::filesystem::path& file);
std::wstring open_folder(const std::filesystem::path& folder);

bool set_autostart(bool enabled, const std::filesystem::path& exe);
bool copy_to_clipboard(HWND owner, const std::wstring& text);
std::wstring read_clipboard_text(HWND owner);

}  // namespace kamil
