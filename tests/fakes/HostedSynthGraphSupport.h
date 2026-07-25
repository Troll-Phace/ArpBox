// ─────────────────────────────────────────────────────────────────────────────
// HostedSynthGraphSupport — shared helpers for the Phase 4 synth-hosting graph
// tests (hosting_synth_e2e, hosting_swap_stress). These drive a real EngineGraph
// headless (no audio device) with a HostedPluginNode-wrapped fake in the synth
// slot, so the two things they mix — the engine's HEADLESS juce_audio_processors
// and hosting's FULL juce_audio_processors — meet in one TU. That is safe: the full
// module header includes the headless one under its own guard (it is a strict
// superset), and arpbox_tests already links both libraries.
//
// ASYNC DISCIPLINE: EngineGraph::setSynth/removeSynth apply on the message thread
// with UpdateKind::async. Tests MUST pump the MessageManager (settleGraphEdits)
// after such a call before asserting on routed audio. The headless render proxy is
// the master's published EngineSnapshot meter, NOT the caller buffer (driving the
// graph's processBlock directly does not populate the caller buffer from the
// audio-output node — see tests/graph_smoke.cpp for the same rationale).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "fakes/FakePlugins.h"

#include "engine/graph/EngineGraph.h"
#include "engine/graph/EngineSnapshot.h"
#include "engine/graph/NoteEvent.h"

#include "hosting/HostedPluginNode.h"

#include <juce_events/juce_events.h>

#include <cmath>
#include <cstdint>
#include <memory>

namespace arpbox::testing
{
inline constexpr double kGraphSampleRate = 48000.0;
inline constexpr int kGraphBlockSize = 128;

/** True if every sample in every channel is finite (no NaN / Inf). */
inline bool allFinite (const juce::AudioBuffer<float>& buffer) noexcept
{
    for (int ch = 0; ch < buffer.getNumChannels (); ++ch)
    {
        const float* const d = buffer.getReadPointer (ch);
        for (int i = 0; i < buffer.getNumSamples (); ++i)
            if (! std::isfinite (d[i]))
                return false;
    }
    return true;
}

/** Builds a HostedPluginNode wrapping an already-prepared baseline instrument fake,
    returned as the base juce::AudioProcessor the engine expects. */
inline std::unique_ptr<juce::AudioProcessor> makeWrappedBaselineSynth ()
{
    auto inner = makeFakeInstance (specFor (FakeBehavior::baseline));
    inner->prepareToPlay (kGraphSampleRate, kGraphBlockSize); // ctor contract: prepared inner
    return std::make_unique<hosting::HostedPluginNode> (std::move (inner));
}

/** Same, but also hands back the non-owning wrapper pointer (valid until the node is
    removed from the graph) so a test can drive its fade handshake. */
inline std::unique_ptr<juce::AudioProcessor> makeWrappedBaselineSynth (hosting::HostedPluginNode*& outNode)
{
    auto inner = makeFakeInstance (specFor (FakeBehavior::baseline));
    inner->prepareToPlay (kGraphSampleRate, kGraphBlockSize);
    auto node = std::make_unique<hosting::HostedPluginNode> (std::move (inner));
    outNode = node.get ();
    return node;
}

/** Pumps the message loop in bounded slices so a queued async graph edit
    (setSynth/removeSynth) is applied before the caller renders. The total budget is
    a HANG GUARD, not a synchronization primitive — the edit is posted immediately by
    the setSynth call and dispatched on the first slice under normal conditions. */
inline void settleGraphEdits (int slices = 20, int msPerSlice = 5)
{
    for (int i = 0; i < slices; ++i)
        juce::MessageManager::getInstance ()->runDispatchLoopUntil (msPerSlice);
}

/** Renders `n` blocks through the graph and returns the last published left peak. */
inline float renderBlocks (engine::EngineGraph& graph, juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, int n)
{
    for (int i = 0; i < n; ++i)
    {
        buffer.clear ();
        graph.getProcessor ().processBlock (buffer, midi);
    }
    return graph.snapshots ().read ().peakL;
}

/** Pushes a note-on onto the graph's QWERTY/pad channel. */
inline void pushNoteOn (engine::EngineGraph& graph, int note, int velocity = 100, int channel = 1)
{
    graph.notes ().push (engine::NoteEvent { engine::NoteEventKind::noteOn,
                                             static_cast<std::uint8_t> (note),
                                             static_cast<std::uint8_t> (velocity),
                                             static_cast<std::uint8_t> (channel) });
}

/** Pushes a note-off onto the graph's QWERTY/pad channel. */
inline void pushNoteOff (engine::EngineGraph& graph, int note, int channel = 1)
{
    graph.notes ().push (engine::NoteEvent { engine::NoteEventKind::noteOff,
                                             static_cast<std::uint8_t> (note),
                                             0,
                                             static_cast<std::uint8_t> (channel) });
}

/** Advances the graph until `node` reports its fade-out handshake complete, or the
    block budget is exhausted. Returns true on completion (false ⇒ would-hang guard
    tripped). Rendering is what advances the node's audio-thread fade ramp. */
inline bool renderUntilFadeOut (engine::EngineGraph& graph,
                                juce::AudioBuffer<float>& buffer,
                                juce::MidiBuffer& midi,
                                const hosting::HostedPluginNode& node,
                                int budgetBlocks = 64)
{
    for (int b = 0; b < budgetBlocks; ++b)
    {
        buffer.clear ();
        graph.getProcessor ().processBlock (buffer, midi);
        if (node.isFadeOutComplete ())
            return true;
    }
    return node.isFadeOutComplete ();
}
} // namespace arpbox::testing
