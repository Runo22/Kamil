#include "core/project_state.h"

#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#include "core/text.h"

namespace kamil {

namespace {

// Tabs and newlines cannot appear in the TSV fields.
std::string clean(std::string s) {
    for (char& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

}  // namespace

ProjectChoice ProjectStateStore::get(const std::wstring& root) const {
    auto it = map_.find(fold(root));
    return it == map_.end() ? ProjectChoice{} : it->second;
}

void ProjectStateStore::set(const std::wstring& root, const ProjectChoice& choice) { map_[fold(root)] = choice; }

bool ProjectStateStore::load(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    map_.clear();
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> cols;
        std::string_view rest = line;
        for (;;) {
            const size_t tab = rest.find('\t');
            cols.emplace_back(rest.substr(0, tab));
            if (tab == std::string_view::npos) break;
            rest.remove_prefix(tab + 1);
        }
        cols.resize(6);
        if (cols[0].empty()) continue;
        map_[fold(widen(cols[0]))] = ProjectChoice{cols[1], cols[2], cols[3], cols[4], cols[5]};
    }
    return true;
}

bool ProjectStateStore::save(const std::filesystem::path& file) const {
    std::ostringstream out;
    out << "# Kamil project choices (automatic): root \\t preset \\t target \\t arguments \\t COM \\t hardware id\n";
    for (const auto& [root, c] : map_)
        out << clean(narrow(root)) << '\t' << clean(c.preset) << '\t' << clean(c.target) << '\t' << clean(c.args) << '\t'
            << clean(c.com_port) << '\t' << clean(c.com_hwid) << '\n';
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        const std::string data = out.str();
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) return false;
    }
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

std::string expand_placeholders(std::string_view tmpl, const std::map<std::string, std::string>& values) {
    std::string out;
    for (size_t i = 0; i < tmpl.size();) {
        if (tmpl[i] == '{') {
            const size_t close = tmpl.find('}', i);
            if (close != std::string_view::npos) {
                auto it = values.find(std::string(tmpl.substr(i + 1, close - i - 1)));
                if (it != values.end()) {
                    out += it->second;
                    i = close + 1;
                    continue;
                }
            }
        }
        out.push_back(tmpl[i++]);
    }
    return out;
}

}  // namespace kamil
