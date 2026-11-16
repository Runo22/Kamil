#include <filesystem>
#include <fstream>

#include "core/settings_schema.h"
#include "core/text.h"
#include "core/workspaces.h"
#include "test.h"

using namespace kamil;
namespace fs = std::filesystem;

namespace {

const std::wstring kHome = L"C:\\Users\\tuna";

std::wstring fake_expand(const std::wstring& s) {
    std::wstring out = s;
    for (size_t pos; (pos = out.find(L"%SRC%")) != std::wstring::npos;) out.replace(pos, 5, L"D:\\src");
    return out;
}

void make_repo(const fs::path& dir, const std::string& head) {
    fs::create_directories(dir / ".git");
    std::ofstream(dir / ".git" / "HEAD") << head;
}

}  // namespace

TEST(workspace_settings) {
    const char* yaml =
        "workspaces:\n"
        "  - name: Sensor\n"
        "    keyword: SEN\n"
        "    open:\n"
        "      - path: '%SRC%\\sensor'\n"
        "        with: vs\n"
        "        branch: develop\n"
        "      - { path: '~\\notes\\', with: code }\n"
        "      - { item: Tera Term }\n"
        "      - { with: code }\n"                      // neither path nor item: skipped
        "      - { path: 'D:\\x', item: Calculator }\n"  // both: path wins
        "  - name: Empty\n"
        "    open: []\n";
    auto r = load_settings_text(builtin_schema(), yaml, "settings.yaml");
    for (const auto& d : r.diagnostics) std::printf("    %s\n", d.to_string().c_str());
    CHECK(r.diagnostics.empty());
    std::vector<std::wstring> problems;
    const auto ws = parse_workspaces(r.settings->get_objects(keys::kWorkspaces), kHome, fake_expand, &problems);
    CHECK_EQ(ws.size(), 2u);
    CHECK_EQ(problems.size(), 1u);
    if (!problems.empty()) CHECK(problems[0].find(L"Sensor: 4.") == 0);
    if (ws.size() != 2) return;
    const Workspace& w = ws[0];
    CHECK_EQ(w.name, L"Sensor");
    CHECK_EQ(w.keyword_folded, L"sen");
    CHECK_EQ(w.entries.size(), 4u);
    if (w.entries.size() == 4) {
        CHECK_EQ(w.entries[0].path, L"D:\\src\\sensor");
        CHECK_EQ(w.entries[0].with, std::string("vs"));
        CHECK_EQ(w.entries[0].branch, L"develop");
        CHECK_EQ(w.entries[0].label(), L"sensor");
        CHECK_EQ(w.entries[1].path, kHome + L"\\notes");
        CHECK_EQ(w.entries[1].with, std::string("code"));
        CHECK(w.entries[1].branch.empty());
        CHECK(w.entries[2].path.empty());
        CHECK_EQ(w.entries[2].item, L"Tera Term");
        CHECK_EQ(w.entries[2].with, std::string("default"));
        CHECK_EQ(w.entries[2].label(), L"Tera Term");
        CHECK_EQ(w.entries[3].path, L"D:\\x");
        CHECK(w.entries[3].item.empty());
    }
    CHECK(ws[1].entries.empty());
}

TEST(workspace_settings_errors) {
    const char* yaml =
        "workspaces:\n"
        "  - name: A\n"                          // 'open' is required
        "  - open: [{ path: 'C:\\x' }]\n"         // 'name' is required
        "  - name: B\n"
        "    open:\n"
        "      - { path: 'C:\\x', with: notepad }\n";  // invalid choice
    auto r = load_settings_text(builtin_schema(), yaml, "settings.yaml");
    for (const auto& d : r.diagnostics) std::printf("    %s\n", d.to_string().c_str());
    auto has = [&](const char* key) {
        for (const auto& d : r.diagnostics)
            if (d.key.find(key) != std::string::npos) return true;
        return false;
    };
    CHECK(has("workspaces[0]"));
    CHECK(has("workspaces[1]"));
    CHECK(has("workspaces[2].open[0].with"));
    const auto ws = parse_workspaces(r.settings->get_objects(keys::kWorkspaces), kHome, fake_expand);
    CHECK_EQ(ws.size(), 1u);  // B, its entry with the default 'with'
    if (ws.size() == 1 && ws[0].entries.size() == 1) CHECK_EQ(ws[0].entries[0].with, std::string("default"));
    // The generated settings.yaml and JSON schema describe the nested list.
    const std::string json = generate_json_schema(builtin_schema());
    CHECK(json.find("\"workspaces\"") != std::string::npos && json.find("\"branch\"") != std::string::npos);
    CHECK(load_settings_text(builtin_schema(), generate_default_yaml(builtin_schema()), "settings.yaml").diagnostics.empty());
}

TEST(workspace_branches) {
    const fs::path root = fs::temp_directory_path() / "kamil_test_ws";
    fs::remove_all(root);
    make_repo(root / "sensor", "ref: refs/heads/develop\n");
    make_repo(root / "lib", "ref: refs/heads/feature/x\n");
    make_repo(root / "detached", "0123456789abcdef0123456789abcdef01234567\n");
    fs::create_directories(root / "plain");
    // A worktree: ".git" is a file pointing at the real git dir.
    make_repo(root / "main-wt-gitdir", "ref: refs/heads/release\n");
    fs::create_directories(root / "wt");
    std::ofstream(root / "wt" / ".git") << "gitdir: " << (root / "main-wt-gitdir" / ".git").string() << "\n";

    CHECK(current_branch((root / "sensor").wstring()) == std::optional<std::wstring>(L"develop"));
    CHECK(current_branch((root / "lib").wstring()) == std::optional<std::wstring>(L"feature/x"));
    CHECK(current_branch((root / "detached").wstring()) == std::optional<std::wstring>(L"0123456"));
    CHECK(current_branch((root / "wt").wstring()) == std::optional<std::wstring>(L"release"));
    CHECK(!current_branch((root / "plain").wstring()));
    CHECK(!current_branch((root / "missing").wstring()));

    Workspace w;
    w.name = L"T";
    w.entries = {
        {(root / "sensor").wstring(), L"", "vs", L"develop"},   // fine
        {(root / "lib").wstring(), L"", "code", L"develop"},    // on feature/x
        {(root / "plain").wstring(), L"", "default", L"main"},  // not git
        {(root / "lib").wstring(), L"", "default", L""},        // no branch: not checked
        {L"", L"Tera Term", "default", L"main"},                // an item: not checked
    };
    int calls = 0;
    const auto problems = check_branches(w, [&](const std::wstring& p) {
        ++calls;
        return current_branch(p);
    });
    CHECK_EQ(calls, 3);
    CHECK_EQ(problems.size(), 2u);
    if (problems.size() == 2) {
        CHECK_EQ(problems[0].entry, 1u);
        CHECK_EQ(problems[0].expected, L"develop");
        CHECK_EQ(problems[0].actual, L"feature/x");
        CHECK_EQ(problems[1].entry, 2u);
        CHECK(problems[1].actual.empty());
    }
    fs::remove_all(root);
}
