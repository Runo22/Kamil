#pragma once

#include "core/settings.h"

namespace kamil {

// All of Kamil's settings. New settings are added in settings_schema.cpp only.
const Schema& builtin_schema();

namespace keys {
inline constexpr const char* kHotkey = "general.hotkey";
inline constexpr const char* kStartWithWindows = "general.start_with_windows";
inline constexpr const char* kRememberQuery = "general.remember_last_query";
inline constexpr const char* kHideOnFocusLoss = "general.hide_on_focus_loss";

inline constexpr const char* kTheme = "appearance.theme";
inline constexpr const char* kAccent = "appearance.accent";
inline constexpr const char* kWidth = "appearance.width";
inline constexpr const char* kMaxRows = "appearance.max_rows";
inline constexpr const char* kFontSize = "appearance.font_size";
inline constexpr const char* kPosition = "appearance.position";
inline constexpr const char* kAnimations = "appearance.animations";
inline constexpr const char* kFooter = "appearance.footer";

inline constexpr const char* kMaxResults = "search.max_results";
inline constexpr const char* kShowFrequent = "search.show_frequent_when_empty";
inline constexpr const char* kExcludeApps = "search.exclude_apps";
inline constexpr const char* kAliases = "search.aliases";
inline constexpr const char* kLearning = "search.learning";
inline constexpr const char* kLearningHalfLife = "search.learning_half_life";

inline constexpr const char* kDefaultVs = "dev.default_vs";
inline constexpr const char* kProjectRoots = "dev.project_roots";
inline constexpr const char* kScanDepth = "dev.scan_depth";
inline constexpr const char* kScanExclude = "dev.scan_exclude";
inline constexpr const char* kRepoAction = "dev.repo_action";
inline constexpr const char* kTerminal = "dev.terminal";
inline constexpr const char* kGitStatus = "dev.git_status";
}  // namespace keys

}  // namespace kamil
