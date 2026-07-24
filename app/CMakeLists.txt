# ─────────────────────────────────────────────────────────────────────────────
# ARPBOX — JUCE GUI application target (ARCHITECTURE §3.2)
#
# Owns the AudioDeviceManager, AudioProcessorPlayer, main window, and app
# lifecycle (wired in Phase 2+). Phase 1 ships an empty resizable window.
# ─────────────────────────────────────────────────────────────────────────────

juce_add_gui_app(ARPBOX
    PRODUCT_NAME "ARPBOX"
    COMPANY_NAME "ARPBOX"
    BUNDLE_ID "com.arpbox.arpbox"
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE
    NEEDS_STORE_KIT FALSE)

target_sources(ARPBOX PRIVATE
    Main.cpp
    MainWindow.cpp
    MainWindow.h
    AudioEngine.cpp
    AudioEngine.h
    SynthSlot.cpp
    SynthSlot.h
    DebugPanel.cpp
    DebugPanel.h)

target_compile_features(ARPBOX PUBLIC cxx_std_20)

# Direct module includes (no Projucer/JuceHeader coupling). These defines stand
# in for the settings JuceHeader.h would otherwise inject.
target_compile_definitions(ARPBOX
    PRIVATE
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:ARPBOX,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:ARPBOX,JUCE_VERSION>"
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1)

# arpbox_engine first (brings the non-GUI JUCE modules), then the GUI modules
# the app itself needs.
#
# juce_audio_utils supplies AudioProcessorPlayer (the device→graph host). It
# depends on the FULL juce_audio_processors, which in turn depends on the same
# juce_audio_processors_headless the engine links — JUCE builds each module once,
# so this layers on top without duplicating the engine's headless symbols. This
# module is GUI-side ONLY; it is NEVER added to the engine (which stays headless).
target_link_libraries(ARPBOX
    PRIVATE
        arpbox_engine
        # Plugin hosting (§6): scan/instantiate/persist. Links the FULL, GUI-bearing
        # juce_audio_processors and PUBLIC-exports the "hosting/..." include root, so
        # app/ can #include "hosting/PluginManager.h". Consistent with the full
        # juce_audio_processors the app already pulls via juce_audio_utils.
        arpbox_hosting
        juce::juce_audio_utils
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# ─────────────────────────────────────────────────────────────────────────────
# Code signing — Hardened Runtime + plugin-hosting entitlements (§3.1)
#
# ARPBOX must run under the Hardened Runtime WITH the
# disable-library-validation entitlement so it can load third-party VST3/AU
# plugins. JUCE ships the app ad-hoc / linker-signed with NO hardened runtime
# and NO entitlements, so we re-sign as a POST_BUILD step.
#
# Primary generator is Ninja, under which the XCODE_ATTRIBUTE_* properties are
# inert — they are set anyway so an Xcode-generator build (or IDE inspection)
# picks up the same intent. The POST_BUILD `codesign` below is what actually
# signs the Ninja build.
#
#   ARPBOX_CODESIGN_IDENTITY — signing identity passed to `codesign --sign`.
#     Defaults to "-" (ad-hoc): correct for local dev and CI without secrets.
#     In a release pipeline, pass a Developer ID:
#       -DARPBOX_CODESIGN_IDENTITY="Developer ID Application: <Name> (<TEAMID>)"
# ─────────────────────────────────────────────────────────────────────────────
set(ARPBOX_ENTITLEMENTS "${CMAKE_SOURCE_DIR}/packaging/ArpBox.entitlements"
    CACHE FILEPATH "Path to the Hardened Runtime entitlements plist")
set(ARPBOX_CODESIGN_IDENTITY "-"
    CACHE STRING "codesign identity for the ARPBOX app ('-' = ad-hoc)")

# Intent for the Xcode generator (no effect under Ninja, harmless to set).
set_target_properties(ARPBOX PROPERTIES
    XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME YES
    XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${ARPBOX_ENTITLEMENTS}"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${ARPBOX_CODESIGN_IDENTITY}")

if(APPLE)
    # Re-sign the finished bundle with Hardened Runtime (--options runtime) and
    # the entitlements. --force overrides JUCE's linker-signed ad-hoc signature.
    # The bundle has no nested frameworks (verified), so a single-shot sign of
    # the .app is sufficient — no inside-out / --deep pass needed.
    add_custom_command(TARGET ARPBOX POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E echo
            "== Codesigning ARPBOX.app (identity='${ARPBOX_CODESIGN_IDENTITY}', hardened runtime + entitlements) =="
        COMMAND codesign --force --options runtime
            --entitlements "${ARPBOX_ENTITLEMENTS}"
            --sign "${ARPBOX_CODESIGN_IDENTITY}"
            "$<TARGET_BUNDLE_DIR:ARPBOX>"
        COMMAND codesign -dv --verbose=4 "$<TARGET_BUNDLE_DIR:ARPBOX>"
        VERBATIM
        COMMENT "Signing ARPBOX.app with Hardened Runtime + plugin-hosting entitlements")
endif()
