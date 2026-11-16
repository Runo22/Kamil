#include <filesystem>
#include <fstream>

#include "core/conan.h"
#include "core/text.h"
#include "test.h"

using namespace kamil;
namespace fs = std::filesystem;

namespace {

fs::path make_cache(const char* name) {
    const fs::path root = fs::temp_directory_path() / name;
    fs::remove_all(root);
    return root;
}

void ref(const fs::path& data, const char* name, const char* version, const char* user, const char* channel) {
    const fs::path r = data / name / version / user / channel;
    fs::create_directories(r / "export");
    fs::create_directories(r / "package" / "4cf1e7b5");
    std::ofstream(r / "metadata.json") << "{}";
}

std::wstring w(const fs::path& p) { return p.wstring(); }

const ConanPackage* find(const std::vector<ConanPackage>& ps, const wchar_t* name) {
    for (const auto& p : ps)
        if (p.name == name) return &p;
    return nullptr;
}

}  // namespace

TEST(conan_home_location) {
    CHECK_EQ(conan_home(L"", L"", L"C:\\Users\\tuna"), L"C:\\Users\\tuna\\.conan");
    CHECK_EQ(conan_home(L"", L"D:\\conan", L"C:\\Users\\tuna"), L"D:\\conan\\.conan");  // CONAN_USER_HOME wins
    CHECK_EQ(conan_home(L"", L"D:\\conan\\", L"C:\\Users\\tuna"), L"D:\\conan\\.conan");
    CHECK_EQ(conan_home(L"E:\\cache\\.conan", L"D:\\conan", L"C:\\Users\\tuna"), L"E:\\cache\\.conan");  // the setting wins
    CHECK(conan_home(L"", L"", L"").empty());
}

TEST(conan_data_folder) {
    const fs::path root = make_cache("kamil_test_conan_home");
    CHECK(conan_data_dir(w(root / ".conan")).empty());  // nothing there yet
    CHECK(conan_data_dir(L"").empty());
    fs::create_directories(root / ".conan" / "data");
    CHECK_EQ(conan_data_dir(w(root / ".conan")), w(root / ".conan" / "data"));
    CHECK_EQ(conan_data_dir(w(root)), w(root / ".conan" / "data"));  // the parent of .conan (like CONAN_USER_HOME)
    fs::remove_all(root);
}

TEST(conan_scan_and_open_folder) {
    const fs::path root = make_cache("kamil_test_conan_scan");
    const fs::path data = root / "data";
    ref(data, "fmt", "9.1.0", "acme", "stable");
    ref(data, "fmt", "10.2.1", "acme", "stable");      // two versions: the package folder
    ref(data, "zlib", "1.2.13", "acme", "stable");     // one reference: its own folder
    ref(data, "boost", "1.81.0", "acme", "stable");
    ref(data, "boost", "1.81.0", "other", "stable");   // one version, two users: the version folder
    ref(data, "gtest", "1.14.0", "acme", "stable");
    ref(data, "gtest", "1.14.0", "acme", "testing");   // one user, two channels: the user folder
    ref(data, "nlohmann_json", "3.11.2", "_", "_");    // no user / channel
    fs::create_directories(data / "empty_pkg");        // no reference: the package folder
    std::ofstream(data / "stray.lock") << "x";         // files are ignored

    const auto ps = scan_conan_data(w(data));
    CHECK_EQ(ps.size(), 6u);
    const ConanPackage* fmt = find(ps, L"fmt");
    CHECK(fmt && fmt->refs.size() == 2);
    if (fmt && fmt->refs.size() == 2) {
        CHECK_EQ(fmt->refs[0].version, L"10.2.1");  // newest first, numerically
        CHECK_EQ(fmt->refs[0].display(), L"10.2.1@acme/stable");
        CHECK_EQ(conan_open_folder(*fmt), w(data / "fmt"));
        CHECK_EQ(conan_summary(*fmt), L"10.2.1@acme/stable · 9.1.0@acme/stable");
    }
    const ConanPackage* zlib = find(ps, L"zlib");
    CHECK(zlib && conan_open_folder(*zlib) == w(data / "zlib" / "1.2.13" / "acme" / "stable"));
    const ConanPackage* boost = find(ps, L"boost");
    CHECK(boost && conan_open_folder(*boost) == w(data / "boost" / "1.81.0"));
    const ConanPackage* gtest = find(ps, L"gtest");
    CHECK(gtest && conan_open_folder(*gtest) == w(data / "gtest" / "1.14.0" / "acme"));
    const ConanPackage* json = find(ps, L"nlohmann_json");
    CHECK(json && json->refs.size() == 1 && json->refs[0].display() == L"3.11.2");
    CHECK(json && conan_open_folder(*json) == w(data / "nlohmann_json" / "3.11.2" / "_" / "_"));
    const ConanPackage* empty = find(ps, L"empty_pkg");
    CHECK(empty && empty->refs.empty() && conan_open_folder(*empty) == w(data / "empty_pkg") && conan_summary(*empty).empty());

    CHECK(scan_conan_data(w(root / "nope")).empty());
    CHECK(scan_conan_data(L"").empty());
    CHECK_EQ(scan_conan_data(w(data), 3).size(), 6u);  // the reference limit keeps every package
    fs::remove_all(root);
}

TEST(conan_summary_limit) {
    ConanPackage p;
    p.name = L"x";
    for (const wchar_t* v : {L"4.0", L"3.0", L"2.0", L"1.0", L"0.9"}) p.refs.push_back(ConanRef{v, L"a", L"s", L""});
    CHECK_EQ(conan_summary(p), L"4.0@a/s · 3.0@a/s · 2.0@a/s (+2)");
    CHECK_EQ(conan_summary(p, 1), L"4.0@a/s (+4)");
}

TEST(conan_version_order) {
    CHECK(compare_versions(L"10.0.0", L"9.1.0") > 0);
    CHECK(compare_versions(L"1.2.13", L"1.2.3") > 0);
    CHECK(compare_versions(L"1.2", L"1.2.0") < 0);
    CHECK(compare_versions(L"1.02", L"1.2") == 0);
    CHECK(compare_versions(L"1.0.0", L"1.0.0") == 0);
    CHECK(compare_versions(L"cci.20230101", L"cci.20221231") > 0);
    CHECK(compare_versions(L"1.0-rc1", L"1.0-rc2") < 0);
    CHECK(compare_versions(L"", L"1") < 0);
}
