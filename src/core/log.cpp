#include "core/log.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>

#include "core/text.h"

namespace kamil {

namespace {

constexpr std::uintmax_t kMaxSize = 1u << 20;

std::mutex g_mutex;
std::filesystem::path g_file;
std::ofstream g_out;

void rotate_if_needed() {
    std::error_code ec;
    if (std::filesystem::file_size(g_file, ec) < kMaxSize || ec) return;
    g_out.close();
    std::filesystem::path old = g_file;
    old.replace_extension(".old.log");
    std::filesystem::rename(g_file, old, ec);
    g_out.open(g_file, std::ios::binary | std::ios::app);
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return buf;
}

}  // namespace

void log_open(const std::filesystem::path& file) {
    std::lock_guard lock(g_mutex);
    g_out.close();
    g_file = file;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    g_out.open(file, std::ios::binary | std::ios::app);
    rotate_if_needed();
}

void log_line(std::string_view text) {
    std::lock_guard lock(g_mutex);
    if (!g_out) return;
    g_out << timestamp() << "  " << text << "\r\n";
    g_out.flush();
    rotate_if_needed();
}

void log_line(std::wstring_view text) { log_line(std::string_view(narrow(text))); }

std::filesystem::path log_file() {
    std::lock_guard lock(g_mutex);
    return g_file;
}

}  // namespace kamil
