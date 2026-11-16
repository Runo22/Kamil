#include "core/conan.h"

#include <algorithm>
#include <filesystem>
#include <set>

#include "core/text.h"

namespace kamil {

namespace fs = std::filesystem;

namespace {

std::wstring join(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    const wchar_t last = dir.back();
    if (last == L'\\' || last == L'/') return dir + name;
    // The separator the path already uses ("C:\\Users" also when tested on Linux).
    const wchar_t sep = dir.find(L'\\') != std::wstring::npos ? L'\\' : static_cast<wchar_t>(fs::path::preferred_separator);
    return dir + sep + name;
}

// Sub folders of `dir`, by name (case insensitive).
std::vector<std::wstring> sub_dirs(const std::wstring& dir) {
    std::vector<std::wstring> out;
    std::error_code ec;
    fs::directory_iterator it(fs::path(dir), fs::directory_options::skip_permission_denied, ec);
    if (ec) return out;
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code e2;
        if (!it->is_directory(e2)) continue;
        std::wstring name = it->path().filename().wstring();
        if (name.empty() || name[0] == L'.') continue;
        out.push_back(std::move(name));
    }
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) { return fold(a) < fold(b); });
    return out;
}

bool is_dir(const std::wstring& p) {
    std::error_code ec;
    return fs::is_directory(fs::path(p), ec);
}

}  // namespace

std::wstring ConanRef::display() const {
    if ((user.empty() || user == L"_") && (channel.empty() || channel == L"_")) return version;
    return version + L"@" + user + L"/" + channel;
}

std::wstring conan_home(const std::wstring& setting, const std::wstring& conan_user_home, const std::wstring& profile) {
    if (!setting.empty()) return setting;
    if (!conan_user_home.empty()) return join(conan_user_home, L".conan");
    return profile.empty() ? std::wstring() : join(profile, L".conan");
}

std::wstring conan_data_dir(const std::wstring& home) {
    if (home.empty()) return {};
    const std::wstring direct = join(home, L"data");
    if (is_dir(direct)) return direct;
    const std::wstring nested = join(join(home, L".conan"), L"data");
    if (is_dir(nested)) return nested;
    return {};
}

std::vector<ConanPackage> scan_conan_data(const std::wstring& data_dir, size_t max_refs) {
    std::vector<ConanPackage> out;
    if (data_dir.empty()) return out;
    size_t refs = 0;
    for (const auto& name : sub_dirs(data_dir)) {
        ConanPackage p;
        p.name = name;
        p.folder = join(data_dir, name);
        for (const auto& version : sub_dirs(p.folder)) {
            const std::wstring vdir = join(p.folder, version);
            for (const auto& user : sub_dirs(vdir)) {
                const std::wstring udir = join(vdir, user);
                for (const auto& channel : sub_dirs(udir)) {
                    if (refs++ >= max_refs) break;
                    p.refs.push_back(ConanRef{version, user, channel, join(udir, channel)});
                }
            }
        }
        std::stable_sort(p.refs.begin(), p.refs.end(),
                         [](const ConanRef& a, const ConanRef& b) { return compare_versions(a.version, b.version) > 0; });
        out.push_back(std::move(p));
    }
    return out;
}

std::wstring conan_open_folder(const ConanPackage& p) {
    if (p.refs.empty()) return p.folder;
    std::set<std::wstring> versions, users, channels;
    for (const auto& r : p.refs) versions.insert(r.version);
    if (versions.size() > 1) return p.folder;
    const ConanRef& r = p.refs.front();
    for (const auto& x : p.refs) users.insert(x.user);
    const std::wstring vdir = join(p.folder, r.version);
    if (users.size() > 1) return vdir;
    for (const auto& x : p.refs) channels.insert(x.channel);
    if (channels.size() > 1) return join(vdir, r.user);
    return r.folder;
}

std::wstring conan_summary(const ConanPackage& p, size_t max_shown) {
    std::wstring out;
    for (size_t i = 0; i < p.refs.size() && i < max_shown; ++i) out += (i ? L" · " : L"") + p.refs[i].display();
    if (p.refs.size() > max_shown) out += L" (+" + std::to_wstring(p.refs.size() - max_shown) + L")";
    return out;
}

int compare_versions(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    auto digit = [](wchar_t c) { return c >= L'0' && c <= L'9'; };
    while (i < a.size() && j < b.size()) {
        if (digit(a[i]) && digit(b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && a[i2] == L'0') ++i2;  // leading zeros do not count
            while (j2 < b.size() && b[j2] == L'0') ++j2;
            size_t ie = i2, je = j2;
            while (ie < a.size() && digit(a[ie])) ++ie;
            while (je < b.size() && digit(b[je])) ++je;
            if (ie - i2 != je - j2) return ie - i2 < je - j2 ? -1 : 1;
            if (const int c = a.compare(i2, ie - i2, b, j2, je - j2)) return c < 0 ? -1 : 1;
            i = ie;
            j = je;
            continue;
        }
        const wchar_t x = fold_char(a[i]), y = fold_char(b[j]);
        if (x != y) return x < y ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

}  // namespace kamil
