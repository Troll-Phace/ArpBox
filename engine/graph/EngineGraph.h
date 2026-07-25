#pragma once

#include "../EngineGuiGuard.h"
#include "../sequencer/PatternChannel.h"
#include "../sequencer/PatternDocument.h"
#include "../sequencer/SequencerProcessor.h"
#include "DeviceStatus.h"
#include "EngineCommand.h"
#include "EngineEvent.h"
#include "EngineSnapshotBuffer.h"
#include "MasterProcessor.h"
#include "MidiInputControl.h"
#include "MidiInputProcessor.h"
#include "NoteEvent.h"
#include "ToneControl.h"
#include "Transport.h"
#include "TransportPlayHead.h"
#include "TransportProcessor.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <cstdint>
#include <memory>

namespace arpbox::engine
{
/** The highest sample rate ARPBOX supports, in Hz (issue #56).

    THIS IS A CORRECTNESS BOUND, NOT A TASTE PREFERENCE. The sequencer's
    step-index snap window (SequencerProcessor.h, "THE SNAP-BOUNDARY WINDOW",
    issue #37) is what makes step emission buffer-size independent, and its width
    scales linearly with the sample rate:

        window (samples) = stepIndexSnapSteps x stepPpq x (60 / bpm) x sampleRate
                         = 1e-6 x stepPpq x 60 x sampleRate / bpm

    The documented invariant is that the window stays BELOW one sample, so a
    boundary inside it lands at most one sample late and the effect can never
    compound. Evaluated at the worst supported combination — the coarsest grid
    (§2.1: 1/32..1/4 with triplet/dotted, so a dotted quarter, stepPpq = 1.5) at
    `Transport::minBpm` (20) — the window is:

        192 kHz  ->  1e-6 x 1.5 x (60/20) x 192000 = 0.864 samples   (13.6% headroom)
        384 kHz  ->  1e-6 x 1.5 x (60/20) x 384000 = 1.728 samples   (INVARIANT BROKEN)

    384 kHz is not hypothetical — DXD interfaces exist and CoreAudio will happily
    negotiate them. Nothing in the engine caps the rate on its own (the only other
    guards in the tree are non-positive floors substituting 44100 in
    Transport.cpp, EngineGraph.cpp and app/SynthSlot.cpp), so the ceiling is
    enforced at the one place the rate is chosen: `app::AudioEngine`, which
    down-negotiates a device above this rate and refuses it outright if the device
    offers nothing at or below it. See `AudioEngine::enforceSampleRateCeiling`.

    RAISING THIS VALUE IS A CONTRACT CHANGE: re-derive the table above first (and
    the one in SequencerProcessor.h), or close the window structurally per #37
    option (a). Doubling it to 384000.0 alone makes the sequencer's buffer-size
    independence claim false. */
inline constexpr double kMaxSupportedSampleRate = 192000.0;

/** Owner and assembler of the root `AudioProcessorGraph` and the three canonical
    cross-thread channels (ARCHITECTURE §3.3, §3.4).

    OWNERSHIP & LIFETIME (all message-thread constructed, outlive the callback):
      This object owns, by value, the single `EngineCommandQueue`,
      `EngineSnapshotBuffer`, `EngineEventQueue`, and the shared `ToneControl`.
      They are constructed BEFORE the graph is built and destroyed AFTER the graph
      (member declaration order below guarantees this), so the graph nodes' raw,
      non-owning pointers into them are always valid while audio can run. The app
      MUST stop the `AudioProcessorPlayer` (detach this graph) before destroying
      the `EngineGraph`.

    TOPOLOGY (built once, on the message thread, in the constructor): the fixed
    core is `Transport → MidiIn → Seq` and `TestTone → Master → audioOutput` (stereo
    pairs). The `Transport` node (a `TransportProcessor`) is the HEAD: it drains the
    command queue and advances the transport clock before anything else renders. The
    `MidiIn` node (a `MidiInputProcessor`) merges QWERTY/pad events (the owned
    `NoteEventQueue`) with hardware MIDI (the owned `MidiMessageCollector`). The `Seq`
    node (a `SequencerProcessor`, Phase 5.2) follows it, passing that live MIDI through
    untouched and ADDING the generated arp events; it is left MIDI-unconnected
    downstream until a synth is set. `setSynth()` inserts the synth on the MESSAGE
    thread with `UpdateKind::async`, wiring `Seq → synth → Master` — the synth's MIDI
    source is the SEQUENCER, not `MidiIn`, so the full chain is
    `Transport → MidiIn → Seq → Synth → Master`. The debug `TestTone → Master` edge is
    kept (tone off by default) as a fallback source. An `audioInput` and the graph's own
    `midiInput` IO node are added for completeness and stay unconnected. Topology is
    NEVER edited from the audio thread.

    TRANSPORT & PLAYHEAD (Phase 5.1): this object owns the `Transport` clock and the
    `TransportPlayHead` that wraps it, both declared BEFORE `graph` so they outlive
    the nodes holding non-owning pointers to them. `buildGraph()` installs the
    playhead on the graph ONCE (`graph.setPlayHead`), which JUCE then hands to every
    node — including plugins inserted later — on every block.

    HOSTING: `getProcessor()` returns the underlying `AudioProcessorGraph` for
    `AudioProcessorPlayer::setProcessor`. The player prepares/processes the graph;
    the graph propagates `prepareToPlay`/`processBlock` to the nodes. The
    `prepareToPlay`/`releaseResources` passthroughs here exist for HEADLESS use
    (tests drive the graph without a device — see the notes at those methods).

    COMMAND-DRAIN ARRANGEMENT (Phase 5.1): the single command queue has exactly ONE
    consumer — the `TransportProcessor` head node. It drains once per block and fans
    each command out to the registered `ICommandSink`s (the `Transport`, the
    `MasterProcessor` and, since Phase 6, the `SequencerProcessor`). See ICommandSink.h
    for the fan-out contract and ToneControl.h for why the tone commands still cross to
    the tone node through an atomic shim.

    PATTERN DATA (Phase 6): this object also owns §3.4's THIRD channel — the
    `PatternChannel` the sequencer node adopts `PatternSnapshot`s from — and, for now,
    the `PatternDocument` that produces them (see the member's note; Phase 11 moves the
    document into the Project object). `buildGraph()` attaches the channel as the
    document's publish target and primes it with one snapshot, so the node has pattern
    data on its very first block. */
class EngineGraph
{
public:
    // MESSAGE-THREAD ONLY: constructs the channels and builds the fixed topology.
    /** Builds the root graph and wires `TestTone → Master → audioOutput`. */
    EngineGraph ();

    /** ~EngineGraph. The graph (and its nodes) are torn down before the channels
        they reference, per member order; ensure audio is stopped first. */
    ~EngineGraph () = default;

    // MESSAGE-THREAD ONLY: hosting handle.
    /** Returns the root `AudioProcessorGraph` for `AudioProcessorPlayer`. */
    juce::AudioProcessor& getProcessor () noexcept { return graph; }

    // MESSAGE-THREAD ONLY (producer side of the UI→engine channel).
    /** UI-writable command queue; drained by the engine every block. */
    EngineCommandQueue& commands () noexcept { return commandQueue; }

    // MESSAGE-THREAD ONLY (consumer side of the engine→UI state channel).
    /** Snapshot triple buffer; call `.read()` from the UI at frame rate. */
    EngineSnapshotBuffer& snapshots () noexcept { return snapshotBuffer; }

    // MESSAGE-THREAD ONLY (consumer side of the engine→UI event channel).
    /** Discrete engine→UI event queue (audio thread is the only producer). */
    EngineEventQueue& events () noexcept { return eventQueue; }

    // MESSAGE-THREAD ONLY (producer side of the QWERTY/pad note channel, §3.3).
    /** UI-writable note-event queue (on-screen keyboard / QWERTY / pads); drained
        by the MIDI-In node on the audio thread. */
    NoteEventQueue& notes () noexcept { return noteQueue; }

    // MESSAGE-THREAD ONLY: the shared hardware-MIDI collector. The app's
    // `MidiInputCallback` feeds it off the MIDI thread; the MIDI-In node drains it
    // on the audio thread. Its sample rate is (re)configured by the MIDI-In node's
    // prepareToPlay — callers must NOT reset it concurrently.
    /** Shared `juce::MidiMessageCollector` for hardware MIDI input. */
    juce::MidiMessageCollector& midiInputCollector () noexcept { return midiCollector; }

    // ── Transport (Phase 5.1) ────────────────────────────────────────────────

    // MESSAGE-THREAD ONLY (observation): the transport is AUDIO-THREAD-OWNED state,
    // mutated ONLY by `beginBlock`/`applyCommand` on the audio thread. Its scalars are
    // non-atomic by design (see Transport.h), so a message-thread read can tear against
    // a concurrent `beginBlock`. Returned `const` (issue #39) so no API on the graph can
    // mutate the clock off the audio thread — every write arrives as an `EngineCommand`
    // (§3.4). This accessor exists for HEADLESS TESTS and engine-internal wiring, which
    // drive the graph themselves; the UI must read transport state from `EngineSnapshot`
    // instead (§10.1).
    /** The transport clock. Not for UI consumption — use `snapshots()`. */
    const Transport& getTransport () const noexcept { return transport; }

    // MESSAGE-THREAD ONLY (observation).
    /** The custom playhead installed on the graph; exposed for tests. */
    juce::AudioPlayHead& getPlayHead () noexcept { return playHead; }

    // ── Sequencer (Phase 5.2) ────────────────────────────────────────────────

    // MESSAGE-THREAD ONLY (observation): like `getTransport()`, this reaches
    // AUDIO-THREAD-OWNED state (the sounding-note table is non-atomic by design), so a
    // read concurrent with a block can tear. It exists for HEADLESS TESTS, which drive
    // the graph themselves and assert the §5.5 invariant that the table is empty after
    // every flush point. The UI must read sequencer state from `EngineSnapshot` (§10.1).
    /** The sequencer node. Not for UI consumption — use `snapshots()`. */
    const SequencerProcessor& getSequencer () const noexcept
    {
        jassert (sequencerProcessor != nullptr); // set unconditionally by buildGraph()
        return *sequencerProcessor;
    }

    // ── Pattern model (Phase 6) ──────────────────────────────────────────────

    // MESSAGE-THREAD ONLY (producer side of §3.4's third channel). `buildGraph()`
    // attaches the channel as this document's publish target, so every committed edit
    // rebuilds and publishes a snapshot automatically — no extra call needed.
    /** The editable pattern model; edits reach the audio thread as snapshots. */
    PatternDocument& patterns () noexcept { return patternDocument; }

    // MESSAGE-THREAD ONLY (OBSERVATION ONLY — the reference is `const`, issue #57).
    //
    // WHY CONST, AND WHAT IT BUYS: `PatternChannel` is a strict SPSC channel whose
    // three mutating methods each belong to exactly ONE thread — `adopt()` / `retire()`
    // to the AUDIO thread (they are the sequencer node's half of the pair) and
    // `publish()` to the MESSAGE thread, reached only through `patterns()`, whose
    // document owns the publish target. A message-thread caller reaching those through
    // the graph would put two producers on the retirement queue and two consumers on
    // the publish slot. All three are non-const, so handing out a `const&` makes that
    // misuse a COMPILE ERROR rather than a documented prohibition — the same barrier
    // #39 put on `getTransport()`, for the same reason.
    //
    // What remains reachable is exactly the observation surface: `peekPending()`,
    // `getNumPendingRetirements()`, `getDroppedRetirementCount()`. The one legitimate
    // message-thread mutation — reclamation — has its own narrow entry point below
    // (`reclaimRetiredPatterns()`), and publishing goes through `patterns()`.
    //
    // Headless tests that need to drive publish/adopt/retire by hand own their OWN
    // `PatternChannel` (see tests/support/SequencerRenderRig.h); they do not, and must
    // not, borrow the graph's.
    /** Read-only view of the pattern publish/adopt/retire channel (observation only). */
    const PatternChannel& patternSnapshots () const noexcept { return patternChannel; }

    // MESSAGE-THREAD ONLY: deletes every `PatternSnapshot` the audio thread has
    // retired. THE APP MUST CALL THIS ON ITS UI TICK (`DebugPanel::refreshFromEngine`
    // does, from the vblank — never a `juce::Timer`, prohibited by code-style.md).
    // `PatternDocument::publishTo` also reclaims, but only as a side effect of
    // EDITING: a session that plays for an hour without a single document edit would
    // otherwise never drain the queue, and the audio thread retires a snapshot on
    // every adoption. A steady tick is the design; the publish-path reclaim is the
    // safety net.
    /** Frees all retired pattern snapshots (message-thread reclamation, §3.4). */
    void reclaimRetiredPatterns () noexcept;

    // MESSAGE-THREAD ONLY: observation, for the debug readout.
    /** Retired snapshots awaiting `reclaimRetiredPatterns()` (advisory). */
    int getNumPendingRetirements () const noexcept;

    // MESSAGE-THREAD ONLY: observation, for the debug readout.
    /** Retirements dropped because the retirement queue was full. EACH ONE IS A
        LEAKED SNAPSHOT (the queue deliberately never frees on the audio thread), so a
        non-zero value is a defect, not a statistic — it means the message thread is
        not reclaiming often enough. Must stay 0. */
    std::uint64_t getDroppedRetirementCount () const noexcept;

    // ── Synth slot: type-agnostic graph topology API (§3.3, §6.3) ────────────

    // MESSAGE-THREAD ONLY: inserts (or swaps in) the synth instance, wiring
    // `MidiIn → synth → Master` with `UpdateKind::async`. The instance is the BASE
    // `juce::AudioProcessor` type — the engine never names the hosting wrapper. The
    // instance MUST already be prepared by the caller (the graph re-prepares it for
    // its own SR/block during the async rebuild; do not double-prepare). If a synth
    // is already present it is removed first (JUCE reclaims the retired node on the
    // message thread). Passing `nullptr` is equivalent to `removeSynth()`.
    /** Sets/swaps the hosted synth in the single instrument slot. */
    void setSynth (std::unique_ptr<juce::AudioProcessor> synth);

    // MESSAGE-THREAD ONLY: removes the current synth (async). Safe when none is set.
    /** Removes the hosted synth from the instrument slot. */
    void removeSynth ();

    // MESSAGE-THREAD ONLY: requests a one-shot all-notes-off flush (CC123 on all
    // channels) on the MIDI-In path and zeroes its voice count. A flush primitive
    // for the plugin-slot coordinator around synth swaps/removals (§5.5).
    /** Requests an all-notes-off flush on the MIDI-in path. */
    void allNotesOff () noexcept;

    // MESSAGE-THREAD ONLY: sets the MIDI-input channel filter (bit i ⇒ channel i+1
    // passes). Default all-pass. Lock-free; the audio thread reads it next block.
    /** Sets the 16-bit MIDI-input channel mask. */
    void setMidiChannelMask (std::uint16_t mask) noexcept;

    // MESSAGE-THREAD ONLY: the graph's current audio config, for preparing a synth
    // instance to hand to `setSynth()`. Valid once the graph/player has been
    // prepared (0 before then).
    /** Current graph sample rate (Hz). */
    double getSampleRate () const noexcept;
    /** Current graph block size (samples). */
    int getBlockSize () const noexcept;

    // MESSAGE-THREAD ONLY: headless prepare. The app path (AudioProcessorPlayer)
    // prepares the graph itself; call this only when driving the graph directly.
    /** Configures stereo I/O and prepares the graph and all nodes. */
    void prepareToPlay (double sampleRate, int blockSize);

    // MESSAGE-THREAD ONLY: headless release counterpart to prepareToPlay.
    /** Releases the graph and node resources. */
    void releaseResources ();

    // MESSAGE-THREAD ONLY: device/app layer reports audio-device health here; the
    // value is surfaced through `EngineSnapshot.deviceStatus` on the next block.
    /** Forwards a device-status level to the master node. */
    void setDeviceStatus (DeviceStatus status) noexcept;

private:
    // MESSAGE-THREAD ONLY: one-time topology assembly (called from the ctor).
    void buildGraph ();

    // Cross-thread channels — declared FIRST so they outlive `graph` (whose nodes
    // hold non-owning pointers into them). Destruction is reverse-declaration:
    // graph (and nodes) first, then these.
    EngineCommandQueue commandQueue;
    EngineSnapshotBuffer snapshotBuffer;
    EngineEventQueue eventQueue;
    ToneControl toneControl;
    NoteEventQueue noteQueue;                 ///< QWERTY/pad channel; MIDI-In node consumes.
    juce::MidiMessageCollector midiCollector; ///< Hardware MIDI; fed off the MIDI thread, drained by the MIDI-In node.
    MidiInputControl midiControl;             ///< MIDI-In channel mask / voice count / flush.

    // §3.4's THIRD cross-thread mechanism, and its message-thread producer. Same
    // lifetime rule as everything above: declared before `graph`, so the sequencer
    // node (which holds a non-owning pointer to the channel and, between adoptions, a
    // pointer to a snapshot the channel's destructor reclaims) dies FIRST.
    //
    // ORDER WITHIN THE PAIR MATTERS TOO: the document holds a publish-target pointer
    // to the channel, so it must be destroyed first — hence channel, then document.
    PatternChannel patternChannel; ///< Publish/adopt/retire channel for PatternSnapshot.

    /** The authoritative editable pattern model. TEMPORARY HOME: Phase 11's Project
        object owns the document (alongside the rack, master and MIDI config, §8.1)
        and the graph merely borrows it. It lives here now because the sequencer node
        needs a published snapshot from its very first block and nothing above the
        engine exists yet to provide one. */
    PatternDocument patternDocument;

    // Transport clock + the playhead wrapping it. Declared HERE (before `graph`) for
    // the same lifetime reason as the channels above: the transport node, the master
    // node, the SEQUENCER node and the graph's own playhead pointer all reference them
    // non-owningly, and must never outlive them.
    Transport transport;                      ///< Audio-thread-owned musical clock.
    TransportPlayHead playHead { transport }; ///< Installed on the graph in buildGraph().

    // The root graph owns the processor nodes.
    juce::AudioProcessorGraph graph;

    // Non-owning handle into a graph-owned node (for setDeviceStatus forwarding).
    MasterProcessor* masterProcessor = nullptr;
    MidiInputProcessor* midiInputProcessor = nullptr; ///< Non-owning; graph-owned MIDI-In node.
    TransportProcessor* transportProcessor = nullptr; ///< Non-owning; graph-owned head node.
    SequencerProcessor* sequencerProcessor = nullptr; ///< Non-owning; graph-owned arp engine node.

    // Message-thread-only node identities retained for topology edits.
    juce::AudioProcessorGraph::NodeID transportNodeId; ///< Transport head node (ordering root).
    juce::AudioProcessorGraph::NodeID midiInputNodeId; ///< MIDI-In node (upstream of the sequencer).
    juce::AudioProcessorGraph::NodeID sequencerNodeId; ///< Sequencer node (source of synth MIDI).
    juce::AudioProcessorGraph::NodeID masterNodeId;    ///< Master node (synth audio sink).
    juce::AudioProcessorGraph::NodeID synthNodeId;     ///< Current synth (invalid uid 0 when none).

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EngineGraph)
};
} // namespace arpbox::engine
