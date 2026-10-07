#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "platform/win.h"

namespace kamil {

struct IconResult {
    std::wstring key;           // item key the icon belongs to
    uint32_t size = 0;          // width == height, physical pixels
    std::vector<uint32_t> bgra; // premultiplied BGRA, empty if no icon could be extracted
};

// Extracts shell icons on a background STA thread. Newest requests are served first, so the rows
// the user is looking at right now get their icons before stale ones.
class IconLoader {
public:
    explicit IconLoader(HWND target);
    ~IconLoader();
    IconLoader(const IconLoader&) = delete;
    IconLoader& operator=(const IconLoader&) = delete;

    // `source` is a parsing name: a file system path or "shell:AppsFolder\..." moniker.
    void request(std::wstring key, std::wstring source, uint32_t size);

private:
    struct Request {
        std::wstring key;
        std::wstring source;
        uint32_t size;
    };
    void run();

    HWND target_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace kamil
