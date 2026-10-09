// kamil_bench: search latency on synthetic data, so "fast as you type" stays a number.
//
//   kamil_bench [files]     (default 200000 indexed files)
//
// Prints p50 / p99 / max per query for the item search (apps, projects, commands) and the file
// index. A keystroke should stay well below 16 ms (one frame) in total.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "core/file_index.h"
#include "core/fuzzy.h"
#include "core/search.h"
#include "core/text.h"
#include "core/usage.h"

using namespace kamil;
using Clock = std::chrono::steady_clock;

namespace {

const wchar_t* const kWords[] = {L"build",  L"deploy", L"sensor", L"flash",   L"test",   L"config", L"report", L"device", L"update",
                                 L"serial", L"log",    L"tool",   L"proje",   L"çalışma", L"notlar", L"backup", L"server", L"client",
                                 L"data",   L"image",  L"main",   L"utility", L"setup",  L"driver", L"monitor", L"export", L"import"};
const wchar_t* const kExt[] = {L".cpp", L".h", L".txt", L".py", L".bat", L".ps1", L".md", L".json", L".pyw", L".log"};

std::wstring random_name(std::mt19937& rng, int words) {
    std::wstring out;
    for (int i = 0; i < words; ++i) {
        if (i) out += (rng() % 2) ? L"_" : L" ";
        std::wstring w = kWords[rng() % std::size(kWords)];
        if (rng() % 3 == 0) w[0] = static_cast<wchar_t>(towupper(w[0]));
        out += w;
    }
    return out;
}

struct Stats {
    double p50, p99, max;
};

template <class F>
Stats measure(F&& f, int rounds) {
    std::vector<double> ms;
    for (int i = 0; i < rounds; ++i) {
        const auto t0 = Clock::now();
        f();
        ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    return {ms[ms.size() / 2], ms[std::min(ms.size() - 1, ms.size() * 99 / 100)], ms.back()};
}

}  // namespace

int main(int argc, char** argv) {
    const size_t file_count = argc > 1 ? static_cast<size_t>(std::strtoul(argv[1], nullptr, 10)) : 200'000;
    std::mt19937 rng(42);

    std::vector<Item> items;
    for (int i = 0; i < 1500; ++i) {
        Item it;
        it.key = L"app:" + std::to_wstring(i);
        it.title = random_name(rng, 1 + static_cast<int>(rng() % 3));
        it.prepare();
        items.push_back(std::move(it));
    }

    IndexRoot root;
    root.path = L"C:\\src";
    std::vector<std::wstring> paths;
    paths.reserve(file_count);
    std::wstring dir = L"C:\\src";
    for (size_t i = 0; i < file_count; ++i) {
        if (i % 40 == 0) dir = L"C:\\src\\" + random_name(rng, 1) + L"\\" + random_name(rng, 1) + std::to_wstring(i / 40);
        paths.push_back(dir + L"\\" + random_name(rng, 1 + static_cast<int>(rng() % 3)) + kExt[rng() % std::size(kExt)]);
    }
    const auto build0 = Clock::now();
    const FileIndex index = FileIndex::from_paths(paths, root);
    const double build_ms = std::chrono::duration<double, std::milli>(Clock::now() - build0).count();

    UsageStore usage;
    SearchOptions opt;
    opt.now_unix = 1'800'000'000;
    const wchar_t* const queries[] = {L"b", L"bu", L"bui", L"build", L"dep", L"sens fl", L"calisma", L"xyzq", L"cfg", L"tool log"};

    std::printf("kamil_bench: %zu items, %zu indexed files (index built in %.0f ms)\n\n", items.size(), index.size(), build_ms);
    std::printf("%-12s %26s %26s\n", "query", "items p50/p99/max ms", "files p50/p99/max ms");
    double worst = 0;
    for (const wchar_t* q : queries) {
        const Stats a = measure([&] { (void)search(items, q, usage, opt); }, 50);
        const Stats f = measure(
            [&] {
                FuzzyMatcher m(q);
                (void)index.search(m, 9);
            },
            20);
        worst = std::max(worst, a.p99 + f.p99);
        std::printf("%-12s %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f\n", narrow(q).c_str(), a.p50, a.p99, a.max, f.p50, f.p99, f.max);
    }
    std::printf("\nworst single query (items p99 + files p99): %.2f ms\n", worst);

    // Typing: each keystroke extends the previous query, so the file search narrows.
    std::printf("\ntyping, per keystroke (files, with narrowing):\n");
    double typing_worst = 0;
    for (const wchar_t* q : queries) {
        std::vector<double> per_key;
        for (int round = 0; round < 10; ++round) {
            FileIndex::SearchState state;
            const std::wstring full = q;
            for (size_t n = 1; n <= full.size(); ++n) {
                const auto t0 = Clock::now();
                FuzzyMatcher m(std::wstring_view(full).substr(0, n));
                (void)index.search(m, 9, &state);
                per_key.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
            }
        }
        std::sort(per_key.begin(), per_key.end());
        typing_worst = std::max(typing_worst, per_key.back());
        std::printf("%-12s p50 %6.2f   max %6.2f ms\n", narrow(q).c_str(), per_key[per_key.size() / 2], per_key.back());
    }
    std::printf("\nworst keystroke while typing: %.2f ms\n", typing_worst);
    return 0;
}
