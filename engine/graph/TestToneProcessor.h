#pragma once

#include "../EngineGuiGuard.h"
#include "ToneControl.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

namespace arpbox::engine
{
/** Command-gated debug sine-tone SOURCE node (ARCHITECTURE §3.3).

    Occupies the graph slot the hosted synth will take in Phase 4: it is an
    instrument-style source with NO audio input and a stereo output, wired as
    `TestTone → Master → audioOutput`. Its only job in Phase 2 is to prove signal
    flow all the way to the device — a controllable tone lets 2.1/2.4 verify the
    meter tap and the device path without a real plugin.

    The tone is OFF by default and controlled entirely through the shared
    `ToneControl` block (see ToneControl.h for why control crosses nodes via
    atomics rather than a direct command drain). Synthesis is a hand-rolled
    phasor: zero allocation, no wavetable, phase continuous across blocks. */
class TestToneProcessor : public juce::AudioProcessor
{
public:
    /** Constructs the node with a stereo output bus and no input bus. */
    TestToneProcessor ();

    /** ~TestToneProcessor. */
    ~TestToneProcessor () override = default;

    // MESSAGE-THREAD ONLY: wiring. Injects the shared control block the graph
    // owns. Call once, before the node is added to the graph and before playback.
    /** Sets the non-owning pointer to the graph-owned `ToneControl` the audio
        thread reads each block. The pointed-to object must outlive this node. */
    void setToneControl (ToneControl* control) noexcept { toneControl = control; }

    // ── AudioProcessor overrides ─────────────────────────────────────────────

    /** Returns the node's display name. */
    const juce::String getName () const override { return "ARPBOX Test Tone"; }

    // MESSAGE-THREAD ONLY: allocates/prepares SR-dependent state. Never on audio.
    /** Caches the sample rate and resets the phasor. No allocation happens in
        `processBlock`; all rate-dependent state is finalised here. */
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;

    // MESSAGE-THREAD ONLY: release. Nothing heap-held; a no-op today.
    /** Releases resources (none held); safe to call when unprepared. */
    void releaseResources () override {}

    // RT-SAFE: audio thread. Allocation-free, lock-free, no logging.
    /** Generates the tone (or silence when disabled) into `buffer`. Clears the
        buffer first (source node), then, if enabled, fills every channel with a
        sine at the current control frequency and a fixed debug level. */
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    // RT-SAFE: audio thread. The graph runs float; this double path is unused.
    /** Double-precision path — the graph processes in float, so this must never
        be called. Asserts in debug and clears to silence in release. */
    void processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) override;

    /** Only stereo-output / no-input layouts are supported. */
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    /** No editor (headless engine node). */
    juce::AudioProcessorEditor* createEditor () override { return nullptr; }
    /** Reports that no editor exists. */
    bool hasEditor () const override { return false; }

    /** Source node consumes no MIDI. */
    bool acceptsMidi () const override { return false; }
    /** Source node emits no MIDI. */
    bool producesMidi () const override { return false; }
    /** Not a tail-producing effect. */
    double getTailLengthSeconds () const override { return 0.0; }

    /** Single (default) program. */
    int getNumPrograms () override { return 1; }
    /** Current program index. */
    int getCurrentProgram () override { return 0; }
    /** No-op program change. */
    void setCurrentProgram (int) override {}
    /** No program names. */
    const juce::String getProgramName (int) override { return {}; }
    /** No-op program rename. */
    void changeProgramName (int, const juce::String&) override {}

    // MESSAGE-THREAD ONLY: state is transient debug state, not persisted.
    /** No persisted state (debug node). */
    void getStateInformation (juce::MemoryBlock&) override {}
    /** No persisted state (debug node). */
    void setStateInformation (const void*, int) override {}

private:
    /** Fixed debug output level, ~-12 dBFS linear. Not user-controllable. */
    static constexpr float toneLevel = 0.251f;

    ToneControl* toneControl = nullptr; ///< Non-owning; graph owns the object.
    double currentSampleRate = 44100.0; ///< Cached in prepareToPlay.
    double phase = 0.0;                 ///< Phasor accumulator in radians [0, 2π).

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TestToneProcessor)
};
} // namespace arpbox::engine
