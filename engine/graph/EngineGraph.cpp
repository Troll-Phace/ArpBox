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

    // IO nodes. audioOutput is the sink for this phase; audioInput and midiInput
    // are added for topology completeness and stay unconnected until Phase 4.
    const auto audioOutNode = graph.addNode (std::make_unique<IOProcessor> (IOProcessor::audioOutputNode));
    graph.addNode (std::make_unique<IOProcessor> (IOProcessor::audioInputNode));
    graph.addNode (std::make_unique<IOProcessor> (IOProcessor::midiInputNode));

    // Test-tone source: inject the shared control block before insertion.
    auto tone = std::make_unique<TestToneProcessor> ();
    tone->setToneControl (&toneControl);
    const auto toneNode = graph.addNode (std::move (tone));

    // Master: inject the shared channels before insertion; keep a non-owning
    // handle for setDeviceStatus forwarding.
    auto master = std::make_unique<MasterProcessor> ();
    master->setSharedState (&commandQueue, &snapshotBuffer, &toneControl);
    masterProcessor = master.get ();
    const auto masterNode = graph.addNode (std::move (master));

    // Wire TestTone → Master → audioOutput as stereo pairs.
    for (int ch = 0; ch < numStereoChannels; ++ch)
        graph.addConnection ({ { toneNode->nodeID, ch }, { masterNode->nodeID, ch } });

    for (int ch = 0; ch < numStereoChannels; ++ch)
        graph.addConnection ({ { masterNode->nodeID, ch }, { audioOutNode->nodeID, ch } });
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
