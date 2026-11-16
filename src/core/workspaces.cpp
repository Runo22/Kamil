#include "core/workspaces.h"

#include "core/bookmarks.h"
#include "core/git.h"
#include "core/text.h"

namespace kamil {

std::wstring WorkspaceEntry::label() const {
    if (path.empty()) return item;
    const std::wstring part = last_path_part(path);
    return part.empty() ? path : part;
}

std::vector<Workspace> parse_workspaces(const Value::Objects& objects, const std::wstring& home, const EnvExpander& expand,
                                        std::vector<std::wstring>* problems) {
    std::vector<Workspace> out;
    auto text = [](const Object& o, const char* field) {
        const Value* v = o.find(field);
        return v && !v->is_null() ? std::wstring(trim(std::wstring_view(widen(v->as_string())))) : std::wstring();
    };
    for (const auto& obj : objects) {
        Workspace w;
        w.name = text(obj, "name");
        if (w.name.empty()) continue;
        w.keyword_folded = fold(text(obj, "keyword"));
        const Value* open = obj.find("open");
        if (open && !open->is_null()) {
            size_t index = 0;
            for (const auto& e : open->as_objects()) {
                ++index;
                WorkspaceEntry entry;
                entry.path = normalize_bookmark_path(text(e, "path"), home, expand);
                entry.item = text(e, "item");
                if (const Value* with = e.find("with"); with && !with->is_null()) entry.with = with->as_string();
                entry.branch = text(e, "branch");
                if (entry.path.empty() && entry.item.empty()) {
                    if (problems)
                        problems->push_back(w.name + L": " + std::to_wstring(index) + L". entry has no 'path' or 'item'");
                    continue;
                }
                if (!entry.path.empty()) entry.item.clear();  // path wins
                w.entries.push_back(std::move(entry));
            }
        }
        out.push_back(std::move(w));
    }
    return out;
}

std::optional<std::wstring> current_branch(const std::wstring& path) {
    const auto git_dir = resolve_git_dir(std::filesystem::path(path));
    if (!git_dir) return std::nullopt;
    const GitHead head = read_head(*git_dir);
    if (!head.valid) return std::nullopt;
    return widen(head.display());
}

std::vector<BranchProblem> check_branches(const Workspace& w,
                                          const std::function<std::optional<std::wstring>(const std::wstring&)>& branch_of) {
    std::vector<BranchProblem> out;
    for (size_t i = 0; i < w.entries.size(); ++i) {
        const WorkspaceEntry& e = w.entries[i];
        if (e.branch.empty() || e.path.empty()) continue;
        const auto actual = branch_of(e.path);
        if (actual && *actual == e.branch) continue;
        out.push_back(BranchProblem{i, e.branch, actual.value_or(std::wstring())});
    }
    return out;
}

}  // namespace kamil
