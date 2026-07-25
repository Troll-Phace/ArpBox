#pragma once

#include "../EngineGuiGuard.h"
#include "EngineCommand.h"
#include "ICommandSink.h"
#include "Transport.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <array>

namespace arpbox::engine
{
/** The HEAD node of the engine (ARCHITECTURE §3.3, §4 steps 1–2). A MIDI-only
    `AudioProcessor` that emits nothing and renders nothing; its entire job is to
    run FIRST in the render sequence and, in this exact order:

      1. drain the single `EngineCommandQueue` ONCE and fan every command out to
         the registered `ICommandSink`s (§4 step 1);
      2. call `Transport::beginBlock()` so the block-start musical position is
         latched (§4 step 2).

    Because it runs before every other node, both effects are visible to the whole
    block: a master-gain command lands in the SAME block it was drained in, and
    every consumer of transport state (the sequencer node, `TransportPlayHead`, the
    master's snapshot write) reads a position that is already advanced and
    consistent.

    SOLE COMMAND CONSUMER: `EngineCommandQueue` is strict SPSC and may have exactly
    ONE consumer. That consumer is this node. Phase 2 had `MasterProcessor` drain
    the queue (with the `ToneControl` atomic shim to reach the earlier-processed
    tone node); from Phase 5 the master is an `ICommandSink` fed by this node
    instead. Do NOT add a second `drain()` call anywhere.

    RENDER-ORDER GUARANTEE: see the extended note in `EngineGraph::buildGraph()`.
    In short — an ordering-only MIDI edge (`transport → MIDI-In`) forces the entire
    MIDI chain to follow this node, and this node is added to the graph BEFORE any
    other, which is what places it first among nodes the topology does not otherwise
    order.

    MIDI CONTRACT: no audio buses; accepts and produces MIDI so it can sit at the
    head of the MIDI chain. It adds NO events of its own and must never clear the
    incoming buffer — the ordering edge is a dependency, not a data path.

    RT-SAFETY: `processBlock` is allocation-free, lock-free, I/O-free and
    logging-free. The command drain invokes each sink in place (no `std::function`,
    no type erasure) and every sink is contractually RT-safe. */
class TransportProcessor : public juce::AudioProcessor
{
public:
    /** Maximum number of command sinks. Phase 5 uses two (transport, master); the
        sequencer node claims a third in Phase 5.2. Fixed-capacity by design — the
        sink table must never heap-allocate. */
    static constexpr std::size_t maxCommandSinks = 4;

    /** Constructs the node with NO audio buses (MIDI-only). */
    TransportProcessor ();

    /** ~TransportProcessor. */
    ~TransportProcessor () override = default;

    // MESSAGE-THREAD ONLY: wiring. Injects the graph-owned command queue (this node
    // is its SOLE consumer) and the graph-owned transport this node advances. Call
    // once, before the node joins the graph and before playback. Both pointers must
    // outlive this node.
    /** Sets the command queue drained by this node and the transport it advances. */
    void setSharedState (EngineCommandQueue* commands, Transport* transportToDrive) noexcept;

    // MESSAGE-THREAD ONLY: wiring. Registers the fan-out targets for drained
    // commands, in dispatch order. Null entries are skipped, so a partially-filled
    // table is valid. All sinks must outlive this node. Call once, before playback.
    /** Sets the fixed-capacity command-sink table (dispatch order = array order). */
    void setCommandSinks (std::array<ICommandSink*, maxCommandSinks> sinksToUse) noexcept;

    // ── AudioProcessor overrides ─────────────────────────────────────────────

    /** Returns the node's display name. */
    const juce::String getName () const override { return "ARPBOX Transport"; }

    // MESSAGE-THREAD ONLY: forwards the sample rate to the transport. Never on the
    // audio thread.
    /** Prepares the transport for `sampleRate` (a non-positive rate is clamped to
        44100 inside `Transport::prepare`, matching `MidiInputProcessor`). */
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;

    // MESSAGE-THREAD ONLY: release. Nothing heap-held.
    /** Releases resources (none held). */
    void releaseResources () override {}

    // RT-SAFE: audio thread. Allocation-free, lock-free, no logging/String.
    /** Drains the command queue into the sinks, then latches the block-start
        transport state. Emits no MIDI and renders no audio. */
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

    /** Sits at the head of the MIDI chain (passes its input through untouched). */
    bool acceptsMidi () const override { return true; }
    /** Produces MIDI (nothing of its own today; the ordering edge needs this true). */
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

    // MESSAGE-THREAD ONLY: transport state is project-level (§8.1), not a plugin
    // blob; persistence arrives in Phase 11.
    /** No persisted state. */
    void getStateInformation (juce::MemoryBlock&) override {}
    /** No persisted state. */
    void setStateInformation (const void*, int) override {}

private:
    // Injected, non-owning (graph-owned) shared state.
    EngineCommandQueue* commandQueue = nullptr; ///< SOLE consumer is this node.
    Transport* transport = nullptr;             ///< Advanced once per block by this node.

    // Fixed-capacity fan-out table; null entries skipped. No dynamic allocation.
    std::array<ICommandSink*, maxCommandSinks> sinks {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportProcessor)
};
} // namespace arpbox::engine
