#include "core/ryml_support.h"

#include <mutex>

#ifndef _RYML_SINGLE_HEADER_AMALGAMATED_HPP_
#include <ryml_all.hpp>
#endif

namespace kamil {

namespace {
[[noreturn]] void on_error(const char* msg, size_t len, ryml::Location loc, void*) {
    throw RymlError{std::string(msg, len), loc.line, loc.col};
}
}  // namespace

void install_ryml_error_handler() {
    static std::once_flag once;
    std::call_once(once, [] { ryml::set_callbacks(ryml::Callbacks(nullptr, nullptr, nullptr, on_error)); });
}

}  // namespace kamil
