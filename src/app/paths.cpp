#include "app/paths.h"

#include <shlobj.h>

#include "platform/win.h"

namespace kamil {

namespace {

std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw))) result = raw;
    CoTaskMemFree(raw);
    return result;
}

std::filesystem::path exe_path() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

}  // namespace

Paths resolve_paths() {
    Paths p;
    p.exe = exe_path();
    const auto dir = p.exe.parent_path();
    std::error_code ec;
    if (std::filesystem::is_directory(dir / "data", ec) || std::filesystem::exists(dir / "portable.txt", ec)) {
        p.portable = true;
        p.config_dir = dir / "data";
        p.cache_dir = dir / "data" / "cache";
    } else {
        p.config_dir = known_folder(FOLDERID_RoamingAppData) / "Kamil";
        p.cache_dir = known_folder(FOLDERID_LocalAppData) / "Kamil" / "cache";
    }
    std::filesystem::create_directories(p.config_dir / "schema", ec);
    std::filesystem::create_directories(p.cache_dir, ec);
    return p;
}

}  // namespace kamil
