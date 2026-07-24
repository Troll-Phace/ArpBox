#pragma once

#include "../EngineGuiGuard.h"
#include "DeviceStatus.h"
#include "EngineCommand.h"
#include "EngineEvent.h"
#include "EngineSnapshotBuffer.h"
#include "MasterProcessor.h"
#include "ToneControl.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

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

    TOPOLOGY (built once, on the message thread, in the constructor — NEVER edited
    from the audio thread): `TestTone → Master → audioOutput`, stereo pairs. An
    `audioInput` and a `midiInput` IO node are added for topology completeness and
    left unconnected until Phase 4 wires the synth/MIDI path.

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

    // The root graph owns the processor nodes.
    juce::AudioProcessorGraph graph;

    // Non-owning handle into a graph-owned node (for setDeviceStatus forwarding).
    MasterProcessor* masterProcessor = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EngineGraph)
};
} // namespace arpbox::engine
