# ─────────────────────────────────────────────────────────────────────────────
# arpbox_hosting — plugin discovery / instantiation / persistence (ARCHITECTURE §6)
#
# Owns all AudioPluginInstance lifecycle: scanning (KnownPluginList behind the
# CustomScanner seam), async instantiation, and known-list persistence. Phase 3
# scope; the HostedPluginNode wrapper (Phase 4) and editor windows (Phase 10)
# extend this target later.
#
# DEPENDENCY DIRECTION (hard rule — plan "Risk areas"): app → hosting → JUCE and
# app → engine → JUCE. hosting must NOT depend on arpbox_engine, and engine must
# NEVER include anything from hosting/ (that would trip the engine GUI-free guard,
# since hosting links the FULL, GUI-bearing juce_audio_processors). Keep this
# target free of any include back into engine.
# ─────────────────────────────────────────────────────────────────────────────

add_library(arpbox_hosting STATIC
    InstantiationResult.h
    InProcessScanner.h
    InProcessScanner.cpp
    PluginManager.h
    PluginManager.cpp
    PluginInstantiator.h
    PluginInstantiator.cpp)

target_compile_features(arpbox_hosting PUBLIC cxx_std_20)

# ── Public include scoping (issue #1 fix pattern — mirrors engine/CMakeLists) ──
# Consumers include hosting headers as  #include "hosting/PluginManager.h" . That
# prefix needs a root directory that has `hosting/` as a child. Exporting the repo
# root (../) would leak EVERY sibling module onto consumers' header search paths
# (the issue-#1 defect). Instead synthesize a private, single-entry include root
# in the build tree containing ONLY `hosting` -> this source dir (a symlink), and
# export THAT. Internal hosting TUs use the same "hosting/..." convention (they see
# this root as their own consumer), so no bare-prefix export is needed.
set(_hosting_include_root "${CMAKE_CURRENT_BINARY_DIR}/include")
file(MAKE_DIRECTORY "${_hosting_include_root}")
if(NOT EXISTS "${_hosting_include_root}/hosting")
    file(CREATE_LINK
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${_hosting_include_root}/hosting"
        SYMBOLIC)
endif()

target_include_directories(arpbox_hosting
    PUBLIC
        ${_hosting_include_root})

# JUCE modules. Unlike the engine (which links juce_audio_processors_HEADLESS to
# stay UI-free), hosting links the FULL juce_audio_processors — it owns plugin
# editor windows (ARCHITECTURE §6.4) from Phase 10, and the full module also
# supplies the message-thread instantiation plumbing used here. The app already
# links the full module transitively (via juce_audio_utils), so this is
# consistent; JUCE builds each module once and the full variant layers on the
# headless one (no duplicate symbols).
#
# Linked PUBLIC so consumers (app, tests) inherit the module-availability defines
# needed to compile hosting headers.
target_link_libraries(arpbox_hosting
    PUBLIC
        juce::juce_core
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_audio_processors
    PRIVATE
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Web/curl belong to GUI/network layers, not hosting. Disable so the
# audio_processors module never pulls them (matches engine + app).
target_compile_definitions(arpbox_hosting
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
