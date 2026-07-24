#pragma once

#include "../EngineGuiGuard.h"
#include "DeviceStatus.h"
#include "EngineCommand.h"
#include "EngineEvent.h"
#include "EngineSnapshotBuffer.h"
#include "ToneControl.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>
#include <juce_dsp/juce_dsp.h>

#include <atomic>
#include <cstdint>

namespace arpbox::engine
{
/** Master-section graph node: the last processor before the audio-output node
    (ARCHITECTURE §7). Signal chain, in order, per `processBlock`:

        drain EngineCommandQueue → output gain → safety limiter (default ON)
        → NaN/Inf scrub (graph boundary) → meter tap → publish EngineSnapshot

    COMMAND DRAIN (Phase-2 arrangement — TEMPORARY): the master is the SINGLE
    consumer of the shared `EngineCommandQueue`. It drains at the top of its block
    and dispatches by type: master-gain / limiter-enable are applied to itself
    here; the two test-tone commands are written into the shared `ToneControl`
    atomics that the (earlier-processed) tone source reads next block. This
    head-of-engine drain migrates to the transport/arp head node in Phase 5; the
    master then owns only gain/limiter/metering.

    NaN/Inf SCRUB: this node is THE scrub point for the whole graph boundary — a
    non-finite sample reaching CoreAudio can wedge the driver, so every sample is
    forced finite here, once, after gain+limiter and before metering/output.

    All sample-rate-dependent DSP state (`dsp::Gain`, `dsp::Limiter`) is prepared
    ONLY in `prepareToPlay`; `processBlock` never allocates, locks, or re-prepares. */
class MasterProcessor : public juce::AudioProcessor
{
public:
    /** Constructs the node with stereo input and stereo output buses. */
    MasterProcessor ();

    /** ~MasterProcessor. */
    ~MasterProcessor () override = default;

    // MESSAGE-THREAD ONLY: wiring. Injects the graph-owned cross-thread channels
    // and shared tone control. Call once, before the node joins the graph and
    // before playback. All pointers must outlive this node.
    /** Wires the shared command queue (drained by this node), the snapshot buffer
        (written by this node each block), and the tone control (published to by
        this node when it drains tone commands). */
    void setSharedState (EngineCommandQueue* commands,
                         EngineSnapshotBuffer* snapshots,
                         ToneControl* tone) noexcept;

    // MESSAGE-THREAD ONLY: wiring. Injects the engine→UI event queue this node
    // produces onto (the AUDIO thread is that queue's SOLE producer — see
    // EngineEvent.h). Call once, before the node joins the graph.
    /** Wires the discrete event queue used to emit `latencyChanged` from the audio
        thread. Non-owning; must outlive this node. */
    void setEventQueue (EngineEventQueue* events) noexcept { eventQueue = events; }

    // MESSAGE-THREAD ONLY: wiring. Injects the processor whose `getLatencySamples()`
    // is polled each block to detect graph-latency changes (normally the root graph
    // itself, so total serial-chain latency is reported). Non-owning; must outlive
    // this node. Reading an int on the audio thread is lock-free and benign.
    /** Sets the latency source polled on the audio thread for `latencyChanged`. */
    void setLatencySource (const juce::AudioProcessor* source) noexcept { latencySource = source; }

    // MESSAGE-THREAD ONLY: wiring. Injects the atomic the MIDI-In node publishes its
    // live voice count into; copied into every `EngineSnapshot`. Non-owning.
    /** Sets the voice-count source surfaced through `EngineSnapshot.voiceCount`. */
    void setVoiceCountSource (const std::atomic<std::uint16_t>* source) noexcept { voiceCountSource = source; }

    // MESSAGE-THREAD ONLY: called by the device/app layer (Phase 2.1) to report
    // audio-device health. The value is copied into every published snapshot.
    /** Sets the device-status level surfaced through `EngineSnapshot.deviceStatus`.
        Lock-free; readable by the audio thread on its next block. */
    void setDeviceStatus (DeviceStatus status) noexcept
    {
        deviceStatus.store (static_cast<std::uint8_t> (status), std::memory_order_relaxed);
    }

    // ── AudioProcessor overrides ─────────────────────────────────────────────

    /** Returns the node's display name. */
    const juce::String getName () const override { return "ARPBOX Master"; }

    // MESSAGE-THREAD ONLY: prepares all SR-dependent DSP. Never on audio thread.
    /** Prepares the gain and limiter for the given rate/block size and primes the
        gain smoothing ramp. This is the only place DSP state is (re)allocated. */
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;

    // MESSAGE-THREAD ONLY: release. DSP objects hold small inline state only.
    /** Releases resources (none heap-held); safe when unprepared. */
    void releaseResources () override {}

    // RT-SAFE: audio thread. Allocation-free, lock-free, no logging/String.
    /** Runs the full master chain and publishes the per-block meter snapshot. */
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    // RT-SAFE: audio thread. The graph runs float; this double path is unused.
    /** Double-precision path — must never be called (graph is float). */
    void processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) override;

    /** Only stereo→stereo layouts are supported. */
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    /** No editor (headless engine node). */
    juce::AudioProcessorEditor* createEditor () override { return nullptr; }
    /** Reports that no editor exists. */
    bool hasEditor () const override { return false; }

    /** Master consumes no MIDI. */
    bool acceptsMidi () const override { return false; }
    /** Master emits no MIDI. */
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

    // MESSAGE-THREAD ONLY: master DSP params are transient in Phase 2; not saved
    // in the plugin blob (project-level persistence is Phase 11).
    /** No persisted state yet. */
    void getStateInformation (juce::MemoryBlock&) override {}
    /** No persisted state yet. */
    void setStateInformation (const void*, int) override {}

private:
    // RT-SAFE: applies one drained command. Called from processBlock's drain loop.
    void applyCommand (const EngineCommand& command) noexcept;

    // Injected, non-owning cross-thread channels (graph-owned).
    EngineCommandQueue* commandQueue = nullptr;   ///< Drained by this node (SPSC consumer).
    EngineSnapshotBuffer* snapshotBuffer = nullptr;///< Written by this node each block.
    ToneControl* toneControl = nullptr;            ///< Published to on tone commands.
    EngineEventQueue* eventQueue = nullptr;        ///< Produced onto by this node (latencyChanged).

    // Latency reporting: poll the source's latency on the audio thread and emit a
    // latencyChanged event only when it changes. `-1` forces one report on the
    // first block so the UI always gets an initial reading.
    const juce::AudioProcessor* latencySource = nullptr; ///< Usually the root graph.
    std::int32_t lastReportedLatency = -1;               ///< Audio-thread private; last emitted value.

    // Voice-count source (MIDI-In node publishes; this node snapshots it).
    const std::atomic<std::uint16_t>* voiceCountSource = nullptr;

    // Master DSP — prepared in prepareToPlay, read/processed on the audio thread.
    juce::dsp::Gain<float> outputGain;   ///< Smoothed output gain (dB target).
    juce::dsp::Limiter<float> limiter;   ///< Brickwall safety limiter.
    bool limiterEnabled = true;          ///< Audio-thread only; default ON (§7).

    // Cross-thread status level (message thread writes, audio thread reads).
    std::atomic<std::uint8_t> deviceStatus { static_cast<std::uint8_t> (deviceStatusOk) };

    // Freshness counter carried into the snapshot; audio-thread private.
    std::uint64_t blockCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterProcessor)
};
} // namespace arpbox::engine
