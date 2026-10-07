cmake_minimum_required(VERSION 3.25)

project(Kamil VERSION 0.1.0 DESCRIPTION "Offline developer launcher for Windows" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Static CRT: Kamil.exe runs on machines without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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

# ---------------------------------------------------------------------------------------------
# Core: platform independent, unit tested

add_library(kamil_core STATIC
    src/core/fuzzy.cpp
    src/core/hotkey.cpp
    src/core/line_edit.cpp
    src/core/search.cpp
    src/core/settings.cpp
    src/core/settings_schema.cpp
    src/core/text.cpp
    src/core/usage.cpp
)
target_include_directories(kamil_core PUBLIC src)
target_link_libraries(kamil_core PUBLIC kamil_ryml)
target_compile_options(kamil_core PRIVATE ${KAMIL_WARNINGS})

# ---------------------------------------------------------------------------------------------
# Kamil.exe (Windows)

if(WIN32)
    add_executable(Kamil WIN32
        src/app/main.cpp
        src/app/app.cpp
        src/app/paths.cpp
        src/platform/file_watcher.cpp
        src/platform/icon_loader.cpp
        src/platform/shell.cpp
        src/platform/system_theme.cpp
        src/platform/tray.cpp
        src/providers/apps_provider.cpp
        src/ui/launcher_window.cpp
        src/ui/renderer.cpp
        res/kamil.rc
    )
    target_link_libraries(Kamil PRIVATE kamil_core
        d2d1 dwrite d3d11 dxgi dcomp dwmapi windowscodecs
        shell32 shlwapi ole32 oleaut32 uuid propsys comctl32 user32 gdi32 advapi32)
    target_compile_definitions(Kamil PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00 WINVER=0x0A00 KAMIL_VERSION="${PROJECT_VERSION}")
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
    add_executable(kamil_tests tests/test_main.cpp tests/test_core.cpp tests/test_settings.cpp)
    target_link_libraries(kamil_tests PRIVATE kamil_core)
    target_compile_options(kamil_tests PRIVATE ${KAMIL_WARNINGS})
    if(MINGW)
        target_link_options(kamil_tests PRIVATE -static)
    endif()
    add_test(NAME kamil_tests COMMAND kamil_tests)
endif()
