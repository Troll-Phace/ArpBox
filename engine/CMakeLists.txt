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
    EngineGuiGuard.h)

target_compile_features(arpbox_engine PUBLIC cxx_std_20)

# Engine headers use include paths rooted at the repo's engine/ parent, e.g.
# #include "engine/graph/Foo.h" from outside, or relative from within engine/.
target_include_directories(arpbox_engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..)

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
