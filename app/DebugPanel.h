// TEMPORARY DEBUG UI — replaced by the real UI in Phase 15+.
#pragma once

#include "AudioEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>

namespace arpbox::app
{
/** Throwaway Phase-2 debug surface (INSTRUCTIONS.md Phase 2 success criteria):
    proves the test tone runs through the graph to the device and shows live
    meter values / device status in a plain readout.

    Deliberately crude default-JUCE look — no `ui/Tokens.h` (that arrives in
    Phase 15). It talks to the engine ONLY through the canonical channels (§10.1):
    it PUSHES `EngineCommand`s and READS the `EngineSnapshot`/event queue; it never
    touches audio-thread state directly.

    The 60 fps read uses `juce::VBlankAttachment`, NOT `juce::Timer` (prohibited —
    see .claude/rules/code-style.md).

    MESSAGE-THREAD ONLY. */
class DebugPanel final : public juce::Component
{
public:
    // MESSAGE-THREAD ONLY: builds the controls and starts the vblank read.
    /** @param engine the app audio backbone (non-owning; must outlive this). */
    explicit DebugPanel (AudioEngine& engine);

    /** ~DebugPanel. */
    ~DebugPanel () override = default;

    /** Lays the debug controls out in a simple vertical stack. */
    void resized () override;

    /** Paints the plain background. */
    void paint (juce::Graphics& g) override;

private:
    // MESSAGE-THREAD ONLY (vblank): reads the newest snapshot, drains events, and
    // refreshes the meter / status / starvation readouts.
    void refreshFromEngine ();

    // MESSAGE-THREAD ONLY: pushes a float-payload command onto the queue.
    void pushFloat (engine::EngineCommandType type, float value);

    // MESSAGE-THREAD ONLY: pushes an int-payload command onto the queue.
    void pushInt (engine::EngineCommandType type, std::int32_t value);

    AudioEngine& audioEngine; ///< Non-owning; owned by the application.

    // ── Controls (push commands) ─────────────────────────────────────────────
    juce::ToggleButton testToneButton { "Test Tone" };
    juce::Slider frequencySlider;   ///< Tone frequency in Hz.
    juce::Slider masterGainSlider;  ///< Master gain in dB.
    juce::ToggleButton limiterButton { "Safety Limiter" };
    juce::TextButton simulateLossButton { "Simulate Device Loss" };

    juce::Label frequencyLabel { {}, "Tone Hz" };
    juce::Label masterGainLabel { {}, "Gain dB" };

    // ── Readouts (from snapshots/events) ─────────────────────────────────────
    juce::Label deviceLabel;   ///< Active device name / SR / buffer.
    juce::Label statusBanner;  ///< Device-status banner (OK / fell back / dead).
    juce::Label meterLabel;    ///< Peak / RMS in dB.
    juce::Label blockLabel;    ///< Block counter + STARVED indicator.
    juce::Label eventLabel;    ///< Last drained engine event + dropped-command count.

    std::uint64_t lastBlockCounter = 0; ///< For starvation detection frame-over-frame.
    std::uint64_t engineEventCount = 0; ///< Total engine events drained since launch.
    juce::String lastEventText { "none" };

    // 60 fps snapshot read (message thread). Declared LAST so it is destroyed
    // FIRST — the callback must never fire after the members it touches are gone.
    juce::VBlankAttachment vblank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DebugPanel)
};
} // namespace arpbox::app
