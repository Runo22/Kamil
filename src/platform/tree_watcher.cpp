#include "platform/tree_watcher.h"

namespace kamil {

TreeWatcher::~TreeWatcher() { stop(); }

size_t TreeWatcher::start(const std::vector<std::filesystem::path>& roots, HWND target) {
    stop();
    target_ = target;
    for (const auto& path : roots) {
        if (roots_.size() >= MAXIMUM_WAIT_OBJECTS - 1) break;
        Root r;
        r.path = path.wstring();
        while (r.path.size() > 3 && (r.path.back() == L'\\' || r.path.back() == L'/')) r.path.pop_back();
        r.dir = CreateFileW(r.path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (r.dir == INVALID_HANDLE_VALUE) continue;
        r.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        r.buffer.resize(16 * 1024);  // 64 KB: the limit for network folders
        roots_.push_back(std::move(r));
    }
    if (roots_.empty()) return 0;
    for (auto& r : roots_) r.ov.hEvent = r.event;  // addresses are stable now
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { run(); });
    return roots_.size();
}

void TreeWatcher::stop() {
    if (thread_.joinable()) {
        SetEvent(stop_event_);
        thread_.join();
    }
    for (auto& r : roots_) {
        if (r.dir != INVALID_HANDLE_VALUE) CloseHandle(r.dir);
        if (r.event) CloseHandle(r.event);
    }
    roots_.clear();
    if (stop_event_) CloseHandle(stop_event_);
    stop_event_ = nullptr;
}

bool TreeWatcher::arm(Root& r) {
    ResetEvent(r.event);
    return ReadDirectoryChangesW(r.dir, r.buffer.data(), static_cast<DWORD>(r.buffer.size() * sizeof(DWORD)), TRUE,
                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME, nullptr, &r.ov, nullptr) != FALSE;
}

void TreeWatcher::run() {
    std::vector<HANDLE> handles;
    std::vector<size_t> armed;  // index into roots_ for handles[i]
    for (size_t i = 0; i < roots_.size(); ++i) {
        if (!arm(roots_[i])) continue;
        handles.push_back(roots_[i].event);
        armed.push_back(i);
    }
    handles.push_back(stop_event_);
    while (handles.size() > 1) {
        const DWORD w = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
        if (w < WAIT_OBJECT_0 || w >= WAIT_OBJECT_0 + handles.size() - 1) break;  // stop event or failure
        const size_t slot = w - WAIT_OBJECT_0;
        Root& r = roots_[armed[slot]];
        DWORD bytes = 0;
        auto* changes = new std::vector<FsChange>;
        if (!GetOverlappedResult(r.dir, &r.ov, &bytes, FALSE) || bytes == 0) {
            changes->push_back({FsChange::Type::Overflow, r.path});  // buffer overflow or error: changes were lost
        } else {
            const auto* base = reinterpret_cast<const BYTE*>(r.buffer.data());
            for (DWORD offset = 0;;) {
                const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + offset);
                std::wstring path = r.path + (r.path.back() == L'\\' ? L"" : L"\\") +
                                    std::wstring(info->FileName, info->FileNameLength / sizeof(wchar_t));
                switch (info->Action) {
                    case FILE_ACTION_ADDED:
                    case FILE_ACTION_RENAMED_NEW_NAME: changes->push_back({FsChange::Type::Added, std::move(path)}); break;
                    case FILE_ACTION_REMOVED:
                    case FILE_ACTION_RENAMED_OLD_NAME: changes->push_back({FsChange::Type::Removed, std::move(path)}); break;
                    default: break;
                }
                if (!info->NextEntryOffset) break;
                offset += info->NextEntryOffset;
            }
        }
        if (changes->empty()) delete changes;
        else post_owned(target_, WM_KAMIL_FS_CHANGES, changes);
        if (!arm(r)) {  // the folder went away: stop watching it
            handles.erase(handles.begin() + static_cast<std::ptrdiff_t>(slot));
            armed.erase(armed.begin() + static_cast<std::ptrdiff_t>(slot));
        }
    }
    for (size_t i : armed) {
        CancelIoEx(roots_[i].dir, &roots_[i].ov);
        DWORD ignored = 0;
        GetOverlappedResult(roots_[i].dir, &roots_[i].ov, &ignored, TRUE);
    }
}

}  // namespace kamil
