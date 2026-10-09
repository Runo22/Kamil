#include "platform/tools.h"

#include <algorithm>
#include <cwctype>

#include "core/text.h"
#include "platform/process.h"
#include "platform/win.h"

namespace kamil {

namespace {

std::wstring env(const wchar_t* name) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

bool exists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring first_existing(std::initializer_list<std::wstring> candidates) {
    for (const auto& c : candidates)
        if (exists(c)) return c;
    return {};
}

std::wstring search_path(const wchar_t* exe) {
    wchar_t buf[MAX_PATH];
    const DWORD n = SearchPathW(nullptr, exe, nullptr, MAX_PATH, buf, nullptr);
    return n && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

std::wstring parent(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring registry_string(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[MAX_PATH];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

std::wstring vswhere_devenv(const std::wstring& vswhere, const wchar_t* range, std::wstring& report) {
    if (vswhere.empty()) return {};
    const std::wstring cmd = quote_arg(vswhere) + L" -latest -prerelease -products * -version " + range +
                             L" -property productPath -utf8";
    auto r = run_capture(cmd, {}, 8000);
    if (!r) {
        report += std::wstring(L"  vswhere ") + range + L": could not start\n";
        return {};
    }
    std::wstring out = widen(r->output);
    out.erase(std::remove(out.begin(), out.end(), L'\uFEFF'), out.end());  // -utf8 output may start with a BOM
    std::wstring_view view = trim(std::wstring_view(out));
    std::wstring path(trim(view.substr(0, view.find_first_of(L"\r\n"))));
    report += std::wstring(L"  vswhere ") + range + L": exit " + std::to_wstring(r->exit_code) + L", \"" + path + L"\"" +
              (path.empty() || exists(path) ? L"" : L" (file missing)") + L"\n";
    return exists(path) ? path : std::wstring();
}

// <ProgramFiles>\Microsoft Visual Studio\<folder>\<edition>\Common7\IDE\devenv.exe
std::wstring scan_install_folders(const wchar_t* folder) {
    static const wchar_t* const kEditions[] = {L"Enterprise", L"Professional", L"Community", L"Insiders", L"Preview"};
    for (const std::wstring& base : {env(L"ProgramFiles"), env(L"ProgramW6432"), env(L"ProgramFiles(x86)")}) {
        if (base.empty()) continue;
        const std::wstring root = base + L"\\Microsoft Visual Studio\\" + folder + L"\\";
        for (const wchar_t* edition : kEditions) {
            const std::wstring p = root + edition + L"\\Common7\\IDE\\devenv.exe";
            if (exists(p)) return p;
        }
        // Any other edition folder name.
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((root + L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::wstring found;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            const std::wstring p = root + fd.cFileName + L"\\Common7\\IDE\\devenv.exe";
            if (exists(p)) found = p;
        } while (found.empty() && FindNextFileW(h, &fd));
        FindClose(h);
        if (!found.empty()) return found;
    }
    return {};
}

}  // namespace

const std::wstring& Tools::devenv(std::string_view preferred) const {
    if (preferred == "vs2022") return !devenv_2022.empty() ? devenv_2022 : devenv_2026;
    return !devenv_2026.empty() ? devenv_2026 : devenv_2022;
}

Tools discover_tools(const std::wstring& devenv_override) {
    Tools t;
    const std::wstring windir = env(L"WINDIR");
    t.explorer = windir + L"\\explorer.exe";
    t.cmd = windir + L"\\System32\\cmd.exe";
    t.powershell = windir + L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";

    const std::wstring vswhere = first_existing({env(L"ProgramFiles(x86)") + L"\\Microsoft Visual Studio\\Installer\\vswhere.exe",
                                                 env(L"ProgramFiles") + L"\\Microsoft Visual Studio\\Installer\\vswhere.exe"});
    t.report = L"vswhere: " + (vswhere.empty() ? std::wstring(L"not found") : vswhere) + L"\n";
    t.devenv_2026 = vswhere_devenv(vswhere, L"[18.0,19.0)", t.report);
    t.devenv_2022 = vswhere_devenv(vswhere, L"[17.0,18.0)", t.report);
    if (t.devenv_2026.empty()) t.devenv_2026 = scan_install_folders(L"18");
    if (t.devenv_2022.empty()) t.devenv_2022 = scan_install_folders(L"2022");
    if (!devenv_override.empty()) {
        if (exists(devenv_override)) {
            // ...\Microsoft Visual Studio\2022\<edition>\... is VS 2022; anything else is treated as VS 2026.
            std::wstring lower = devenv_override;
            for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
            (lower.find(L"\\2022\\") != std::wstring::npos ? t.devenv_2022 : t.devenv_2026) = devenv_override;
            t.report += L"dev.devenv_path: " + devenv_override + L"\n";
        } else {
            t.report += L"dev.devenv_path: " + devenv_override + L" (file missing, ignored)\n";
        }
    }
    t.report += L"VS 2026: " + (t.devenv_2026.empty() ? std::wstring(L"not found") : t.devenv_2026) + L"\n";
    t.report += L"VS 2022: " + (t.devenv_2022.empty() ? std::wstring(L"not found") : t.devenv_2022) + L"\n";

    std::wstring code_cmd = search_path(L"code.cmd");  // ...\Microsoft VS Code\bin\code.cmd
    t.code = first_existing({code_cmd.empty() ? std::wstring() : parent(parent(code_cmd)) + L"\\Code.exe",
                             env(L"LOCALAPPDATA") + L"\\Programs\\Microsoft VS Code\\Code.exe",
                             env(L"ProgramFiles") + L"\\Microsoft VS Code\\Code.exe"});

    t.wt = search_path(L"wt.exe");
    if (t.wt.empty()) t.wt = first_existing({env(L"LOCALAPPDATA") + L"\\Microsoft\\WindowsApps\\wt.exe"});

    t.git = search_path(L"git.exe");
    std::wstring git_root;
    if (!t.git.empty()) {
        git_root = parent(parent(t.git));  // <root>\cmd\git.exe or <root>\bin\git.exe
    } else {
        git_root = registry_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GitForWindows", L"InstallPath");
        if (!git_root.empty()) t.git = first_existing({git_root + L"\\cmd\\git.exe", git_root + L"\\bin\\git.exe"});
    }
    if (!git_root.empty()) {
        t.git_bash = first_existing({git_root + L"\\git-bash.exe"});
        t.git_gui = first_existing({git_root + L"\\cmd\\git-gui.exe"});
        t.bash = first_existing({git_root + L"\\bin\\bash.exe", git_root + L"\\usr\\bin\\bash.exe"});
    }
    t.py = search_path(L"py.exe");
    t.pyw = search_path(L"pyw.exe");
    t.python = search_path(L"python.exe");
    if (t.python.find(L"WindowsApps") != std::wstring::npos) t.python.clear();  // Store installer stub, not Python
    t.pythonw = search_path(L"pythonw.exe");
    return t;
}

}  // namespace kamil
