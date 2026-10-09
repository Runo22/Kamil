#pragma once

// Small append-only diagnostic log (kamil.log in the cache folder). Thread-safe; the file is
// rotated to kamil.old.log when it grows past 1 MB. Lines are written immediately so the log
// survives a crash.

#include <filesystem>
#include <string_view>

namespace kamil {

void log_open(const std::filesystem::path& file);
void log_line(std::string_view text);
void log_line(std::wstring_view text);
std::filesystem::path log_file();

}  // namespace kamil
