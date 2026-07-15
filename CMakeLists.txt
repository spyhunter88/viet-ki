cmake_minimum_required(VERSION 3.20)
project(VietKi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VIETKI_MACOS "Build the macOS shell instead of the Windows shell" OFF)
option(VIETKI_LINUX "Build the Linux IBus engine" OFF)
option(VIETKI_BUILD_TESTS "Build the core unit tests" ON)

# ---------------------------------------------------------------------------
# core: OS-independent engine, compiled into every shell.
# ---------------------------------------------------------------------------
add_library(vietki_core STATIC
    core/src/engine.cpp
    core/src/telex.cpp
    core/src/vni.cpp
    core/src/tone_placement.cpp
    core/src/syllable.cpp
    core/src/gaming.cpp
)
target_include_directories(vietki_core PUBLIC core/include)
target_include_directories(vietki_core PRIVATE core/src)

if(MSVC)
    target_compile_options(vietki_core PRIVATE /utf-8 /W4 /permissive-)
else()
    target_compile_options(vietki_core PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
if(VIETKI_BUILD_TESTS)
    enable_testing()
    add_executable(vietki_tests tests/engine_tests.cpp tests/gaming_tests.cpp)
    target_link_libraries(vietki_tests PRIVATE vietki_core)
    target_include_directories(vietki_tests PRIVATE tests)
    if(MSVC)
        target_compile_options(vietki_tests PRIVATE /utf-8)
    endif()
    add_test(NAME vietki_tests COMMAND vietki_tests)
endif()

# ---------------------------------------------------------------------------
# Windows shell: a single WIN32 executable, no console window.
# ---------------------------------------------------------------------------
if(WIN32 AND NOT VIETKI_MACOS)
    add_executable(VietKi WIN32
        windows/src/main.cpp
        windows/src/hook.cpp
        windows/src/injector.cpp
        windows/src/paste_inject.cpp
        windows/src/tray.cpp
        windows/src/config.cpp
        windows/src/omnibox_probe.cpp
        windows/src/settings.cpp
        windows/src/overlay.cpp
        windows/src/typing_stats.cpp
        windows/resources.rc
    )
    target_link_libraries(VietKi PRIVATE vietki_core comctl32 uxtheme ole32
        oleaut32 taskschd)
    target_include_directories(VietKi PRIVATE windows/src)
    target_compile_definitions(VietKi PRIVATE UNICODE _UNICODE NOMINMAX)
    if(MSVC)
        target_compile_options(VietKi PRIVATE /utf-8)
    endif()
    # Ship as_invoker manifest.
    if(EXISTS "${CMAKE_SOURCE_DIR}/windows/app.manifest")
        target_sources(VietKi PRIVATE windows/app.manifest)
    endif()
endif()

# ---------------------------------------------------------------------------
# macOS shell: an LSUIElement .app bundle driven by a CGEventTap.
# ---------------------------------------------------------------------------
if(VIETKI_MACOS AND APPLE)
    enable_language(OBJCXX)
    # Phase 2 E: bundle the colour menu-bar status icons into Resources/. The
    # .icns is the Dock/Finder app icon (CFBundleIconFile in Info.plist).
    set(VIETKI_MAC_ICONS
        "${CMAKE_SOURCE_DIR}/assets/VietKi.icns"
        "${CMAKE_SOURCE_DIR}/assets/VietKi.png"
        "${CMAKE_SOURCE_DIR}/assets/states/menubar_v.png"    "${CMAKE_SOURCE_DIR}/assets/states/menubar_v@2x.png"
        "${CMAKE_SOURCE_DIR}/assets/states/menubar_e.png"    "${CMAKE_SOURCE_DIR}/assets/states/menubar_e@2x.png"
        "${CMAKE_SOURCE_DIR}/assets/states/menubar_vplus.png" "${CMAKE_SOURCE_DIR}/assets/states/menubar_vplus@2x.png"
        "${CMAKE_SOURCE_DIR}/assets/states/menubar_vminus.png" "${CMAKE_SOURCE_DIR}/assets/states/menubar_vminus@2x.png"
    )
    foreach(asset ${VIETKI_MAC_ICONS})
        set_source_files_properties("${asset}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources")
    endforeach()
    add_executable(VietKi MACOSX_BUNDLE
        macos/src/main.mm
        macos/src/EventTap.mm
        macos/src/Injector.mm
        macos/src/Settings.mm
        macos/src/PasteInject.mm
        macos/src/typing_stats.mm
        ${VIETKI_MAC_ICONS}
    )
    target_link_libraries(VietKi PRIVATE vietki_core
        "-framework Cocoa"
        "-framework ApplicationServices"
        "-framework Carbon"
    )
    target_include_directories(VietKi PRIVATE macos/src)
    set_target_properties(VietKi PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/macos/Info.plist"
        MACOSX_BUNDLE_ICON_FILE "VietKi.icns"
    )
    # Automatically codesign the app bundle post-build to ensure macOS TCC registers permissions properly
    add_custom_command(TARGET VietKi POST_BUILD
        COMMAND codesign --force --deep --sign - $<TARGET_FILE_DIR:VietKi>/../..
        COMMENT "Codesigning VietKi.app bundle..."
    )
endif()

# ---------------------------------------------------------------------------
# Linux shell: an IBus engine (composition model). This is the only mechanism
# that works under Wayland, where a process cannot hook/inject another app's
# keystrokes (PHASE4.1). The core is reused verbatim; the shell renders preedit
# and commits on a word break.
# ---------------------------------------------------------------------------
if(VIETKI_LINUX AND UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(IBUS REQUIRED ibus-1.0 glib-2.0 gobject-2.0)

    add_executable(ibus-engine-vietki
        linux/src/main.cpp
        linux/src/engine_ibus.cpp
    )
    target_include_directories(ibus-engine-vietki PRIVATE linux/src ${IBUS_INCLUDE_DIRS})
    target_link_libraries(ibus-engine-vietki PRIVATE vietki_core ${IBUS_LIBRARIES})
    target_compile_options(ibus-engine-vietki PRIVATE ${IBUS_CFLAGS_OTHER})
endif()
