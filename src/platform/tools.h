#pragma once

#include <string>
#include <string_view>

namespace kamil {

// External programs Kamil can hand work to. Empty string = not found.
struct Tools {
    std::wstring devenv_2026;
    std::wstring devenv_2022;
    std::wstring code;      // VS Code (Code.exe)
    std::wstring wt;        // Windows Terminal
    std::wstring git;
    std::wstring git_bash;
    std::wstring git_gui;
    std::wstring bash;      // Git for Windows bash.exe
    std::wstring py;        // Python launcher py.exe
    std::wstring pyw;       // pyw.exe (no console)
    std::wstring python;    // python.exe on PATH
    std::wstring pythonw;
    std::wstring explorer;
    std::wstring cmd;
    std::wstring powershell;

    // How Visual Studio was found (or why not), for the VS connection report.
    std::wstring report;

    // "vs2026" / "vs2022"; falls back to the other version when the requested one is missing.
    const std::wstring& devenv(std::string_view preferred) const;
};

// Runs vswhere and probes PATH / well-known install locations. Takes up to a few hundred
// milliseconds: call it off the UI thread. `devenv_override` (dev.devenv_path) wins when it
// exists; its version is taken from the install folder name.
Tools discover_tools(const std::wstring& devenv_override = {});

}  // namespace kamil
