cmake_minimum_required(VERSION 3.22)

# ─────────────────────────────────────────────────────────────────────────────
# ARPBOX — standalone macOS generative arp workstation
# Root build superstructure. See docs/ARCHITECTURE.md §3.1 (Stack) and
# §3.2 (Module layout). Section numbers there are the source of truth.
# ─────────────────────────────────────────────────────────────────────────────

# Universal binary + deployment target MUST be set before project()/targets so
# they apply to every target (JUCE included) uniformly. (ARCHITECTURE §3.1)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build universal arm64 + x86_64")
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")

# JUCE requires C (and, on macOS, Objective-C/Objective-C++ for its .mm sources)
# in addition to C++ — see JUCE/CMakeLists.txt. LANGUAGES therefore lists more
# than the C++-only set named in the task brief.
project(ARPBOX VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)

# C++20 across the whole tree (ARCHITECTURE §3.1). Individual targets also call
# target_compile_features(... cxx_std_20) so the requirement propagates via usage
# requirements, but we pin the global default here as well.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for clang-tidy / editor tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── JUCE (pinned git submodule, tag 8.0.15) ──────────────────────────────────
add_subdirectory(JUCE)

# ── Plugin-host format enablement (ARCHITECTURE §6.1) ────────────────────────
# The VST3 + AudioUnit hosting classes are gated behind JUCE_PLUGINHOST_VST3 /
# JUCE_PLUGINHOST_AU, which BOTH default to 0 in juce_audio_processors(_headless).
# With them off, juce::addDefaultFormatsToManager() (called by our
# addProductionFormats(), hosting/PluginManager.cpp) registers ZERO formats via
# its #if JUCE_INTERNAL_HAS_VST3 / _AU guards — so a real scan finds nothing and
# reports "0 types, 0 failed". Enabling both is the fix.
#
# These flags gate the COMPILATION of the audio_processors module TUs, so they
# MUST be identical on every target that compiles that module: arpbox_engine
# (headless variant), arpbox_hosting + ARPBOX (full variant), and arpbox_tests
# (links both). If they differed, the headless-vs-full copies of the format
# classes would skew (ODR risk / impl not compiled where needed). Setting them
# ONCE here — before every add_subdirectory() below — is the single-source-of-
# truth way to keep all four targets consistent, and is inherited by the JUCE
# module sources JUCE compiles into each of those targets. Do NOT also set these
# per-target; this global definition is authoritative.
add_compile_definitions(JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=1)

# ── ARPBOX targets ───────────────────────────────────────────────────────────
# engine/  : RT engine static lib, ZERO UI deps (guarded — see engine/CMakeLists).
# hosting/ : plugin scan/instantiate/persist static lib (links FULL, GUI-bearing
#            juce_audio_processors). MUST come before app/ (app links hosting) and
#            must NOT depend on engine (see hosting/CMakeLists dependency rule).
# app/     : JUCE GUI application target.
add_subdirectory(engine)
add_subdirectory(hosting)
add_subdirectory(app)

# ── Tests: Catch2 v3 via CTest — the four contract suites + unit (Phase 1.2) ──
# BUILD_TESTING (from CMake's CTest module, default ON) gates the whole test
# tree so a plain app build can opt out with -DBUILD_TESTING=OFF. enable_testing()
# must be called in THIS top-level list file for `ctest --test-dir build` to see
# the tests registered by the tests/ subdirectory.
option(BUILD_TESTING "Build the ARPBOX test suites (Catch2 v3 via CTest)" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Placeholders filled in by later phases (do NOT create these dirs yet) ─────
# add_subdirectory(scanner-helper)   # Phase 20  — out-of-process scanner binary
# add_subdirectory(ui)               # Phase 15+ — component library, screens
