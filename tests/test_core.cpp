#include <filesystem>

#include "core/fuzzy.h"
#include "core/hotkey.h"
#include "core/i18n.h"
#include "core/line_edit.h"
#include "core/search.h"
#include "core/text.h"
#include "core/usage.h"
#include "test.h"

using namespace kamil;

// ---- text ------------------------------------------------------------------------------------

TEST(utf8_roundtrip) {
    const std::string s = "Çalışma Klasörü — İğne ışık ŞÜÖ 😀";
    CHECK_EQ(narrow(widen(s)), s);
    CHECK_EQ(widen("\xC3"), std::wstring(1, static_cast<wchar_t>(0xFFFD)));  // truncated sequence
}

TEST(turkish_folding) {
    CHECK_EQ(fold(widen("Çalışma Klasörü")), L"calisma klasoru");
    CHECK_EQ(fold(widen("İSTANBUL ığdır")), L"istanbul igdir");
    CHECK_EQ(fold(widen("ŞEKER Ğ Ü Ö")), L"seker g u o");
    CHECK_EQ(fold(widen("Café")), L"cafe");
    CHECK(fold(L"Hello").size() == 5);
}

TEST(glob) {
    CHECK(glob_match(L"*uninstall*", L"visual studio uninstaller"));
    CHECK(glob_match(L"note?ad", L"notepad"));
    CHECK(!glob_match(L"*uninstall*", L"notepad"));
    CHECK(glob_match(fold(widen("*kaldır*")), fold(widen("Programı Kaldır"))));
}

// ---- fuzzy -----------------------------------------------------------------------------------

static std::optional<int> score(std::wstring_view q, std::wstring_view t, std::vector<uint16_t>* pos = nullptr) {
    FuzzyMatcher m(q);
    return m.match(t, fold(t), pos);
}

TEST(fuzzy_basic) {
    CHECK(score(L"vsc", L"Visual Studio Code").has_value());
    CHECK(!score(L"xyz", L"Visual Studio Code").has_value());
    CHECK(score(L"calisma", widen("Çalışma Klasörü")).has_value());
    CHECK(score(L"", L"anything").has_value());
}

TEST(fuzzy_prefers_word_starts) {
    // "vsc" as initials beats a scattered match.
    CHECK(*score(L"vsc", L"Visual Studio Code") > *score(L"vsc", L"Devices and Printers Scanner"));
    // Prefix beats middle.
    CHECK(*score(L"note", L"Notepad") > *score(L"note", L"OneNote"));
    // Exact beats prefix.
    CHECK(*score(L"code", L"Code") > *score(L"code", L"Code Insiders"));
    // Consecutive beats gapped.
    CHECK(*score(L"term", L"Windows Terminal") > *score(L"term", L"Tera Emulator Remote Monitor"));
}

TEST(fuzzy_positions) {
    std::vector<uint16_t> pos;
    CHECK(score(L"vsc", L"Visual Studio Code", &pos).has_value());
    CHECK_EQ(pos.size(), 3u);
    CHECK_EQ(pos[0], 0);
    CHECK_EQ(pos[1], 7);
    CHECK_EQ(pos[2], 14);
}

TEST(fuzzy_multi_token) {
    CHECK(score(L"studio vis", L"Visual Studio 2026").has_value());
    CHECK(!score(L"studio xyz", L"Visual Studio 2026").has_value());
    std::vector<uint16_t> pos;
    score(L"vis 2026", L"Visual Studio 2026", &pos);
    CHECK_EQ(pos.size(), 7u);
}

TEST(fuzzy_camel_case) {
    CHECK(*score(L"ss", L"SensorSuite") > *score(L"ss", L"Classic Shell"));
}

// ---- hotkey ----------------------------------------------------------------------------------

TEST(hotkey_parse) {
    auto hk = parse_hotkey("Alt+Space");
    CHECK(hk.has_value());
    CHECK_EQ(hk->mods, static_cast<uint32_t>(kModAlt));
    CHECK_EQ(hk->vk, 0x20u);
    CHECK_EQ(format_hotkey(*hk), std::string("Alt+Space"));

    hk = parse_hotkey(" ctrl + shift + k ");
    CHECK(hk && hk->mods == (kModCtrl | kModShift) && hk->vk == 'K');
    CHECK_EQ(format_hotkey(*hk), std::string("Ctrl+Shift+K"));

    hk = parse_hotkey("Win+F12");
    CHECK(hk && hk->vk == 0x7B && hk->mods == kModWin);
    CHECK(parse_hotkey("F9").has_value());  // function keys may stand alone
    CHECK(parse_hotkey("Ctrl++").has_value() && parse_hotkey("Ctrl++")->vk == 0xBB);

    std::string err;
    CHECK(!parse_hotkey("Space", &err));
    CHECK(!err.empty());
    CHECK(!parse_hotkey("Hyper+K"));
    CHECK(!parse_hotkey("Ctrl+"));
    CHECK(!parse_hotkey("Ctrl+Alt"));
}

// ---- line edit -------------------------------------------------------------------------------

TEST(line_edit_basic) {
    LineEdit e;
    e.insert(L"hello world");
    CHECK_EQ(e.text(), L"hello world");
    e.backspace(true);
    CHECK_EQ(e.text(), L"hello ");
    e.insert(L"there\r\nfriend");
    CHECK_EQ(e.text(), L"hello there  friend");
    e.home(false);
    e.move_right(true, true);
    CHECK_EQ(e.selected_text(), L"hello ");
    e.insert(L"X");
    CHECK_EQ(e.text(), L"Xthere  friend");
    e.select_all();
    e.backspace(false);
    CHECK(e.text().empty());
    CHECK(!e.backspace(false));
}

TEST(line_edit_path_words) {
    LineEdit e;
    e.insert(L"D:\\src\\kamil");
    e.backspace(true);
    CHECK_EQ(e.text(), L"D:\\src\\");
    e.end(false);
    e.move_left(true, false);
    CHECK_EQ(e.caret(), 3u);
}

// ---- usage + search --------------------------------------------------------------------------

static std::vector<Item> sample_items() {
    std::vector<Item> items;
    auto add = [&](const char* title) {
        Item it;
        it.title = widen(title);
        it.key = L"app:" + it.title;
        it.prepare();
        items.push_back(std::move(it));
    };
    add("Tera Term");
    add("Windows Terminal");
    add("Terminal Services Manager");
    add("Notepad");
    add("Visual Studio 2026");
    add("Visual Studio Code");
    add("Çalışma Notları");
    return items;
}

TEST(search_ranks_and_learns) {
    auto items = sample_items();
    UsageStore usage;
    SearchOptions opt;
    opt.now_unix = 1'800'000'000;

    auto hits = search(items, L"te", usage, opt);
    CHECK(!hits.empty());
    CHECK(hits[0].item->title == L"Tera Term");  // word start + prefix

    // The user keeps picking Windows Terminal for "te": it moves to the top.
    for (int i = 0; i < 3; ++i) usage.record(L"app:Windows Terminal", L"te", opt.now_unix);
    hits = search(items, L"te", usage, opt);
    CHECK(hits[0].item->title == L"Windows Terminal");

    // Learning applies to that prefix; an unrelated query is unaffected.
    hits = search(items, L"code", usage, opt);
    CHECK(hits[0].item->title == L"Visual Studio Code");

    hits = search(items, L"notlar", usage, opt);
    CHECK(!hits.empty() && hits[0].item->title == widen("Çalışma Notları"));
}

TEST(search_empty_query_shows_frequent) {
    auto items = sample_items();
    UsageStore usage;
    SearchOptions opt;
    opt.now_unix = 1'800'000'000;
    CHECK(search(items, L"", usage, opt).empty());
    usage.record(L"app:Notepad", L"no", opt.now_unix);
    usage.record(L"app:Notepad", L"no", opt.now_unix);
    usage.record(L"app:Tera Term", L"te", opt.now_unix);
    auto hits = search(items, L"  ", usage, opt);
    CHECK_EQ(hits.size(), 2u);
    CHECK(hits[0].item->title == L"Notepad");
}

TEST(search_alias) {
    auto items = sample_items();
    UsageStore usage;
    SearchOptions opt;
    opt.aliases.push_back({fold(widen("not defteri")), fold(L"Notepad")});
    auto hits = search(items, widen("Not Defteri"), usage, opt);
    CHECK(!hits.empty() && hits[0].item->title == L"Notepad");
}

TEST(usage_decay_and_persistence) {
    UsageStore usage;
    usage.set_half_life_days(14);
    const int64_t t0 = 1'800'000'000;
    usage.record(L"app:A", L"a", t0);
    CHECK(usage.frecency(L"app:A", t0) > 0.99);
    const double later = usage.frecency(L"app:A", t0 + 14 * 86400);
    CHECK(later > 0.49 && later < 0.51);
    CHECK(usage.affinity(L"a", L"app:A", t0) > 0.99);

    const auto dir = std::filesystem::temp_directory_path() / "kamil_test_usage";
    std::filesystem::create_directories(dir);
    const auto file = dir / "usage.tsv";
    usage.record(widen("app:Çalışma"), widen("çal"), t0);
    CHECK(usage.save(file));
    UsageStore loaded;
    CHECK(loaded.load(file));
    CHECK(loaded.frecency(L"app:A", t0) > 0.99);
    CHECK(loaded.affinity(widen("çal"), widen("app:Çalışma"), t0) > 0.99);
    std::filesystem::remove_all(dir);
}

TEST(search_folder_priority) {
    std::vector<Item> items;
    auto add = [&](const wchar_t* title, const wchar_t* path) {
        Item it;
        it.title = title;
        it.key = std::wstring(L"repo:") + path;
        it.path = path;
        it.prepare();
        items.push_back(std::move(it));
    };
    add(L"sensor-tools", L"C:\\Users\\me\\source\\repos\\sensor-tools");
    add(L"sensor", L"D:\\src\\ana\\sensor");
    add(L"sensor-old", L"D:\\src2\\sensor-old");
    UsageStore usage;
    SearchOptions opt;
    auto hits = search(items, L"sensor", usage, opt);
    CHECK(hits[0].item->title == L"sensor");  // exact match wins without boosts

    opt.folder_boosts = {{normalize_folder(L"c:/users/ME/source/repos/"), 60}, {normalize_folder(L"D:\\src"), -80}};
    hits = search(items, L"sensor", usage, opt);
    CHECK(hits[0].item->title == L"sensor-tools");
    CHECK(hits.back().item->title == L"sensor");          // pushed down
    CHECK_EQ(folder_bonus(fold(L"D:\\src2\\sensor-old"), opt.folder_boosts), 0);  // D:\src is not a prefix of D:\src2

    opt.folder_boosts.push_back({normalize_folder(L"D:\\src\\ana"), 40});  // longest prefix wins
    CHECK_EQ(folder_bonus(fold(L"D:\\src\\ana\\sensor"), opt.folder_boosts), 40);
}

#include "core/pixels.h"

TEST(icon_pixels_premultiply) {
    // Straight alpha: a half transparent white edge pixel must become 50% grey, not stay white.
    std::vector<uint32_t> px{0xFFFF0000u, 0x80FFFFFFu, 0x00000000u};
    CHECK(normalize_icon_pixels(px));
    CHECK_EQ(px[0], 0xFFFF0000u);
    CHECK_EQ(px[1], 0x80808080u);
    CHECK_EQ(px[2], 0x00000000u);

    // Already premultiplied: untouched.
    std::vector<uint32_t> pre{0xFF102030u, 0x80404040u};
    CHECK(!normalize_icon_pixels(pre));
    CHECK_EQ(pre[1], 0x80404040u);

    // No alpha at all (legacy icon): opaque.
    std::vector<uint32_t> legacy{0x00112233u};
    CHECK(normalize_icon_pixels(legacy));
    CHECK_EQ(legacy[0], 0xFF112233u);
}

TEST(format_placeholders) {
    CHECK(fmt(L"{} could not be opened", L"Notepad") == L"Notepad could not be opened");
    CHECK(fmt(L"{} / {} s", std::to_wstring(3), L"90") == L"3 / 90 s");
    CHECK(fmt(L"{com} stays, {} is replaced", L"x") == L"{com} stays, x is replaced");
    CHECK(fmt(L"missing {} {}", L"a") == L"missing a ");
}

TEST(search_keywords) {
    std::vector<Item> items(2);
    items[0].key = L"kamil:settings";
    items[0].title = L"Kamil: Edit settings";
    items[0].keywords = widen("Kamil: Ayarları düzenle");
    items[1].key = L"app:Notepad";
    items[1].title = L"Notepad";
    for (auto& it : items) it.prepare();
    UsageStore usage;
    SearchOptions opt;
    opt.now_unix = 1'800'000'000;
    auto hits = search(items, L"ayarlar", usage, opt);  // the other language's title still finds it
    CHECK(!hits.empty() && hits[0].item->key == L"kamil:settings");
    hits = search(items, L"settings", usage, opt);
    CHECK(!hits.empty() && hits[0].item->key == L"kamil:settings");
}
