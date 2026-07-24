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

# ── ARPBOX targets ───────────────────────────────────────────────────────────
# engine/ : RT engine static lib, ZERO UI deps (guarded — see engine/CMakeLists).
# app/    : JUCE GUI application target.
add_subdirectory(engine)
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
# add_subdirectory(hosting)          # Phase 3+  — plugin scan/host/wrappers
# add_subdirectory(scanner-helper)   # Phase 20  — out-of-process scanner binary
# add_subdirectory(ui)               # Phase 15+ — component library, screens
