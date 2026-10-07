#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace kamil {

// The last choices per project, remembered across restarts ("son seçili plan").
struct ProjectChoice {
    std::string preset;
    std::string target;
    std::string args;      // template, e.g. "--port {com} --baud 115200"
    std::string com_port;  // "COM7"
    std::string com_hwid;  // "VID_0403&PID_6001" (to find the device again if its number changes)
};

class ProjectStateStore {
public:
    ProjectChoice get(const std::wstring& root) const;
    void set(const std::wstring& root, const ProjectChoice& choice);

    bool load(const std::filesystem::path& file);
    bool save(const std::filesystem::path& file) const;

private:
    std::map<std::wstring, ProjectChoice> map_;  // key: folded root path
};

// Replaces {com}, {preset}, {target}, {config} and {project} in a run argument template.
// Unknown placeholders stay as they are.
std::string expand_placeholders(std::string_view tmpl, const std::map<std::string, std::string>& values);

}  // namespace kamil
