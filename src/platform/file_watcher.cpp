#include "platform/file_watcher.h"

#include <algorithm>
#include <cwctype>

namespace kamil {

namespace {
bool iequals(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); });
}
}  // namespace

FileWatcher::~FileWatcher() { stop(); }

bool FileWatcher::start(const std::filesystem::path& dir, std::vector<std::wstring> file_names, HWND target) {
    stop();
    dir_ = dir;
    names_ = std::move(file_names);
    target_ = target;
    dir_handle_ = CreateFileW(dir_.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (dir_handle_ == INVALID_HANDLE_VALUE) return false;
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { run(); });
    return true;
}

void FileWatcher::stop() {
    if (thread_.joinable()) {
        SetEvent(stop_event_);
        thread_.join();
    }
    if (dir_handle_ != INVALID_HANDLE_VALUE) CloseHandle(dir_handle_);
    if (stop_event_) CloseHandle(stop_event_);
    dir_handle_ = INVALID_HANDLE_VALUE;
    stop_event_ = nullptr;
}

void FileWatcher::run() {
    alignas(DWORD) BYTE buffer[16 * 1024];
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (;;) {
        ResetEvent(ov.hEvent);
        if (!ReadDirectoryChangesW(dir_handle_, buffer, sizeof(buffer), FALSE,
                                   FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                                   nullptr, &ov, nullptr))
            break;
        HANDLE handles[] = {ov.hEvent, stop_event_};
        const DWORD w = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0) {
            CancelIoEx(dir_handle_, &ov);
            DWORD ignored = 0;
            GetOverlappedResult(dir_handle_, &ov, &ignored, TRUE);
            break;
        }
        DWORD bytes = 0;
        if (!GetOverlappedResult(dir_handle_, &ov, &bytes, FALSE)) break;
        bool hit = bytes == 0;  // buffer overflow: be safe and report a change
        for (DWORD offset = 0; bytes != 0;) {
            const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
            const std::wstring_view name(info->FileName, info->FileNameLength / sizeof(wchar_t));
            for (const auto& n : names_) hit |= iequals(name, n);
            if (!info->NextEntryOffset) break;
            offset += info->NextEntryOffset;
        }
        if (hit) PostMessageW(target_, WM_KAMIL_FILE_CHANGED, 0, 0);
    }
    CloseHandle(ov.hEvent);
}

}  // namespace kamil
