#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// engine/ MUST NOT depend on JUCE GUI modules (ARCHITECTURE §3.2, code-style.md
// "Module organization"). The engine is a UI-free static library so it can be
// unit-tested headless and reused (plugin / multi-track builds later).
//
// Enforcement — the AUTHORITATIVE check is the repo-wide grep test, NOT this
// header:
//
//   The compile-time guard below is only a best-effort tripwire. It works by
//   testing whether a GUI module's include-guard macro is already defined when
//   this header is processed, so it can ONLY catch a GUI include that appears
//   *before* this header in the translation unit. A stray
//   `#include <juce_gui_basics/...>` placed AFTER this header compiles cleanly
//   and slips past — include order defeats it. Treat it as defense-in-depth,
//   not as the thing that guarantees the engine stays UI-free.
//
//   The order-independent, authoritative enforcement is a grep-based test wired
//   into CTest + CI: it scans engine/ sources for forbidden GUI includes
//   regardless of where they sit in a file. That test is what actually fails
//   the build when the boundary is crossed; this header is a fast local hint.
//
//   The CMake link boundary (arpbox_engine links juce_audio_processors_headless,
//   never the GUI modules) is also complementary: JUCE exposes every module
//   under one shared include root (JUCE/modules), so linking only non-GUI
//   modules does NOT remove the GUI headers from the engine's include path — a
//   stray `#include <juce_gui_basics/...>` would still resolve. The link
//   boundary keeps GUI object code and symbols out of the engine library, but
//   is not what stops a stray #include.
//
// Every engine translation unit / public header should include this guard early
// (before any JUCE includes) to give the tripwire the best chance of firing.
//
// The checks are gated on ARPBOX_ENGINE_BUILD (defined PRIVATE on the
// arpbox_engine CMake target). That macro is set ONLY while compiling the
// engine library's own translation units, so the guard enforces there but does
// NOT false-fire when app/ or ui/ code includes an engine header alongside the
// GUI modules it legitimately uses.
//
// NOTE: because the tripwire is order-dependent, adding
//     #include <juce_gui_basics/juce_gui_basics.h>
// AFTER this header will NOT fail the build — rely on the grep test in
// CI/CTest to prove the engine is GUI-free.
// ─────────────────────────────────────────────────────────────────────────────

#if defined(ARPBOX_ENGINE_BUILD)

#    if defined(JUCE_GUI_BASICS_H_INCLUDED)
#        error "engine/ must not depend on juce_gui_basics (ARCHITECTURE §3.2 — the engine is UI-free)"
#    endif

#    if defined(JUCE_GUI_EXTRA_H_INCLUDED)
#        error "engine/ must not depend on juce_gui_extra (ARCHITECTURE §3.2 — the engine is UI-free)"
#    endif

#    if defined(JUCE_GRAPHICS_H_INCLUDED)
#        error "engine/ must not depend on juce_graphics (ARCHITECTURE §3.2 — the engine is UI-free)"
#    endif

#endif // ARPBOX_ENGINE_BUILD
