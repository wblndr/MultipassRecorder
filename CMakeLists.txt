cmake_minimum_required(VERSION 3.16)
# CMP0091: honour MSVC_RUNTIME_LIBRARY below instead of baking /MD into CMAKE_CXX_FLAGS.
# Must be set before project().
cmake_policy(SET CMP0091 NEW)
project(MultipassRecorderAddon)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Instructions to fetch ReShade SDK:
# 1. Initialize a git repository if not done: git init
# 2. Add ReShade as a submodule:
#    git submodule add https://github.com/crosire/reshade.git deps/reshade
# 3. Update the submodule:
#    git submodule update --init --recursive
# ---------------------------------------------------------------------------

add_library(MultipassRecorderAddon SHARED 
    main.cpp
)

# Define the Addon target
set_target_properties(MultipassRecorderAddon PROPERTIES 
    SUFFIX ".addon"
    PREFIX ""
)

# If you have ReShade SDK locally, adjust this path. 
# Defaults to a submodule in deps/reshade/
#
# SYSTEM on both: ReShade's and ImGui's headers are third-party and noisy at /W4
# (see the warning-level block below). Marking them system keeps the warnings we
# turn on pointed at this project's own code, which is the only code we can fix.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/reshade/source")
    target_include_directories(MultipassRecorderAddon SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/deps/reshade/source" "${CMAKE_CURRENT_SOURCE_DIR}/deps/reshade/include")
else()
    # FATAL, not WARNING: without the SDK the build fails anyway, but as a wall of
    # opaque "cannot open source file reshade.hpp" errors from a unity build. Say
    # what is actually wrong, at configure time, with the command that fixes it.
    message(FATAL_ERROR
        "ReShade SDK not found in deps/reshade/.\n"
        "Fetch it with:\n"
        "  git submodule update --init --recursive\n"
        "  git -C deps/reshade checkout v6.5.1\n"
        "  git -C deps/reshade submodule update --init deps/imgui\n"
        "(pin v6.5.1 - see CLAUDE.md for why not main)")
endif()

# We need imgui matching the ReShade one
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/reshade/deps/imgui")
    target_include_directories(MultipassRecorderAddon SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/deps/reshade/deps/imgui")
else()
    message(FATAL_ERROR
        "ImGui not found in deps/reshade/deps/imgui/.\n"
        "Fetch it with:  git -C deps/reshade submodule update --init deps/imgui\n"
        "(the overlay needs an ImGui the target ReShade runtime accepts - CLAUDE.md, "
        "'ImGui version coupling')")
endif()

# We don't link any explicit ReShade lib, it's an additive DLL for exactly API.
# Just need to build as SHARED or MODULE.
target_compile_definitions(MultipassRecorderAddon PRIVATE ImTextureID=ImU64)

# ---------------------------------------------------------------------------
# Warning level.
#
# cmake_minimum_required(3.16) sets CMP0092 to NEW, so CMake no longer injects
# its historical /W3 — with no flag of our own the effective level was /W1. For a
# heavily threaded unity build that gets injected into someone else's process,
# the /W4 diagnostics (truncation, shadowing, signed/unsigned, unreachable code,
# uninitialised locals) are exactly the class of bug that shows up as a crash in
# a game's SEH handler with no addon frames in the dump.
#
# Deliberately NOT /WX: a warning must never be the reason a deploy can't be
# built. And deliberately not /permissive- — untested against this SDK.
# ---------------------------------------------------------------------------
if(MSVC)
    target_compile_options(MultipassRecorderAddon PRIVATE /W4)
    # /external:* (VS 2019 16.10 / MSVC 19.29+) is what makes /W4 usable here: it
    # silences everything reached through <angle brackets> — the Windows SDK,
    # <reshade.hpp>, <imgui.h> — while our own "mpr_*.inc" fragments, included
    # with quotes, keep full /W4. Belt and braces with the SYSTEM include dirs
    # above, since CMake only maps SYSTEM to /external:I on newer CMake.
    if(MSVC_VERSION GREATER_EQUAL 1929)
        target_compile_options(MultipassRecorderAddon PRIVATE /external:W0 /external:anglebrackets)
    endif()
endif()

# ---------------------------------------------------------------------------
# STATIC CRT — load-bearing, do not switch back to /MD.
#
# An addon is injected into whatever process the game happens to be. Games ship
# their own MSVC redistributable next to the .exe, and the game directory wins
# the DLL search — so a /MD build binds to THAT msvcp140.dll, not the system one.
#
# BO6 (sp24) ships msvcp140.dll 14.34.31948.0. We build with VS 2022 17.14, where
# std::mutex's constructor is constexpr (17.10+): it only zero-inits, and no longer
# calls _Mtx_init_in_situ. The 14.34 runtime still expects that initialization, so
# every std::mutex::lock() dereferenced null and raised 0xC0000005 inside
# msvcp140.dll — which BO6's SEH frame-loop handler caught, unwound, and turned
# into a multi-GB crash dump that looked exactly like a startup freeze.
# That hit q_mutex/pool_mutex, i.e. the recording hot path, not just teardown.
#
# Linking the CRT statically means the addon carries its own STL and cannot bind
# to a game's redistributable at all — this fixes it for every game, not just BO6.
# Safe here because no CRT object (allocation, FILE*, iostream) ever crosses the
# DLL boundary: we talk to ReShade through its C-style API and to FFmpeg via pipes.
# ---------------------------------------------------------------------------
set_property(TARGET MultipassRecorderAddon PROPERTY
             MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Belt and braces: if anyone ever reverts to a dynamic CRT, this keeps std::mutex
# ABI-compatible with older redistributables instead of crashing on lock().
target_compile_definitions(MultipassRecorderAddon PRIVATE _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)

# ---------------------------------------------------------------------------
# ReShade addon API version to target. A single binary at this version covers
# all our ReShade 6.x games. Override for a legacy (ReShade 5.x) host build:
#   cmake -B build64 -A x64 -DMPR_TARGET_API_VERSION=8
# See the comment block at the top of main.cpp for what each version unlocks.
# ---------------------------------------------------------------------------
set(MPR_TARGET_API_VERSION 13 CACHE STRING "ReShade addon API version to target")
target_compile_definitions(MultipassRecorderAddon PRIVATE MPR_TARGET_API_VERSION=${MPR_TARGET_API_VERSION})
