#include "providers/apps_provider.h"

#include <knownfolders.h>
#include <propkey.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "core/text.h"

namespace kamil {

namespace {

// Not declared by every SDK; values from propkey.h.
const PROPERTYKEY kPKeyLinkTargetParsingPath = {{0xB9B4B3FC, 0x2B51, 0x4A42, {0xB5, 0xD8, 0x32, 0x41, 0x46, 0xAF, 0xCF, 0x25}}, 2};

std::wstring display_name(IShellItem* item, SIGDN form) {
    PWSTR raw = nullptr;
    std::wstring out;
    if (SUCCEEDED(item->GetDisplayName(form, &raw)) && raw) out = raw;
    CoTaskMemFree(raw);
    return out;
}

Item make_item(std::wstring title, std::wstring parsing, std::wstring path) {
    Item it;
    it.kind = ItemKind::App;
    it.key = L"app:" + parsing;
    it.title = std::move(title);
    it.target = L"shell:AppsFolder\\" + parsing;
    it.path = std::move(path);
    it.subtitle = it.path.empty() ? L"Uygulama" : it.path;
    it.prepare();
    return it;
}

}  // namespace

std::vector<Item> enumerate_apps() {
    std::vector<Item> items;
    ComPtr<IShellItem> folder;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder)))) return items;
    ComPtr<IEnumShellItems> en;
    if (FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) return items;

    std::unordered_set<std::wstring> seen;
    for (;;) {
        ComPtr<IShellItem> child;
        ULONG fetched = 0;
        if (en->Next(1, &child, &fetched) != S_OK || fetched == 0) break;
        std::wstring title = display_name(child.Get(), SIGDN_NORMALDISPLAY);
        std::wstring parsing = display_name(child.Get(), SIGDN_PARENTRELATIVEPARSING);
        if (title.empty() || parsing.empty() || !seen.insert(parsing).second) continue;

        std::wstring path;
        ComPtr<IShellItem2> item2;
        if (SUCCEEDED(child.As(&item2))) {
            PWSTR target = nullptr;
            if (SUCCEEDED(item2->GetString(kPKeyLinkTargetParsingPath, &target)) && target) path = target;
            CoTaskMemFree(target);
        }
        items.push_back(make_item(std::move(title), std::move(parsing), std::move(path)));
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.title_folded < b.title_folded; });
    return items;
}

AppsProvider::~AppsProvider() {
    if (worker_.joinable()) worker_.join();
}

void AppsProvider::scan_async(HWND target) {
    if (worker_.joinable()) worker_.join();  // at most one scan at a time; scans are short
    worker_ = std::thread([target] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        auto* items = new std::vector<Item>(enumerate_apps());
        if (SUCCEEDED(hr)) CoUninitialize();
        post_owned(target, WM_KAMIL_APPS_READY, items);
    });
}

std::vector<Item> AppsProvider::load_cache(const std::filesystem::path& file) {
    std::vector<Item> items;
    std::ifstream in(file, std::ios::binary);
    if (!in) return items;
    std::string line;
    std::getline(in, line);
    if (line.rfind("kamil-apps 1", 0) != 0) return items;  // unknown format: rescan instead
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t a = line.find('\t');
        const size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        items.push_back(make_item(widen(std::string_view(line).substr(0, a)), widen(std::string_view(line).substr(a + 1, b - a - 1)),
                                  widen(std::string_view(line).substr(b + 1))));
    }
    return items;
}

void AppsProvider::save_cache(const std::filesystem::path& file, const std::vector<Item>& items) {
    std::ostringstream out;
    out << "kamil-apps 1\n";
    constexpr std::wstring_view kPrefix = L"shell:AppsFolder\\";
    for (const auto& it : items) {
        if (it.kind != ItemKind::App || it.target.compare(0, kPrefix.size(), kPrefix) != 0) continue;
        out << narrow(it.title) << '\t' << narrow(std::wstring_view(it.target).substr(kPrefix.size())) << '\t' << narrow(it.path)
            << '\n';
    }
    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        const std::string data = out.str();
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
}

}  // namespace kamil
