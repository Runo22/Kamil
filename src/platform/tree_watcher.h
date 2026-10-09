#pragma once

// Watches whole folder trees (ReadDirectoryChangesW, recursive) and posts the names of added and
// removed entries to a window as WM_KAMIL_FS_CHANGES (lParam: std::vector<FsChange>*). Used to
// keep the file index current without rescanning.

#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "platform/win.h"

namespace kamil {

struct FsChange {
    enum class Type { Added, Removed, Overflow };  // Overflow: changes were lost, rescan
    Type type = Type::Added;
    std::wstring path;  // absolute
};

class TreeWatcher {
public:
    ~TreeWatcher();
    // Watches up to 63 roots on one thread. Returns how many roots could be watched (missing or
    // inaccessible folders are skipped).
    size_t start(const std::vector<std::filesystem::path>& roots, HWND target);
    void stop();

private:
    struct Root {
        std::wstring path;
        HANDLE dir = INVALID_HANDLE_VALUE;
        HANDLE event = nullptr;
        OVERLAPPED ov{};
        std::vector<DWORD> buffer;  // DWORD aligned, as ReadDirectoryChangesW requires
    };
    void run();
    bool arm(Root& r);

    std::vector<Root> roots_;
    HWND target_ = nullptr;
    HANDLE stop_event_ = nullptr;
    std::thread thread_;
};

}  // namespace kamil
