#pragma once

#include "../EngineGuiGuard.h"
#include "DeviceStatus.h"
#include "EngineCommand.h"
#include "EngineEvent.h"
#include "EngineSnapshotBuffer.h"
#include "MasterProcessor.h"
#include "MidiInputControl.h"
#include "MidiInputProcessor.h"
#include "NoteEvent.h"
#include "ToneControl.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <cstdint>
#include <memory>

namespace arpbox::engine
{
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
    core is `MidiIn` and `TestTone → Master → audioOutput` (stereo pairs). The
    `MidiIn` node (a `MidiInputProcessor`) merges QWERTY/pad events (the owned
    `NoteEventQueue`) with hardware MIDI (the owned `MidiMessageCollector`) and is
    left MIDI-unconnected until a synth is set. `setSynth()` inserts the synth on
    the MESSAGE thread with `UpdateKind::async`, wiring `MidiIn → synth → Master`;
    the debug `TestTone → Master` edge is kept (tone off by default) as a fallback
    source. An `audioInput` and the graph's own `midiInput` IO node are added for
    completeness and stay unconnected. Topology is NEVER edited from the audio
    thread.

    HOSTING: `getProcessor()` returns the underlying `AudioProcessorGraph` for
    `AudioProcessorPlayer::setProcessor`. The player prepares/processes the graph;
    the graph propagates `prepareToPlay`/`processBlock` to the nodes. The
    `prepareToPlay`/`releaseResources` passthroughs here exist for HEADLESS use
    (tests drive the graph without a device — see the notes at those methods).

    COMMAND-DRAIN ARRANGEMENT (Phase 2, temporary): the single command queue is
    drained by `MasterProcessor` at the head of its block (it is the sole SPSC
    consumer). See MasterProcessor.h / ToneControl.h for how tone commands cross
    from the master to the earlier-processed tone node. Phase 5 replaces this with
    a dedicated transport head node. */
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
    NoteEventQueue noteQueue;             ///< QWERTY/pad channel; MIDI-In node consumes.
    juce::MidiMessageCollector midiCollector; ///< Hardware MIDI; fed off the MIDI thread, drained by the MIDI-In node.
    MidiInputControl midiControl;         ///< MIDI-In channel mask / voice count / flush.

    // The root graph owns the processor nodes.
    juce::AudioProcessorGraph graph;

    // Non-owning handle into a graph-owned node (for setDeviceStatus forwarding).
    MasterProcessor* masterProcessor = nullptr;
    MidiInputProcessor* midiInputProcessor = nullptr; ///< Non-owning; graph-owned MIDI-In node.

    // Message-thread-only node identities retained for topology edits.
    juce::AudioProcessorGraph::NodeID midiInputNodeId; ///< MIDI-In node (source of synth MIDI).
    juce::AudioProcessorGraph::NodeID masterNodeId;    ///< Master node (synth audio sink).
    juce::AudioProcessorGraph::NodeID synthNodeId;     ///< Current synth (invalid uid 0 when none).

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EngineGraph)
};
} // namespace arpbox::engine
