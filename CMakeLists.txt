cmake_minimum_required(VERSION 3.25)

project(Kamil VERSION 0.1.0 DESCRIPTION "Offline developer launcher for Windows" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Static CRT: Kamil.exe runs on machines without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Commit id shown in "Kamil: Diagnostics" (empty outside a git checkout).
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE KAMIL_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

option(KAMIL_BUILD_TESTS "Build the platform independent unit tests" ON)

if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /EHsc /MP)
    set(KAMIL_WARNINGS /W4)
else()
    set(KAMIL_WARNINGS -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------------------------
# rapidyaml (vendored single header, compiled once)

add_library(kamil_ryml STATIC src/core/ryml_impl.cpp)
target_include_directories(kamil_ryml SYSTEM PUBLIC third_party/rapidyaml)
if(MINGW)
    # fast_float (inside the rapidyaml header) calls _umul128 on _WIN64 without declaring it.
    target_compile_options(kamil_ryml PUBLIC -include intrin.h)
endif()

# ---------------------------------------------------------------------------------------------
# Core: platform independent, unit tested

add_library(kamil_core STATIC
    src/core/cmake.cpp
    src/core/console_buffer.cpp
    src/core/file_index.cpp
    src/core/fuzzy.cpp
    src/core/git.cpp
    src/core/hotkey.cpp
    src/core/i18n.cpp
    src/core/log.cpp
    src/core/line_edit.cpp
    src/core/pixels.cpp
    src/core/project_state.cpp
    src/core/ryml_support.cpp
    src/core/search.cpp
    src/core/settings.cpp
    src/core/settings_schema.cpp
    src/core/text.cpp
    src/core/usage.cpp
)
target_include_directories(kamil_core PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(kamil_core PUBLIC kamil_ryml Threads::Threads)
target_compile_options(kamil_core PRIVATE ${KAMIL_WARNINGS})

# ---------------------------------------------------------------------------------------------
# Kamil.exe (Windows)

if(WIN32)
    enable_language(RC)
    add_executable(Kamil WIN32
        src/app/main.cpp
        src/app/app.cpp
        src/app/app_files.cpp
        src/app/app_projects.cpp
        src/app/app_diagnostics.cpp
        src/app/app_commands.cpp
        src/app/app_console.cpp
        src/app/paths.cpp
        src/platform/child_process.cpp
        src/platform/com_ports.cpp
        src/platform/crash.cpp
        src/platform/file_watcher.cpp
        src/platform/icon_loader.cpp
        src/platform/process.cpp
        src/platform/shell.cpp
        src/platform/system_theme.cpp
        src/platform/tools.cpp
        src/platform/tree_watcher.cpp
        src/platform/tray.cpp
        src/platform/vs_bridge.cpp
        src/providers/apps_provider.cpp
        src/providers/repo_provider.cpp
        src/ui/console_window.cpp
        src/ui/launcher_window.cpp
        src/ui/renderer.cpp
        res/kamil.rc
    )
    target_link_libraries(Kamil PRIVATE kamil_core
        d2d1 dwrite d3d11 dxgi dcomp dwmapi windowscodecs
        setupapi shell32 shcore shlwapi ole32 oleaut32 uuid propsys comctl32 user32 gdi32 advapi32)
    target_compile_definitions(Kamil PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00 WINVER=0x0A00 KAMIL_VERSION="${PROJECT_VERSION}" KAMIL_COMMIT="${KAMIL_COMMIT}")
    target_compile_options(Kamil PRIVATE ${KAMIL_WARNINGS})
    set_target_properties(Kamil PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    if(MSVC)
        target_link_options(Kamil PRIVATE /MANIFEST:NO)  # res/kamil.manifest is embedded via kamil.rc
    elseif(MINGW)
        target_link_options(Kamil PRIVATE -static -municode)
    endif()
endif()

# ---------------------------------------------------------------------------------------------
# Tests

if(KAMIL_BUILD_TESTS)
    enable_testing()
    add_executable(kamil_tests tests/test_main.cpp tests/test_cmake.cpp tests/test_console.cpp tests/test_core.cpp tests/test_file_index.cpp tests/test_git.cpp tests/test_settings.cpp)
    target_link_libraries(kamil_tests PRIVATE kamil_core)
    target_compile_options(kamil_tests PRIVATE ${KAMIL_WARNINGS})
    if(MINGW)
        target_link_options(kamil_tests PRIVATE -static)
    endif()
    add_test(NAME kamil_tests COMMAND kamil_tests)

    add_executable(kamil_bench tests/bench.cpp)
    target_link_libraries(kamil_bench PRIVATE kamil_core)
    target_compile_options(kamil_bench PRIVATE ${KAMIL_WARNINGS})
    if(MINGW)
        target_link_options(kamil_bench PRIVATE -static)
    endif()
endif()
