#pragma once

#include <cstddef>
#include <string>

namespace kamil {

// rapidyaml reports errors through a global callback that must not return. Kamil installs one
// that throws RymlError, so parse errors become ordinary C++ exceptions with a location.
struct RymlError {
    std::string message;
    size_t line = 0;  // 0-based
    size_t col = 0;   // 0-based
};

void install_ryml_error_handler();

}  // namespace kamil
