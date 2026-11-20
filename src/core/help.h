#pragma once

// "Kamil: Help": a short page per feature (what it does, the keys, a settings.yaml example to
// copy). The examples are unit tested against the settings schema, so they stay valid.

#include <span>
#include <string>
#include <string_view>

namespace kamil {

struct HelpTopic {
    const char* id;
    const char* title_en;
    const char* title_tr;
    const char* body_en;  // lines; "- " starts a bullet; `code` and **bold** inline
    const char* body_tr;
    const char* yaml;     // settings.yaml example (top level keys), or nullptr
};

std::span<const HelpTopic> help_topics();

// The whole page: self-contained HTML (no network), light and dark, a filter box, copy buttons.
std::string help_html(bool turkish, std::string_view version);

}  // namespace kamil
