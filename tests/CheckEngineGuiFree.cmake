# ─────────────────────────────────────────────────────────────────────────────
# CheckEngineGuiFree.cmake — order-independent enforcement of the UI-free engine.
#
# ARCHITECTURE §3.2 / code-style.md "Module organization": engine/ has ZERO UI
# dependencies. The compile-time tripwire in engine/EngineGuiGuard.h is only
# best-effort — it fires solely when a forbidden GUI include precedes it in the
# translation unit, so include order defeats it. This script is the AUTHORITATIVE
# check: it textually scans every engine/ source and header for an include of a
# forbidden JUCE GUI module regardless of position, and FAILS (non-zero exit via
# message(FATAL_ERROR)) if any is found.
#
# Registered with CTest in tests/CMakeLists.txt as "engine/gui-free: no GUI module
# includes" and invoked as:
#   cmake -DENGINE_DIR=<path/to/engine> -P CheckEngineGuiFree.cmake
# ─────────────────────────────────────────────────────────────────────────────

if(NOT DEFINED ENGINE_DIR)
    message(FATAL_ERROR "CheckEngineGuiFree: ENGINE_DIR must be defined (-DENGINE_DIR=...).")
endif()

if(NOT IS_DIRECTORY "${ENGINE_DIR}")
    message(FATAL_ERROR "CheckEngineGuiFree: ENGINE_DIR '${ENGINE_DIR}' is not a directory.")
endif()

# Recursively collect engine C++ sources and headers.
file(GLOB_RECURSE engine_files
    "${ENGINE_DIR}/*.h"
    "${ENGINE_DIR}/*.hpp"
    "${ENGINE_DIR}/*.cpp"
    "${ENGINE_DIR}/*.cc"
    "${ENGINE_DIR}/*.mm")

# An #include of any forbidden GUI module, in either <...> or "..." form.
# Matches e.g.  #include <juce_gui_basics/juce_gui_basics.h>
#               #  include "juce_graphics/foo.h"
# Anchored to line start (^ + optional leading whitespace) so it matches only a
# real preprocessor directive — a commented mention like `//   #include <...>`
# has `//` before the `#` and is correctly ignored. This is what lets the guard
# header's own documentation (which quotes these include lines in prose) pass.
set(forbidden_regex "^[ \t]*#[ \t]*include[ \t]*[<\"]juce_(gui_basics|gui_extra|graphics)")

set(violations "")

foreach(file IN LISTS engine_files)
    # EngineGuiGuard.h documents and #errors on these module names; its own text
    # legitimately contains the tokens but never actually *includes* the modules.
    # The regex requires a real #include directive, so prose mentions and the
    # #error string literals in the guard do not match. No file is exempted —
    # if a genuine forbidden #include appears anywhere under engine/, it fails.
    file(STRINGS "${file}" matched_lines REGEX "${forbidden_regex}")
    if(matched_lines)
        foreach(line IN LISTS matched_lines)
            string(APPEND violations "  ${file}: ${line}\n")
        endforeach()
    endif()
endforeach()

if(NOT violations STREQUAL "")
    message(FATAL_ERROR
        "engine/ must be UI-free (ARCHITECTURE §3.2) but forbidden JUCE GUI module "
        "includes were found:\n${violations}"
        "Remove these includes — the engine static library must not depend on "
        "juce_gui_basics / juce_gui_extra / juce_graphics.")
endif()

message(STATUS "engine/gui-free: no forbidden GUI module includes found under ${ENGINE_DIR}")
