#include "EngineGraph.h"

#include "TestToneProcessor.h"

namespace arpbox::engine
{
namespace
{
    constexpr int numStereoChannels = 2;

    // Placeholder audio config applied in buildGraph purely so the graph reports a
    // non-zero OUTPUT channel count BEFORE the IO nodes are added (see buildGraph).
    // The real values arrive later from the app's AudioProcessorPlayer (and the
    // headless prepareToPlay). The output channel count stays 2 across those
    // re-configurations, so the connections wired in buildGraph persist.
    constexpr double placeholderSampleRate = 44100.0;
    constexpr int placeholderBlockSize = 512;
} // namespace

// MESSAGE-THREAD ONLY:
EngineGraph::EngineGraph ()
{
    buildGraph ();
}

// MESSAGE-THREAD ONLY:
void EngineGraph::buildGraph ()
{
    using IOProcessor = juce::AudioProcessorGraph::AudioGraphIOProcessor;

    graph.clear ();

    // CRITICAL: configure the graph's channel count BEFORE adding any node.
    // `AudioGraphIOProcessor::setParentGraph` derives the audioOutputNode's INPUT
    // channel count from `graph.getTotalNumOutputChannels()` AT addNode time. With
    // the graph still at its default 0/0, audioOutputNode would be created with ZERO
    // input channels, and every `Master → audioOut` connection below would be
    // silently rejected by `isConnectionLegal` (destChannel < 0 inputs is false).
    // The Master would still meter its own signal (it stays in the render sequence),
    // but its output would reach nothing → the device receives pure SILENCE. Setting
    // 2 output channels here makes those connections legal. SR/block are placeholders
    // (the player / prepareToPlay re-set them with real values; output stays 2, so
    // the connections persist).
    graph.setPlayConfigDetails (numStereoChannels, numStereoChannels, placeholderSampleRate, placeholderBlockSize);

    // ── NODE INSERTION ORDER IS LOAD-BEARING (Phase 5.1) ─────────────────────
    // The transport head node must render FIRST: it drains the command queue and
    // latches the block-start musical position for every other node. Two mechanisms
    // put it there, and BOTH are required:
    //
    //  (a) The ordering-only edge `Transport → MidiIn` added below forces the entire
    //      MIDI chain (MidiIn → Seq → synth → Master) to follow it.
    //
    //  (b) Insertion order settles the nodes the topology does NOT order. JUCE's
    //      `createOrderedNodeList` walks nodes in nodeID (= insertion) order and
    //      inserts each one only as early as a dependency demands, so unrelated
    //      nodes keep their insertion order. With NO synth loaded there is no path
    //      from the transport to the master, so ONLY insertion order separates them
    //      — and because the master is a dependency of `audioOutput`, adding
    //      `audioOutput` FIRST would drag the master to the front of the sequence,
    //      ahead of the transport. Hence: PROCESSOR nodes first (transport first of
    //      all), IO nodes last. Verified against JUCE 8.0.15; the observable
    //      consequence is asserted by the "command applies within the same block"
    //      case in tests/transport_clock.cpp, which fails loudly if JUCE's ordering
    //      heuristic ever changes.
    //
    // DOCUMENTED FALLBACK if this ever proves insufficient: `AudioProcessorGraph` is
    // not `final` and its `processBlock` is virtual — subclass it and advance the
    // transport before delegating to the base render sequence. That makes ordering
    // unconditional at the cost of the head node no longer being a plain graph node.

    // Transport head node: sole consumer of the command queue, advances the clock.
    // Inject the shared state before insertion. Sinks are wired after the master
    // exists (see below).
    auto transportNode = std::make_unique<TransportProcessor> ();
    transportNode->setSharedState (&commandQueue, &transport);
    transportProcessor = transportNode.get ();
    const auto transportGraphNode = graph.addNode (std::move (transportNode));
    transportNodeId = transportGraphNode->nodeID;

    // MIDI-In node: merges QWERTY/pad (noteQueue) + hardware (midiCollector),
    // applies channel filtering, publishes the MIDI-in voice count. Inject the
    // shared state before insertion. Its downstream consumer is the sequencer node.
    auto midiIn = std::make_unique<MidiInputProcessor> ();
    midiIn->setSharedState (&noteQueue, &midiCollector, &midiControl);
    midiInputProcessor = midiIn.get ();
    const auto midiInNode = graph.addNode (std::move (midiIn));
    midiInputNodeId = midiInNode->nodeID;

    // ARP ENGINE node (§3.3, Phase 5.2/6): reads the transport, adopts pattern
    // snapshots from the pattern channel, passes the live MIDI-in stream through
    // untouched and ADDS the generated arp events. Inject both before insertion — the
    // transport and the channel are declared before `graph` (see the header's
    // member-order contract), so they outlive this node. Left MIDI-unconnected
    // downstream until a synth is set; `setSynth()` wires `Seq → synth`.
    auto sequencer = std::make_unique<SequencerProcessor> ();
    sequencer->setSharedState (&transport, &patternChannel);
    sequencerProcessor = sequencer.get ();
    const auto sequencerNode = graph.addNode (std::move (sequencer));
    sequencerNodeId = sequencerNode->nodeID;

    // Test-tone source (debug fallback): inject the shared control block before insertion.
    auto tone = std::make_unique<TestToneProcessor> ();
    tone->setToneControl (&toneControl);
    const auto toneNode = graph.addNode (std::move (tone));

    // Master: inject the snapshot buffer + tone control + event queue
    // (latencyChanged) + latency source (the graph itself) + voice-count source +
    // transport (for the snapshot's transport fields) before insertion; keep a
    // non-owning handle for setDeviceStatus forwarding. The master no longer drains
    // the command queue — it is an ICommandSink fed by the transport node.
    auto master = std::make_unique<MasterProcessor> ();
    master->setSharedState (&snapshotBuffer, &toneControl);
    master->setEventQueue (&eventQueue);
    master->setLatencySource (&graph);
    master->setVoiceCountSource (&midiControl.voiceCount);
    master->setTransportSource (&transport);
    masterProcessor = master.get ();
    const auto masterNode = graph.addNode (std::move (master));
    masterNodeId = masterNode->nodeID;

    // Command fan-out (ICommandSink.h): the transport consumes the four transport
    // commands, the master consumes gain/limiter/tone, and the SEQUENCER consumes
    // Phase 6's `queuePatternSwitch`. Every sink sees every command and ignores what
    // it does not own. ONE spare slot remains.
    //
    // DISPATCH ORDER IS THE ARRAY ORDER, and the transport must stay first: it is the
    // only sink whose state the others read. Note that all of this runs BEFORE
    // `Transport::beginBlock()`, which is why the sequencer's `applyCommand` only
    // RECORDS a switch request and computes its boundary later (see
    // SequencerProcessor.h).
    //
    // Everything else Phase 6 added — euclid, direction, lane edits, grid, pool — is a
    // DOCUMENT edit and reaches the audio thread as a `PatternSnapshot`, not as a
    // command. Do not add command types for them.
    transportProcessor->setCommandSinks ({ &transport, masterProcessor, sequencerProcessor, nullptr });

    // IO nodes, added LAST (see the insertion-order note above). audioOutput is the
    // sink; audioInput and the graph's own midiInput IO node are added for topology
    // completeness and stay unconnected (hardware MIDI enters via the shared
    // collector on the MidiInputProcessor, not here).
    const auto audioOutNode = graph.addNode (std::make_unique<IOProcessor> (IOProcessor::audioOutputNode));
    graph.addNode (std::make_unique<IOProcessor> (IOProcessor::audioInputNode));
    graph.addNode (std::make_unique<IOProcessor> (IOProcessor::midiInputNode));

    // ORDERING-ONLY MIDI EDGE: Transport → MidiIn. It carries NO events — the
    // transport node emits nothing. Its ONLY purpose is to make the whole MIDI chain
    // a DEPENDANT of the transport node, so the render sequence is forced to run the
    // transport (command drain + clock advance) before MidiIn, before the sequencer
    // node, before the hosted synth, and therefore before anything that reads transport
    // state or queries the playhead. Do not remove it and do not treat it as a data path.
    {
        [[maybe_unused]] const bool orderingWired =
            graph.addConnection ({ { transportNodeId, juce::AudioProcessorGraph::midiChannelIndex },
                                   { midiInputNodeId, juce::AudioProcessorGraph::midiChannelIndex } });
        jassert (orderingWired);
    }

    // DATA MIDI EDGE: MidiIn → Seq. Unlike the edge above this one carries real events:
    // the merged QWERTY/hardware stream, which the sequencer passes through (so the
    // keyboard still plays the synth) and which becomes Phase 8's THRU note pool. It is
    // also what places the sequencer after MidiIn — and, transitively, after the
    // transport — in the render sequence. Assert it: a silently rejected connection here
    // is how the arp would emit into a buffer nobody reads.
    {
        [[maybe_unused]] const bool sequencerWired =
            graph.addConnection ({ { midiInputNodeId, juce::AudioProcessorGraph::midiChannelIndex },
                                   { sequencerNodeId, juce::AudioProcessorGraph::midiChannelIndex } });
        jassert (sequencerWired);
    }

    // Wire TestTone → Master → audioOutput as stereo pairs. The tone edge is kept
    // as a debug fallback source (tone off by default); a set synth sums into the
    // same Master input. These connections are load-bearing for ALL audible output —
    // assert they take (a silent rejection here is the "device gets silence" bug),
    // never ignore the return value.
    for (int ch = 0; ch < numStereoChannels; ++ch)
    {
        [[maybe_unused]] const bool toneWired =
            graph.addConnection ({ { toneNode->nodeID, ch }, { masterNodeId, ch } });
        jassert (toneWired);
    }

    for (int ch = 0; ch < numStereoChannels; ++ch)
    {
        [[maybe_unused]] const bool outputWired =
            graph.addConnection ({ { masterNodeId, ch }, { audioOutNode->nodeID, ch } });
        jassert (outputWired);
    }

    // Install the custom playhead ONCE. AudioProcessorGraph::processBlock hands
    // `getPlayHead()` to its render sequence, and every node's render op calls
    // `processor.setPlayHead(...)` with it on EVERY block — so this single call
    // reaches every current and future node, including plugins inserted later by an
    // async graph edit. It also means `AudioProcessorPlayer` never installs its own
    // sample-count playhead (it only does so when `getPlayHead() == nullptr`), so
    // ours is the only playhead hosted plugins ever see (§3.3).
    graph.setPlayHead (&playHead);

    // PATTERN DATA (§3.4 mechanism 3): attach the channel as the document's publish
    // target, so every committed edit rebuilds and republishes automatically, then
    // PRIME it with one snapshot. `setPublishTarget` deliberately does not publish, and
    // without this first `publishTo` the sequencer node would have nothing adopted on
    // its first block and would render silence until the user's first edit.
    patternDocument.setPublishTarget (&patternChannel);
    patternDocument.publishTo (patternChannel);
}

// MESSAGE-THREAD ONLY:
void EngineGraph::reclaimRetiredPatterns () noexcept
{
    patternChannel.reclaim ();
}

// MESSAGE-THREAD ONLY:
int EngineGraph::getNumPendingRetirements () const noexcept
{
    return patternChannel.getNumPendingRetirements ();
}

// MESSAGE-THREAD ONLY:
std::uint64_t EngineGraph::getDroppedRetirementCount () const noexcept
{
    return patternChannel.getDroppedRetirementCount ();
}

// MESSAGE-THREAD ONLY:
void EngineGraph::setSynth (std::unique_ptr<juce::AudioProcessor> synth)
{
    using UpdateKind = juce::AudioProcessorGraph::UpdateKind;

    if (synth == nullptr)
    {
        removeSynth ();
        return;
    }

    // Swap: remove any current synth first. JUCE reclaims the retired node on the
    // message thread via its RenderSequenceExchange — we must NOT free it ourselves
    // and must never touch topology from the audio thread.
    if (synthNodeId.uid != 0)
    {
        graph.removeNode (synthNodeId, UpdateKind::async);
        synthNodeId = {};
    }

    // The instance is already prepared by the caller; the graph re-prepares it for
    // its own SR/block during the async rebuild. addNode takes ownership.
    const auto synthNode = graph.addNode (std::move (synth), {}, UpdateKind::async);
    if (synthNode == nullptr)
        return; // add failed → slot stays empty; previous synth already removed
    synthNodeId = synthNode->nodeID;

    // MIDI: Seq → synth (special MIDI channel index). The SEQUENCER is the synth's MIDI
    // source, not MidiIn: the sequencer already passes the live MIDI-in stream through,
    // so routing from MidiIn as well would double every played note. The full chain is
    // `Transport → MidiIn → Seq → synth → Master`, and the transport → MidiIn ordering
    // edge guarantees all of it renders after the transport.
    //
    // Assert the edge: this is the ONLY path by which generated MIDI reaches the synth,
    // so a silent rejection means total silence — the Phase-4 failure mode.
    {
        [[maybe_unused]] const bool synthMidiWired =
            graph.addConnection ({ { sequencerNodeId, juce::AudioProcessorGraph::midiChannelIndex },
                                   { synthNodeId, juce::AudioProcessorGraph::midiChannelIndex } },
                                 UpdateKind::async);

        // A slot occupant that does not accept MIDI (an audio-only probe in a test, a
        // mis-declared plugin) makes the refusal CORRECT, so scope the assertion to the
        // case where the edge should have taken.
        [[maybe_unused]] const bool synthWantsMidi =
            synthNode->getProcessor () != nullptr && synthNode->getProcessor ()->acceptsMidi ();
        jassert (synthMidiWired || ! synthWantsMidi);
    }

    // Audio: synth → Master, stereo pairs. Attempt both channels; a missing channel
    // (e.g. a mono synth before the wrapper adapts it) fails gracefully. Bus
    // adaptation is the hosting wrapper's responsibility (§6.3), not the engine's.
    for (int ch = 0; ch < numStereoChannels; ++ch)
        graph.addConnection ({ { synthNodeId, ch }, { masterNodeId, ch } }, UpdateKind::async);
}

// MESSAGE-THREAD ONLY:
void EngineGraph::removeSynth ()
{
    if (synthNodeId.uid != 0)
    {
        graph.removeNode (synthNodeId, juce::AudioProcessorGraph::UpdateKind::async);
        synthNodeId = {};
    }
}

// MESSAGE-THREAD ONLY:
void EngineGraph::allNotesOff () noexcept
{
    midiControl.allNotesOffRequested.store (true, std::memory_order_release);
}

// MESSAGE-THREAD ONLY:
void EngineGraph::setMidiChannelMask (std::uint16_t mask) noexcept
{
    midiControl.channelMask.store (mask, std::memory_order_relaxed);
}

// MESSAGE-THREAD ONLY:
double EngineGraph::getSampleRate () const noexcept
{
    return graph.getSampleRate ();
}

// MESSAGE-THREAD ONLY:
int EngineGraph::getBlockSize () const noexcept
{
    return graph.getBlockSize ();
}

// MESSAGE-THREAD ONLY:
void EngineGraph::prepareToPlay (double sampleRate, int blockSize)
{
    const auto sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    const auto block = juce::jmax (1, blockSize);

    graph.setPlayConfigDetails (numStereoChannels, numStereoChannels, sr, block);
    graph.prepareToPlay (sr, block);

    // The transport's sample rate arrives via the transport NODE's prepareToPlay,
    // which the graph drives for every node it owns (both here and on the app path,
    // where AudioProcessorPlayer prepares the graph instead). Assert the plumbing
    // held, so a future topology change that drops the head node fails loudly in
    // debug rather than silently leaving the clock at its 44.1 kHz default — a
    // wrong-rate clock would corrupt every PPQ value and every golden MIDI file.
    jassert (juce::approximatelyEqual (transport.sampleRate (), sr));
}

// MESSAGE-THREAD ONLY:
void EngineGraph::releaseResources ()
{
    graph.releaseResources ();
}

// MESSAGE-THREAD ONLY:
void EngineGraph::setDeviceStatus (DeviceStatus status) noexcept
{
    if (masterProcessor != nullptr)
        masterProcessor->setDeviceStatus (status);
}
} // namespace arpbox::engine
