#pragma once

// Crash dumps: on an unhandled exception (or std::terminate) Kamil writes a minidump into the
// cache folder, so a crash on the work PC can be analysed instead of guessed at. At the next
// start the new dumps are reported once.

#include <filesystem>
#include <vector>

namespace kamil {

void install_crash_handler(const std::filesystem::path& dump_dir);

// Dumps written since the last call (in any earlier run); marks them as reported. Keeps the
// newest five dumps and deletes older ones.
std::vector<std::filesystem::path> unreported_crash_dumps(const std::filesystem::path& dump_dir);
std::vector<std::filesystem::path> crash_dumps(const std::filesystem::path& dump_dir);  // newest first

}  // namespace kamil
