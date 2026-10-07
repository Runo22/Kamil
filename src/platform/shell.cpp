#include "platform/shell.h"

#include <shellapi.h>
#include <shlobj.h>

#include "core/text.h"

namespace kamil {

std::wstring win32_error_message(DWORD error) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, error, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring msg = n ? std::wstring(trim(std::wstring_view(buf, n))) : L"Hata " + std::to_wstring(error);
    LocalFree(buf);
    return msg;
}

// ---------------------------------------------------------------------------------------------

Executor::Executor() : thread_([this] { run(); }) {}

Executor::~Executor() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_one();
    thread_.join();
}

void Executor::post(std::function<void()> task) {
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
}

void Executor::run() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (queue_.empty()) break;  // stop requested and drained
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();
    }
    if (SUCCEEDED(hr)) CoUninitialize();
}

// ---------------------------------------------------------------------------------------------

namespace {

std::wstring shell_execute(const std::wstring& file, const wchar_t* verb, const std::wstring& params = {},
                           const std::wstring& dir = {}) {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = verb;
    sei.lpFile = file.c_str();
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) return {};
    const DWORD err = GetLastError();
    if (err == ERROR_CANCELLED) return {};  // user declined the UAC prompt
    return win32_error_message(err);
}

std::wstring reveal_in_explorer(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (!pidl) return L"Konum bulunamadı: " + path;
    const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ILFree(pidl);
    return SUCCEEDED(hr) ? std::wstring{} : win32_error_message(static_cast<DWORD>(hr));
}

}  // namespace

std::wstring launch_item(const Item& item, LaunchMode mode) {
    switch (mode) {
        case LaunchMode::OpenLocation:
            if (item.path.empty()) return L"Bu öğenin dosya konumu bilinmiyor.";
            return reveal_in_explorer(item.path);
        case LaunchMode::Admin:
            return shell_execute(item.path.empty() ? item.target : item.path, L"runas");
        case LaunchMode::Normal:
            break;
    }
    return shell_execute(item.target, nullptr);
}

std::wstring open_in_editor(const std::filesystem::path& file) {
    const std::wstring err = shell_execute(file.wstring(), L"open");
    if (err.empty()) return {};
    // .yaml often has no association on a fresh machine.
    return shell_execute(L"notepad.exe", nullptr, L"\"" + file.wstring() + L"\"");
}

std::wstring open_folder(const std::filesystem::path& folder) { return shell_execute(folder.wstring(), L"open"); }

bool set_autostart(bool enabled, const std::filesystem::path& exe) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE | KEY_QUERY_VALUE,
                      &key) != ERROR_SUCCESS)
        return false;
    LSTATUS st;
    if (enabled) {
        const std::wstring cmd = L"\"" + exe.wstring() + L"\" --autostart";
        st = RegSetValueExW(key, L"Kamil", 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(key, L"Kamil");
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return st == ERROR_SUCCESS;
}

bool copy_to_clipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (mem) {
        if (void* p = GlobalLock(mem)) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(mem);
            ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        }
        if (!ok) GlobalFree(mem);
    }
    CloseClipboard();
    return ok;
}

std::wstring read_clipboard_text(HWND owner) {
    std::wstring out;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(owner)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

}  // namespace kamil
