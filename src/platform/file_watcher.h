#pragma once

#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "platform/win.h"

namespace kamil {

// Watches one directory (non-recursive) and posts WM_KAMIL_FILE_CHANGED to `target` whenever one
// of the given file names is written, created or renamed into place.
class FileWatcher {
public:
    ~FileWatcher();
    bool start(const std::filesystem::path& dir, std::vector<std::wstring> file_names, HWND target);
    void stop();

private:
    void run();

    std::filesystem::path dir_;
    std::vector<std::wstring> names_;
    HWND target_ = nullptr;
    HANDLE dir_handle_ = INVALID_HANDLE_VALUE;
    HANDLE stop_event_ = nullptr;
    std::thread thread_;
};

}  // namespace kamil
