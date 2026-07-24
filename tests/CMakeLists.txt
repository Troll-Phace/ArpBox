# ─────────────────────────────────────────────────────────────────────────────
# ARPBOX test harness — Catch2 v3 via CTest (docs/INSTRUCTIONS.md Phase 1.2,
# docs/ARCHITECTURE.md §3.1 "Catch2 v3 via CTest", §12.5 build/test commands).
#
# One test executable (arpbox_tests) links Catch2::Catch2WithMain and the engine
# static library, so future headless engine tests (construct → prepareToPlay →
# processBlock loops) already have their link seam. Each of the five suites is a
# single Catch2 TEST_CASE tagged with its suite name; catch_discover_tests(...
# ADD_TAGS_AS_LABELS ...) maps every Catch2 [tag] to a CTest label, which is how
# the contract suites become independently selectable via `ctest -L <label>`.
#
# Contract suites (ARCHITECTURE §12.5): determinism | midi-conformance |
# hosting-lab | perf-budget, plus unit. These are smoke placeholders proving the
# labeled pipeline; later phases flesh out the real determinism/fuzzer/perf logic.
# ─────────────────────────────────────────────────────────────────────────────

include(FetchContent)

# Catch2 v3 — pinned to a stable v3 tag. v3.7.0+ is required for the
# ADD_TAGS_AS_LABELS option on catch_discover_tests (tags → CTest labels).
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.8.1
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Catch2)

# Catch2 ships its CMake integration helpers (Catch.cmake → catch_discover_tests)
# in extras/. Add it to the module path, then pull in CTest + the helper.
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# ── Test executable ──────────────────────────────────────────────────────────
# One binary carrying all suites. Catch2WithMain supplies main(); the engine lib
# is linked so headless engine tests construct real engine objects.
#
# Phase 2.4 infrastructure tests (ARCHITECTURE §3.4, §11): the SPSC FIFO / command
# / event channels, the snapshot triple buffer + retirement queue, the concurrent
# TSan stress, the headless EngineGraph render smoke, and the zero-allocation
# steady-state guard. support/AllocationCounter.cpp REPLACES the global operator
# new/delete for the allocation guard — it must live in THIS test binary only
# (never in arpbox_engine / the app); see support/AllocationSentinel.h.
add_executable(arpbox_tests
    smoke_unit.cpp
    smoke_determinism.cpp
    smoke_midi_conformance.cpp
    smoke_hosting_lab.cpp
    smoke_perf_budget.cpp
    # Phase 2.4 infrastructure test suite.
    infra_fifo.cpp
    infra_snapshot.cpp
    infra_tsan_stress.cpp
    graph_smoke.cpp
    infra_alloc_guard.cpp
    support/AllocationCounter.cpp
    support/AllocationSentinel.h)

target_compile_features(arpbox_tests PRIVATE cxx_std_20)

# tests/ as an include root so tests reach support/ headers as
# "support/AllocationSentinel.h" (engine headers come via arpbox_engine's exported
# "engine/..." include root).
target_include_directories(arpbox_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(arpbox_tests
    PRIVATE
        Catch2::Catch2WithMain
        arpbox_engine)

# Register each Catch2 TEST_CASE with CTest and turn its [tags] into CTest labels
# (ADD_TAGS_AS_LABELS). PRE_TEST discovery runs the binary at test time rather
# than post-build, which plays nicely with universal / cross-arch builds.
catch_discover_tests(arpbox_tests
    ADD_TAGS_AS_LABELS
    DISCOVERY_MODE PRE_TEST)

# ── Order-independent GUI-free enforcement (Phase 1 success criterion) ─────────
# "engine/ fails to compile if juce_gui_basics is included (enforced check)."
# The compile-time tripwire in engine/EngineGuiGuard.h is bypassable by include
# order, so it does NOT truthfully satisfy that criterion on its own. This CTest
# test is the authoritative, order-independent enforcement: a CMake script
# (tests/CheckEngineGuiFree.cmake) textually scans every engine/ source/header
# and FAILS (non-zero exit) if any file includes juce_gui_basics / juce_gui_extra
# / juce_graphics, no matter where the include sits. It runs in the default
# `ctest --test-dir build` run and in CI. Not a Catch2 case — pure CMake so it
# needs no compilation and cannot be defeated by TU ordering.
add_test(
    NAME "engine/gui-free: no GUI module includes"
    COMMAND ${CMAKE_COMMAND}
            -DENGINE_DIR=${CMAKE_SOURCE_DIR}/engine
            -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckEngineGuiFree.cmake)
set_tests_properties("engine/gui-free: no GUI module includes"
    PROPERTIES LABELS "unit")
