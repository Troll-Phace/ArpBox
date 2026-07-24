#include "EngineGraph.h"

#include "TestToneProcessor.h"

namespace arpbox::engine
{
namespace
{
    constexpr int numStereoChannels = 2;
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

    // IO nodes. audioOutput is the sink; audioInput and the graph's own midiInput
    // IO node are added for topology completeness and stay unconnected (hardware
    // MIDI enters via the shared collector on the MidiInputProcessor, not here).
    const auto audioOutNode = graph.addNode (std::make_unique<IOProcessor> (IOProcessor::audioOutputNode));
    graph.addNode (std::make_unique<IOProcessor> (IOProcessor::audioInputNode));
    graph.addNode (std::make_unique<IOProcessor> (IOProcessor::midiInputNode));

    // MIDI-In node: merges QWERTY/pad (noteQueue) + hardware (midiCollector),
    // applies channel filtering, publishes the MIDI-in voice count. Inject the
    // shared state before insertion. Left MIDI-unconnected until a synth is set.
    auto midiIn = std::make_unique<MidiInputProcessor> ();
    midiIn->setSharedState (&noteQueue, &midiCollector, &midiControl);
    midiInputProcessor = midiIn.get ();
    const auto midiInNode = graph.addNode (std::move (midiIn));
    midiInputNodeId = midiInNode->nodeID;

    // Test-tone source (debug fallback): inject the shared control block before insertion.
    auto tone = std::make_unique<TestToneProcessor> ();
    tone->setToneControl (&toneControl);
    const auto toneNode = graph.addNode (std::move (tone));

    // Master: inject the shared channels + event queue (latencyChanged) + latency
    // source (the graph itself) + voice-count source before insertion; keep a
    // non-owning handle for setDeviceStatus forwarding.
    auto master = std::make_unique<MasterProcessor> ();
    master->setSharedState (&commandQueue, &snapshotBuffer, &toneControl);
    master->setEventQueue (&eventQueue);
    master->setLatencySource (&graph);
    master->setVoiceCountSource (&midiControl.voiceCount);
    masterProcessor = master.get ();
    const auto masterNode = graph.addNode (std::move (master));
    masterNodeId = masterNode->nodeID;

    // Wire TestTone → Master → audioOutput as stereo pairs. The tone edge is kept
    // as a debug fallback source (tone off by default); a set synth sums into the
    // same Master input.
    for (int ch = 0; ch < numStereoChannels; ++ch)
        graph.addConnection ({ { toneNode->nodeID, ch }, { masterNodeId, ch } });

    for (int ch = 0; ch < numStereoChannels; ++ch)
        graph.addConnection ({ { masterNodeId, ch }, { audioOutNode->nodeID, ch } });
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

    // MIDI: MIDI-In → synth (special MIDI channel index).
    graph.addConnection ({ { midiInputNodeId, juce::AudioProcessorGraph::midiChannelIndex },
                           { synthNodeId, juce::AudioProcessorGraph::midiChannelIndex } },
                         UpdateKind::async);

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
