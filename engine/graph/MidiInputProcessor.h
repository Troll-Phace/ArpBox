#pragma once

#include "../EngineGuiGuard.h"
#include "MidiInputControl.h"
#include "NoteEvent.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <cstdint>

namespace arpbox::engine
{
/** MIDI-only merge/source node at the head of the note path (ARCHITECTURE §3.3,
    the "[MIDI In Node] (merge + channel filter)"). It processes EARLIER than the
    synth in the graph and is wired `MIDI-In → synth` via
    `AudioProcessorGraph::midiChannelIndex`.

    It has NO audio buses — it neither consumes nor produces audio; it only emits a
    `MidiBuffer` for downstream nodes. Each block it merges TWO input sources into
    the outgoing MIDI, applies channel filtering, and tracks a live voice count:

      1. QWERTY/pad events from the injected `NoteEventQueue` (message-thread
         producer). Added at sample offset 0 — human/UI jitter dwarfs sub-block
         precision — in producer ORDER (drain is FIFO), so a note-on never inverts
         with its following note-off.
      2. Hardware MIDI from the injected `juce::MidiMessageCollector`, pulled with
         `removeNextBlockOfMessages` (which timestamps within the block). The same
         collector instance is fed by the app's `MidiInputCallback` off the MIDI
         thread; `MidiBuffer` keeps merged events sorted by sample position.

    CHANNEL FILTERING: a 16-bit mask (`MidiInputControl::channelMask`, default
    all-pass) gates channel-voice messages; system messages (no channel) always
    pass.

    RT-SAFETY: `processBlock` is allocation-free and lock-free with ONE documented
    exception — `MidiMessageCollector::removeNextBlockOfMessages` briefly takes the
    collector's internal `CriticalSection` (see the call site). That bounded (~µs)
    vendored lock is the sole accepted audio-thread lock on this path. All MIDI is
    assembled from RAW bytes (no `juce::MidiMessage` construction) so nothing on the
    hot path can heap-allocate; the hardware scratch buffer is pre-sized in
    `prepareToPlay`, and the graph-owned OUTGOING MidiBuffer has its capacity ensured
    every block (a no-op once satisfied) so the 16-event CC123 flush and typical
    traffic never grow it in steady state — and it re-warms after a graph
    render-sequence rebuild hands the node a fresh pool buffer. */
class MidiInputProcessor : public juce::AudioProcessor
{
public:
    /** Constructs the node with NO audio buses (MIDI-only). */
    MidiInputProcessor ();

    /** ~MidiInputProcessor. */
    ~MidiInputProcessor () override = default;

    // MESSAGE-THREAD ONLY: wiring. Injects the graph-owned QWERTY/pad note queue,
    // the shared hardware-MIDI collector, and the shared control/status block. Call
    // once, before the node joins the graph and before playback. All pointers must
    // outlive this node.
    /** Sets the non-owning pointers the audio thread reads each block: the
        `NoteEventQueue` (QWERTY/pads), the `MidiMessageCollector` (hardware MIDI),
        and the `MidiInputControl` (channel mask / voice count / flush). */
    void setSharedState (NoteEventQueue* notes,
                         juce::MidiMessageCollector* collector,
                         MidiInputControl* control) noexcept;

    // ── AudioProcessor overrides ─────────────────────────────────────────────

    /** Returns the node's display name. */
    const juce::String getName () const override { return "ARPBOX MIDI In"; }

    // MESSAGE-THREAD ONLY: caches the sample rate, resets the collector to it, and
    // pre-sizes the hardware scratch buffer. Never on the audio thread.
    /** Resets the shared collector to `sampleRate` (this node owns the collector's
        SR configuration) and pre-allocates the MIDI scratch buffer so
        `processBlock` never grows it. */
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;

    // MESSAGE-THREAD ONLY: release. Nothing heap-held beyond the scratch buffer.
    /** Releases resources (none beyond the pre-sized scratch buffer). */
    void releaseResources () override {}

    // RT-SAFE: audio thread. Allocation-free; the ONLY lock is the collector's
    // brief internal CriticalSection (documented at the call site).
    /** Merges QWERTY/pad + hardware MIDI into `midi`, applies channel filtering,
        and publishes the live MIDI-in voice count. Renders no audio. */
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    // RT-SAFE: audio thread. The graph runs float; this double path is unused.
    /** Double-precision path — must never be called (graph is float). */
    void processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) override;

    /** MIDI-only: only the no-audio-bus layout is supported. */
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    /** No editor (headless engine node). */
    juce::AudioProcessorEditor* createEditor () override { return nullptr; }
    /** Reports that no editor exists. */
    bool hasEditor () const override { return false; }

    /** Consumes MIDI (it also self-generates from the queue/collector). */
    bool acceptsMidi () const override { return true; }
    /** Emits MIDI to the downstream synth. */
    bool producesMidi () const override { return true; }
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

    // MESSAGE-THREAD ONLY: transient routing node; nothing persisted.
    /** No persisted state. */
    void getStateInformation (juce::MemoryBlock&) override {}
    /** No persisted state. */
    void setStateInformation (const void*, int) override {}

private:
    // RT-SAFE: true if `channel` (1..16) passes `mask`; non-channel/out-of-range
    // always passes.
    static bool channelPasses (std::uint16_t mask, int channel) noexcept;

    // Injected, non-owning (graph-owned) shared state.
    NoteEventQueue* noteQueue = nullptr;        ///< QWERTY/pad producer channel (SPSC consumer here).
    juce::MidiMessageCollector* collector = nullptr; ///< Hardware MIDI, fed off the MIDI thread.
    MidiInputControl* control = nullptr;        ///< Channel mask / voice count / flush.

    // Pre-sized in prepareToPlay so removeNextBlockOfMessages + iteration never
    // allocate on the audio thread.
    juce::MidiBuffer hardwareScratch;

    // Audio-thread-private running count of live MIDI-in notes; published into
    // MidiInputControl::voiceCount each block. Reset by an all-notes-off flush.
    int liveVoices = 0;

    double currentSampleRate = 44100.0; ///< Cached in prepareToPlay (for the collector).

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiInputProcessor)
};
} // namespace arpbox::engine
