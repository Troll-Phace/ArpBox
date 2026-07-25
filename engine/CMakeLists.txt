# ─────────────────────────────────────────────────────────────────────────────
# arpbox_engine — RT engine static library (ARCHITECTURE §3.2)
#
# ZERO UI dependencies. This target links ONLY non-GUI JUCE modules; it must
# NEVER link (or transitively pull) juce_gui_basics / juce_gui_extra /
# juce_graphics — hence juce_audio_processors_headless below, not the full
# module. NOTE: JUCE puts all modules under one include root, so the link set
# alone does not stop a stray GUI #include — the compile-time guard in
# engine/EngineGuiGuard.h (gated on ARPBOX_ENGINE_BUILD) is the real enforcer.
#
# Module subdirectories from §3.2 (graph/ sequencer/ generative/ midi/) are
# populated by later phases. Phase 1 ships a single real translation unit in
# graph/ so the library compiles.
# ─────────────────────────────────────────────────────────────────────────────

add_library(arpbox_engine STATIC
    graph/EnginePlaceholder.cpp
    graph/EnginePlaceholder.h
    # Phase 2.3 cross-thread infrastructure (ARCHITECTURE §3.4) — the ONLY
    # cross-thread channels for the whole project. Header-only; listed here so
    # they surface in IDEs and travel with the target. EnginePlaceholder.cpp
    # remains the library's real translation unit.
    graph/SpscFifo.h
    graph/EngineCommand.h
    graph/EngineSnapshot.h
    graph/EngineSnapshotBuffer.h
    graph/EngineEvent.h
    graph/NoteEvent.h
    graph/RetirementQueue.h
    # Phase 2.2 master section, test-tone source, and root graph assembly
    # (ARCHITECTURE §3.3, §7). No GUI deps — juce_dsp + headless audio_processors.
    graph/DeviceStatus.h
    graph/ToneControl.h
    graph/TestToneProcessor.h
    graph/TestToneProcessor.cpp
    graph/MasterProcessor.h
    graph/MasterProcessor.cpp
    # Phase 4 MIDI-in path (ARCHITECTURE §3.3): QWERTY/pad note channel + the
    # MIDI-only merge/filter node feeding the synth slot.
    graph/MidiInputControl.h
    graph/MidiInputProcessor.h
    graph/MidiInputProcessor.cpp
    graph/EngineGraph.h
    graph/EngineGraph.cpp
    EngineGuiGuard.h)

target_compile_features(arpbox_engine PUBLIC cxx_std_20)

# ── Public include scoping (issue #1) ────────────────────────────────────────
# Consumers include engine headers as  #include "engine/graph/Foo.h"  (see
# app/Main.cpp, tests/smoke_unit.cpp). That prefix needs a root directory that
# has `engine/` as a child. Exporting the REPO ROOT (../) for that purpose leaks
# EVERY sibling module (hosting/, ui/, packaging/, docs/, …) onto every
# consumer's header search path — the defect tracked in issue #1.
#
# Fix: synthesize a private, single-entry include root in the build tree that
# contains ONLY `engine` -> this source dir (a symlink), and export THAT. The
# `engine/...` include convention keeps working, but nothing outside engine/ is
# reachable through this target. Internal engine TUs use relative includes
# (e.g. "../EngineGuiGuard.h"), so no in-tree bare-prefix export is needed.
set(_engine_include_root "${CMAKE_CURRENT_BINARY_DIR}/include")
file(MAKE_DIRECTORY "${_engine_include_root}")
if(NOT EXISTS "${_engine_include_root}/engine")
    file(CREATE_LINK
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${_engine_include_root}/engine"
        SYMBOLIC)
endif()

target_include_directories(arpbox_engine
    PUBLIC
        ${_engine_include_root})

# Semantic version string surfaced by EnginePlaceholder::getEngineVersion().
target_compile_definitions(arpbox_engine
    PUBLIC
        ARPBOX_ENGINE_VERSION="${PROJECT_VERSION}"
    PRIVATE
        # Marks a translation unit as part of the engine library. The GUI-free
        # compile guard (engine/EngineGuiGuard.h) is active ONLY under this
        # define, so the guard enforces on engine TUs without false-firing in
        # app/ or ui/ TUs that legitimately include both engine + GUI headers.
        ARPBOX_ENGINE_BUILD=1)

# NON-GUI JUCE modules only. Linked PUBLIC so the app inherits the module
# availability defines it needs to compile engine headers.
#
# HARD RULE (code-style.md, ARCHITECTURE §3.2): do NOT add juce_gui_basics,
# juce_gui_extra, or juce_graphics here — ever.
target_link_libraries(arpbox_engine
    PUBLIC
        juce::juce_core
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        # HEADLESS audio_processors: hosts/loads VST3/AU WITHOUT the GUI editor
        # classes. The full juce_audio_processors depends on juce_gui_extra
        # (→ juce_gui_basics + juce_graphics), which would drag the whole GUI
        # stack into the "UI-free" engine. The app links the full module for
        # plugin editor windows (ARCHITECTURE §6.4); the engine stays headless.
        juce::juce_audio_processors_headless
        juce::juce_dsp
    PRIVATE
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Web/curl features belong to GUI/network layers, not the engine. Disable so the
# audio_processors module never pulls them.
target_compile_definitions(arpbox_engine
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
