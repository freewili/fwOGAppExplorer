cmake_minimum_required(VERSION 3.28)
project(fwOGAppExplorer VERSION 1.0.0 LANGUAGES C CXX)

# What the title bar shows. Defined ONCE, here, and handed to the code as a
# compile definition rather than written into a string literal in fwApp.cpp --
# a version that lives in two places is a version that disagrees with itself.
#
# Deliberately its own string rather than being derived from PROJECT_VERSION:
# the two answer different questions. PROJECT_VERSION is semver for the build,
# and this is what a user should call the thing they are looking at. Bump it to
# "v1.1" or "v2" here and the title follows.
set(FWOG_DISPLAY_VERSION "v2")

# MSYS2's gcc precedes MSVC on some machines' PATH, so CMake can silently
# select GCC for a preset named "msvc" -- which quietly drops the static-CRT
# requirement and every MSVC-specific code path. Fail loudly instead.
if(FWOG_REQUIRE_MSVC AND NOT MSVC)
    message(FATAL_ERROR
        "This preset requires MSVC, but CMake selected "
        "${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER}).\n"
        "Start a shell with the MSVC environment first:\n"
        "  \"<Visual Studio install>\\VC\\Auxiliary\\Build\\vcvars64.bat\"\n"
        "  (or use the \"x64 Native Tools Command Prompt\" shortcut)\n"
        "then delete the stale build tree and reconfigure.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# One self-contained executable: static CRT on MSVC, so no redistributable.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Emscripten: -pthread is NOT optional for this build, and it must be set
# globally rather than per-target.
#
# Why it is needed: fwHttp.cpp's Emscripten branch uses
# EMSCRIPTEN_FETCH_SYNCHRONOUS, which the browser forbids on the main thread.
# RemoteCatalog already performs that fetch on a std::thread worker, and under
# Emscripten a std::thread IS a Web Worker -- which is exactly where a
# synchronous fetch is legal. So the existing threading is what makes the web
# HTTP path work; -pthread is what makes std::thread exist.
#
# Why global: -pthread changes the ABI (it implies -sSHARED_MEMORY and a
# different atomics/TLS lowering). Every translation unit and every fetched
# dependency -- SDL3 and Dear ImGui included -- must agree, or the link is
# silently wrong. Setting it here rather than on fwOGAppExplorer alone is the
# only way to guarantee the FetchContent subprojects pick it up too.
#
# The consequence a reader needs to know about: -sSHARED_MEMORY means the
# built page MUST be served with COOP/COEP headers or the browser refuses to
# create the SharedArrayBuffer and nothing starts. See web/README.md.
if(EMSCRIPTEN)
    # UNVERIFIED: no Emscripten SDK exists on the machine this was written on,
    # so nothing inside any if(EMSCRIPTEN) block in this file -- this one, the
    # fwfinder exclusion, the link options and the test-registration skip --
    # has ever been compiled. See web/README.md. This says nothing about the
    # rest of the file: the Windows/MSVC build below it is fully verified and
    # was kept building warning-clean throughout.
    add_compile_options(-pthread)
    add_link_options(-pthread)
endif()

# ---------------------------------------------------------------------------
# One self-contained executable, part two: the Linux half of what
# CMAKE_MSVC_RUNTIME_LIBRARY does above for Windows.
#
# The owner's requirement is "no dependencies, one exe", and the design spec's
# build table names GCC with static libstdc++/libgcc for the Linux preset.
# Without these flags a GCC build hard-depends on libstdc++.so.6 (and on
# libgcc_s.so.1 for unwinding), which is precisely the shared-library
# dependency the requirement rules out -- and the most likely one to be too old
# on the machine someone actually runs this on. libc itself is deliberately
# left dynamic: statically linking glibc breaks dlopen(), and dlopen() is how
# fwHttp.cpp reaches libcurl without linking it.
#
# add_link_options() rather than a target property so the FetchContent
# subprojects built into this executable (SDL3, fwfinder) agree with it; it is
# placed here, before the first FetchContent_MakeAvailable() below, for exactly
# that reason.
#
# RUN, no longer merely written. The linux-gcc-release preset configures,
# builds and links on GCC 16.1.1 / glibc 2.44, these two flags reach all 7 link
# lines, and `readelf -d` on the two executables it produces reports DT_NEEDED
# as exactly libudev.so.1, libm.so.6, libc.so.6 and ld-linux-x86-64.so.2 --
# neither libstdc++.so.6 nor libgcc_s.so.1 is there. (libgcc_s.so.1 is still
# mapped at run time; ldd shows it arriving as libudev's dependency, not this
# executable's.) Unlike the Emscripten paths above, this block is no longer a
# promise with no implementation behind it.
#
# The part of "no dependencies, one exe" that these flags do NOT deliver, said
# plainly rather than left for someone to discover from an ldd on a user's
# machine: libudev.so.1 is a hard, direct dependency of the executable. It
# comes from freewili-finder and it is not something this file can flag away.
#
# THAT PACKAGING DECISION HAS NOW BEEN MADE: leave it dynamic, and document it.
# There is no libudev.a to link against (the systemd package ships only the
# shared library, and `pkg-config --libs --static libudev` answers `-ludev`), so
# a static libudev would mean building systemd from source to build this app;
# libudev is LGPL-2.1-or-later, and static linking carries a relink obligation
# that a single shipped executable cannot satisfy on its own; and the third
# option -- replacing freewili-finder's libudev use with direct sysfs reads --
# is a rewrite of an upstream library's platform backend, which would fork the
# finder this app shares with the rest of the FreeWili tooling. README.md's
# "The dependency truth" states the result to users, including the display
# libraries SDL3 dlopens that never appear in DT_NEEDED at all.
#
# Note also what this executable's DT_NEEDED does NOT bound: the glibc symbol
# versions it requires. Measured here, the floor is GLIBC_2.43, and it is a
# property of the BUILD MACHINE's glibc rather than of this source -- so a
# release build belongs on the oldest distribution it is meant to run on. See
# README.md, "Will the binary run on another machine?".
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_link_options(-static-libstdc++ -static-libgcc)
    endif()
endif()
if(APPLE)
    # Every executable names fwog_core/device/platform explicitly AND inherits
    # them again through fwog_flash's PUBLIC links. Classic linkers dedup that
    # silently; the Xcode 15 linker warns about it. It is the same static
    # library either way, so silence the warning here rather than reshape
    # every target_link_libraries line around one linker's tidiness.
    # Probed rather than passed unconditionally: classic ld64 (Xcode < 15, or
    # -ld_classic) ERRORS on the unknown option, which would turn a cosmetic
    # silencing into a silent toolchain floor.
    include(CheckLinkerFlag)
    check_linker_flag(CXX "LINKER:-no_warn_duplicate_libraries"
                      FWOG_LD_ACCEPTS_NO_WARN_DUPLICATE_LIBRARIES)
    if(FWOG_LD_ACCEPTS_NO_WARN_DUPLICATE_LIBRARIES)
        add_link_options(LINKER:-no_warn_duplicate_libraries)
    endif()
endif()

include(FetchContent)
# Runs BEFORE the embedding step: probe/probe.uf2 is one of the images that
# step compiles into the executable, and there is no point embedding an image
# whose provenance has not been established. Fails the configure on drift --
# see cmake/ProbeProvenance.cmake.
include(cmake/ProbeProvenance.cmake)
include(cmake/EmbedFirmware.cmake)
include(cmake/EmbedResources.cmake)

set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ---------------------------------------------------------------------------
# fwog_core -- pure logic plus the vendored C. Links nothing but the STL, so
# the host tests can pull it in without SDL, fwfinder or a network stack.
# ---------------------------------------------------------------------------
add_library(fwog_core STATIC
    third_party/sha256/sha256.c
    third_party/miniz/miniz.c
    src/core/fwSha256.cpp
    src/core/fwTypes.h
    # settings.ini's text layer plus the remote catalog URL check the App
    # Explorer tab runs on what the user types. Pure string handling with no
    # SDL and no ImGui, so it lives here and fwog_tests can exercise the
    # write/read round trip directly instead of only through a running app.
    src/core/fwSettingsIo.h
    src/core/fwSettingsIo.cpp
    src/catalog/fwUf2Header.cpp
    src/catalog/fwCatalogJson.cpp
    src/catalog/fwCatalogMerge.cpp
    src/catalog/fwCatalogEmbedded.cpp
    src/catalog/fwCatalogLocal.cpp
    src/catalog/fwOgAppInfo.h
    src/catalog/fwOgAppInfo.cpp
    src/catalog/fwCatalogStub.h
    src/catalog/fwCatalogStub.cpp
    src/flash/fwFlashPlan.cpp
    src/flash/fwVolumeState.cpp
    src/flash/fwFlashEngine.cpp
    # The pre-plan DISPLAY quieting the GUI and the CLI both run before a MAIN
    # install. Pure over FlashIo, like the engine, so fwog_tests drives it.
    src/flash/fwFlashPrep.h
    src/flash/fwFlashPrep.cpp
    # The both-CPUs-erased recovery flow's pure logic. Here rather than in
    # fwog_flash for the usual reason: every piece of I/O it performs is
    # injected through ProbeIo, so fwog_tests can drive the whole flow --
    # including the copy to an unidentified volume -- without a board.
    src/flash/fwCpuProbe.h
    src/flash/fwCpuProbe.cpp
    # fwTabDefaultFirmwareLogic lives under src/ui/ (it's Default Firmware tab
    # logic), but is compiled here rather than into the UI executable's own
    # sources: it depends on nothing but fwTypes.h and fwCatalogEmbedded.h,
    # and keeping it here means fwog_tests can exercise it directly without
    # linking ImGui/SDL, matching the fwCatalogFilter precedent below in
    # fwog_device.
    src/ui/fwTabDefaultFirmwareLogic.h
    src/ui/fwTabDefaultFirmwareLogic.cpp
    # The .ICO parser behind the window icon, here for the same reason as the
    # line above and the fwCatalogFilter precedent: it is pure byte handling
    # with no SDL, so fwog_tests can feed it malformed containers directly.
    # Compiled on every platform even though only the non-Windows builds link
    # the SDL glue that calls it (src/ui/fwWindowIcon.cpp), so that a Windows
    # developer's ctest run still covers it.
    src/ui/fwIcoImage.h
    src/ui/fwIcoImage.cpp
    ${_gen_src}
    ${_gen_hdr}
)
add_dependencies(fwog_core fwog_generate_firmware)
target_include_directories(fwog_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sha256
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz
    ${_gen_dir}
)
target_link_libraries(fwog_core PUBLIC nlohmann_json::nlohmann_json)

# miniz.c is vendored, amalgamated third-party C. At /W4 it produces exactly
# one warning (C4132: a static const array forward-declared without an
# initializer, then defined later in the same file -- harmless upstream
# style, not a real defect). Suppress that one code at the source-file level
# rather than lowering the whole project's warning bar.
if(MSVC)
    set_source_files_properties(third_party/miniz/miniz.c PROPERTIES
        COMPILE_OPTIONS "/wd4132")
endif()


# ---------------------------------------------------------------------------
# fwog_device -- device detection: the pure USB-tree-to-CpuPortRecord mapping,
# CPU identification, and the fwfinder-backed background scanning manager.
# Split from fwog_core because fwFinderManager pulls in fwfinder, which does
# not exist for the Emscripten/web build.
# ---------------------------------------------------------------------------
if(NOT EMSCRIPTEN)
    # fwfinder defaults its own test suite to ON, which would otherwise
    # register ~33 googletest cases (live Windows device enumeration,
    # environment-dependent) into this project's `ctest` run. A failure
    # there would misread as "fwog_tests broke" and a plain ctest exit-code
    # gate could not tell the two apart. Keep this project's ctest run
    # scoped to this project's own tests.
    set(FW_FINDER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(FwFinder
        GIT_REPOSITORY https://github.com/freewili/freewili-finder.git
        GIT_TAG        master
        PATCH_COMMAND  ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/cmake/patch_fwfinder.cmake
    )
    FetchContent_MakeAvailable(FwFinder)
endif()

add_library(fwog_device STATIC
    src/device/fwDeviceRecords.cpp
    src/device/fwCpuIdentify.cpp
    # identifyBoardNow(): one fresh Fw::find_all() for the board at a uniqueID.
    # The live io.identify() behind every production flash (GUI and CLI).
    src/device/fwBoardIdentify.h
    src/device/fwBoardIdentify.cpp
    src/device/fwFinderManager.cpp
    src/device/fwDeviceModel.cpp
    # fwCatalogFilter lives under src/catalog/ (it's catalog logic), but its
    # flashDisabledReason() takes a DeviceView*, so it is compiled here rather
    # than into fwog_core. Do NOT move it back: fwog_core must keep having no
    # dependency on fwDeviceModel.h, and fwCatalogFilter.cpp would drag that
    # header in with it.
    #
    # Note this library IS built for Emscripten -- the add_library() above is
    # unconditional and only the fwfinder FetchContent above and the
    # fwfinder-static link below are guarded. fwDeviceRecords/fwCpuIdentify/
    # fwDeviceModel are pure logic that compile anywhere; fwFinderManager.cpp
    # has its own no-op stub for the web. So the reason for the split is the
    # DEPENDENCY DIRECTION (fwog_core -> nothing device-shaped), not "the web
    # build skips this library".
    src/catalog/fwCatalogFilter.cpp
)
target_link_libraries(fwog_device PUBLIC fwog_core)
if(NOT EMSCRIPTEN)
    target_link_libraries(fwog_device PUBLIC fwfinder-static)
endif()

# ---------------------------------------------------------------------------
# fwog_platform -- the real I/O behind the flash engine: paths, RPI-RP2 mass
# storage volumes, and the 1200-baud serial touch that reboots a running CPU
# into its bootloader. Thin by design; the interesting logic stays in
# fwog_core.
# ---------------------------------------------------------------------------
add_library(fwog_platform STATIC
    src/platform/fwPaths.cpp
    src/platform/fwPaths.h
    src/platform/fwVolume.cpp
    src/platform/fwVolume.h
    src/platform/fwSerialTouch.cpp
    src/platform/fwSerialTouch.h
    src/platform/fwSerialPorts.cpp
    src/platform/fwSerialPorts.h
    src/platform/fwHttp.cpp
    src/platform/fwHttp.h
    src/catalog/fwCatalogRemote.cpp
    src/catalog/fwCatalogRemote.h
)
target_include_directories(fwog_platform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(fwog_platform PUBLIC fwog_core)
if(UNIX AND NOT EMSCRIPTEN)
    target_link_libraries(fwog_platform PRIVATE ${CMAKE_DL_LIBS})
endif()
if(WIN32)
    # SetupDiGetClassDevs and friends: listSerialPortInfo() enumerates the
    # PRESENTLY ATTACHED COM ports rather than reading the stale SERIALCOMM
    # registry key. See src/platform/fwSerialPorts.h.
    target_link_libraries(fwog_platform PRIVATE setupapi)
endif()
if(APPLE)
    # IOKit is the macOS answer to what sysfs answers on Linux: which USB
    # device a /dev/cu.* port belongs to (fwSerialPorts.cpp) and how many
    # RP2040s are sitting in BOOTSEL (fwVolume.cpp). fwfinder links the same
    # pair for the same reason.
    target_link_libraries(fwog_platform PRIVATE
        "-framework IOKit" "-framework CoreFoundation"
        # DiskArbitration: the clean unmount after a flash that keeps macOS
        # from posting "Disk Not Ejected Properly" when the bootrom reboots.
        "-framework DiskArbitration")
endif()

# ---------------------------------------------------------------------------
# fwog_flash -- binds the flash engine (fwog_core) to production I/O
# (fwog_platform: real volumes, serial touch, http, paths) and to the
# selected device's identity (fwog_device: DeviceModel), and owns the worker
# thread that runs one flash at a time. Split out from fwog_platform/
# fwog_device rather than added to either: it depends on BOTH, and neither
# of those depends on the other. No ImGui: fwFlashDialog.cpp (the widget
# that reads FlashController) is compiled straight into the fwOGAppExplorer
# executable instead, same as every other ImGui-coupled .cpp in this
# project, so fwog_tests can link this library without pulling in ImGui/SDL.
# ---------------------------------------------------------------------------
add_library(fwog_flash STATIC
    src/flash/fwFlashController.h
    src/flash/fwFlashController.cpp
    # Same shape as FlashController -- worker thread, mutex-guarded queue,
    # production I/O binding -- for the CPU-identification recovery flow. A
    # separate controller on purpose; see its header comment.
    src/flash/fwCpuProbeController.h
    src/flash/fwCpuProbeController.cpp
)
target_link_libraries(fwog_flash PUBLIC fwog_core fwog_device fwog_platform)

# ---------------------------------------------------------------------------
# Application shell -- SDL3 (static) + Dear ImGui (unmodified upstream
# checkout; the Wili theme lives in our own src/ui/fwTheme.cpp, never as a
# patch to imgui itself, so imgui can be upgraded with nothing to
# re-apply), fonts and the tab shell.
#
# Built for every target INCLUDING Emscripten (Task 21). SDL3 compiles from
# source for the web -- which is exactly why -sUSE_SDL=0 is passed at link
# time below: emscripten ships a bundled SDL *2* port that would otherwise be
# linked in and collide with the SDL3 built here. Dear ImGui's SDL3 +
# SDL_Renderer3 backends both support Emscripten upstream, so the same two
# backend files serve all three platforms; only the main loop differs, and
# that is handled inside fwApp.cpp rather than by swapping entry points.
# ---------------------------------------------------------------------------
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(SDL3)
if(NOT TARGET SDL3::SDL3)
    add_library(SDL3::SDL3 ALIAS SDL3-static)
endif()

# Dear ImGui ships no CMakeLists.txt of its own at this tag; declaring it
# via FetchContent only populates ${imgui_SOURCE_DIR} (FetchContent_MakeAvailable
# skips add_subdirectory() when there is no CMakeLists.txt to add). The
# `imgui` target below is entirely ours, built from that unmodified checkout.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.1-docking
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3)
if(MSVC)
    # Vendored, unmodified upstream: not this project's warnings to fix.
    # MSVC-only on purpose -- /W3 is not a flag clang/emcc understands.
    target_compile_options(imgui PRIVATE /W3)
endif()

# The runtime half of the window icon, guarded on the SAME condition as the
# embedded .ico it decodes (cmake/EmbedResources.cmake): the two are useless
# apart, so they are switched on and off together rather than by two
# independent tests that can drift.
set(_ui_icon_src "")
if(_res_icon_src)
    set(_ui_icon_src src/ui/fwWindowIcon.h src/ui/fwWindowIcon.cpp)
endif()

add_executable(fwOGAppExplorer
    src/main.cpp
    src/ui/fwApp.h
    src/ui/fwApp.cpp
    src/ui/fwTheme.h
    src/ui/fwTheme.cpp
    src/ui/fwFonts.h
    src/ui/fwFonts.cpp
    src/ui/fwDeviceBar.h
    src/ui/fwDeviceBar.cpp
    src/ui/fwTabAppExplorer.h
    src/ui/fwTabAppExplorer.cpp
    src/ui/fwTabDefaultFirmware.h
    src/ui/fwTabDefaultFirmware.cpp
    src/ui/fwRecoveryContent.h
    src/ui/fwTabRecovery.h
    src/ui/fwTabRecovery.cpp
    src/ui/fwTabSettings.h
    src/ui/fwTabSettings.cpp
    src/ui/fwBoardImage.h
    src/ui/fwBoardImage.cpp
    # Generated: the Recovery tab's board diagram, deflated. Listed on the app
    # target rather than on fwog_core because only the UI ever draws it, and
    # fwog_tests links fwog_core without SDL.
    ${_res_board_src}
    # The window icon at runtime, and the icon bytes it decodes. Both empty on
    # Windows, where linking resources/app.rc is the whole of the icon wiring
    # -- see src/ui/fwWindowIcon.h and cmake/EmbedResources.cmake. The PURE
    # decoder is not here: fwIcoImage.cpp is in fwog_core so fwog_tests can
    # exercise it without SDL, on every platform including the one that does
    # not link this glue.
    ${_res_icon_src}
    ${_ui_icon_src}
    src/ui/fwFlashDialog.h
    src/ui/fwFlashDialog.cpp
)
# The application icon. Windows-only and guarded rather than listed above
# unconditionally: .rc is compiled by the MSVC resource compiler, which neither
# GCC on the linux preset nor emcc on the wasm preset has, and CMake would try
# to feed the file to the C compiler instead. Nothing else in the app refers to
# it -- see resources/app.rc for why no SDL_SetWindowIcon() call accompanies it.
if(WIN32 AND NOT EMSCRIPTEN)
    target_sources(fwOGAppExplorer PRIVATE resources/app.rc)
endif()
add_dependencies(fwOGAppExplorer fwog_generate_resources)
target_link_libraries(fwOGAppExplorer PRIVATE
    fwog_core fwog_platform fwog_device fwog_flash imgui SDL3::SDL3)
target_include_directories(fwOGAppExplorer PRIVATE ${CMAKE_SOURCE_DIR}/fonts)
target_compile_definitions(fwOGAppExplorer PRIVATE
    FWOG_DISPLAY_VERSION="${FWOG_DISPLAY_VERSION}")
# WIN32_EXECUTABLE (-> /SUBSYSTEM:WINDOWS) in EVERY configuration, Debug
# included. It used to be Release-only so that SDL_Log output stayed visible
# while developing, but that traded a console window flashing up behind the app
# on every single launch -- for every user, in the build they actually run --
# against developer convenience. main.cpp's WinMain forwards to main(), so
# either subsystem links.
#
# Nothing is lost on the diagnostic side: SDL_Log still goes to the debugger
# output (OutputDebugString) under /SUBSYSTEM:WINDOWS, which is where a
# developer attached to the process is already looking. Run the app from a
# terminal and you simply get no stdout, which is the normal behaviour of a
# GUI application on Windows.
#
# NOT EMSCRIPTEN is part of the condition because a Windows subsystem flag has
# no meaning for emcc and would be passed to the linker as an unknown option.
if(NOT EMSCRIPTEN)
    set_target_properties(fwOGAppExplorer PROPERTIES WIN32_EXECUTABLE ON)
endif()
# CMAKE_MSVC_RUNTIME_LIBRARY (set near the top of this file) already
# governs the static CRT for every target, this one included.

# fwTheme.cpp's applyWiliColors() is a verbatim, colour-by-colour copy of
# fwcom's ImGui::StyleColorsWili() (task brief instruction: copy every
# line as-is). One of those copied lines -- ImVec4(0.38f, 0.38, 0.38,
# 0.54f) -- has un-suffixed double literals in the source we copied from,
# which /W4 flags as C4305 (double-to-float truncation; the value itself
# is exact, this is a style nit inherited from the source, not ours to
# silently "fix" out of a verbatim transcription). Suppress at the
# source-file level rather than lowering the whole project's warning bar.
if(MSVC)
    set_source_files_properties(src/ui/fwTheme.cpp PROPERTIES
        COMPILE_OPTIONS "/wd4305")
endif()

# ---------------------------------------------------------------------------
# Emscripten link settings (Task 21). UNVERIFIED -- see web/README.md.
#
# -sUSE_SDL=0            SDL3 is built from source above; without this emcc
#                        links its own bundled SDL2 port as well.
# -sALLOW_MEMORY_GROWTH  the embedded firmware plus ImGui's font atlas make a
#                        fixed heap the wrong default.
# -sFETCH=1              required by fwHttp.cpp's emscripten_fetch path.
# -sPTHREAD_POOL_SIZE    RemoteCatalog's fetch worker and FlashController's
#                        worker each need a Worker to be available WITHOUT
#                        blocking the main thread to spawn one; 4 leaves
#                        headroom. Deliberately NOT -sPROXY_TO_PTHREAD: the
#                        synchronous fetch already has its own thread, and
#                        proxying the whole app would be solving a problem
#                        this code does not have.
# -sEXPORTED_RUNTIME_METHODS=[]
#                        nothing outside the module calls into it.
# -sSTACK_SIZE           emcc's 64KB default is small for ImGui's per-frame
#                        stack use; 4MB matches what a desktop thread gets.
# --shell-file           web/shell.html; see SUFFIX below for the .html the
#                        build actually emits.
# ---------------------------------------------------------------------------
if(EMSCRIPTEN)
    target_link_options(fwOGAppExplorer PRIVATE
        -sUSE_SDL=0
        -sALLOW_MEMORY_GROWTH=1
        -sFETCH=1
        -sPTHREAD_POOL_SIZE=4
        -sSTACK_SIZE=4194304
        "-sEXPORTED_RUNTIME_METHODS=[]"
        # SHELL: keeps the two tokens as separate argv entries and exempts
        # them from CMake's link-option de-duplication.
        "SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html"
    )
    # Without this the shell is baked in at link time but editing it does not
    # trigger a relink, so the page silently keeps serving the old markup.
    set_target_properties(fwOGAppExplorer PROPERTIES
        LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html)
    # Emits fwOGAppExplorer.html (plus the .js/.wasm/.worker.js beside it)
    # rather than a bare `fwOGAppExplorer` with no extension: emcc decides
    # what to generate from the output file's suffix, so this is what makes
    # the shell file above be used at all.
    set_target_properties(fwOGAppExplorer PROPERTIES SUFFIX ".html")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    GIT_SHALLOW    TRUE
)
# doctest v2.4.11's own CMakeLists.txt declares
# cmake_minimum_required(VERSION 2.8.12), which CMake 4.x refuses to
# evaluate ("Compatibility with CMake < 3.5 has been removed"). This does
# not change doctest's pinned tag; it only tells CMake which policy set to
# apply while configuring that vendored subdirectory.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(doctest)
unset(CMAKE_POLICY_VERSION_MINIMUM)

add_executable(fwog_tests
    tests/main.cpp
    tests/test_fwSha256.cpp
    tests/test_fwUf2Header.cpp
    tests/test_fwOgAppInfo.cpp
    tests/test_fwCatalogStub.cpp
    tests/test_fwCpuIdentify.cpp
    tests/test_fwFlashPlan.cpp
    tests/test_fwVolumeState.cpp
    tests/test_fwFlashEngine.cpp
    tests/test_fwCatalogJson.cpp
    tests/test_fwCatalogMerge.cpp
    tests/test_fwCatalogEmbedded.cpp
    tests/test_fwTabDefaultFirmwareLogic.cpp
    tests/test_fwCatalogLocal.cpp
    tests/test_fwCatalogRemote.cpp
    tests/test_fwPaths.cpp
    tests/test_fwVolume.cpp
    tests/test_fwHttp.cpp
    tests/test_fwDeviceRecords.cpp
    tests/test_fwDeviceModel.cpp
    tests/test_fwFinderManager.cpp
    tests/test_fwCatalogFilter.cpp
    tests/test_fwFlashController.cpp
    tests/test_fwSettingsIo.cpp
    tests/test_fwCpuProbe.cpp
    tests/test_fwSerialPorts.cpp
    tests/test_fwIcoImage.cpp
    tests/test_fwFlashPrep.cpp
)
target_link_libraries(fwog_tests PRIVATE fwog_core fwog_device fwog_platform fwog_flash doctest::doctest)

if(EMSCRIPTEN)
    # -sFETCH is needed here too, not just on the app: fwog_platform pulls in
    # fwHttp.cpp's emscripten_fetch path and test_fwHttp.cpp exercises it.
    # EXIT_RUNTIME so doctest's exit code actually reaches the shell when this
    # is run by hand under node -- otherwise a failing suite exits 0.
    target_link_options(fwog_tests PRIVATE
        -sALLOW_MEMORY_GROWTH=1
        -sFETCH=1
        -sPTHREAD_POOL_SIZE=4
        -sSTACK_SIZE=4194304
        -sEXIT_RUNTIME=1
    )
endif()

# Built for Emscripten too -- the suite is pure logic and links no UI, so it
# is the cheapest way for someone with an emsdk to find out whether this
# project's own code actually compiles for the web -- but not REGISTERED with
# ctest there: the output is a .js that needs node with the right flags, not
# something the host can execute, and a ctest entry that always fails to
# launch would say nothing useful about the code.
if(NOT EMSCRIPTEN)
    add_test(NAME fwog_tests COMMAND fwog_tests)
endif()

# ---------------------------------------------------------------------------
# fwogcli -- command-line flasher. A console front-end over the SAME flash
# engine the GUI uses (fwog_flash: runFlashPlan + makeProductionFlashIo), so a
# scripted flash gets identical touch/wait/hub-port-disambiguation behaviour
# with none of SDL/ImGui. Links exactly what fwog_tests does -- proof this
# path is SDL-free -- and is a plain CONSOLE subsystem exe (no WIN32_EXECUTABLE)
# so its stdout reaches the shell. Not built for the web: it needs USB/serial.
# ---------------------------------------------------------------------------
if(NOT EMSCRIPTEN)
    add_executable(fwogcli src/cli/fwCli.cpp)
    target_link_libraries(fwogcli PRIVATE fwog_core fwog_device fwog_platform fwog_flash)
endif()
