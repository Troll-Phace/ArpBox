// ─────────────────────────────────────────────────────────────────────────────
// graph_smoke — headless EngineGraph render tests (ARCHITECTURE §3.3, §7;
// Phase 2 success criterion "test tone through graph to device out; meter values
// visible"). Drives the graph with no audio device: construct → prepareToPlay →
// processBlock loop, reading the EngineSnapshot the master publishes.
//
// Signal-flow reminder (ToneControl.h): the master drains the command queue, but
// the tone SOURCE node runs earlier in the block, so a setTestToneEnabled command
// lands on the tone node ONE block later. Tests enable the tone, then process
// extra blocks before asserting non-silence.
// ─────────────────────────────────────────────────────────────────────────────

#include "engine/graph/EngineCommand.h"
#include "engine/graph/EngineGraph.h"
#include "engine/graph/EngineSnapshot.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using arpbox::engine::EngineCommand;
using arpbox::engine::EngineCommandType;
using arpbox::engine::EngineGraph;
using arpbox::engine::EngineSnapshot;

namespace
{
// True if every sample in the buffer is finite (no NaN / Inf).
bool allFinite (const juce::AudioBuffer<float>& buffer) noexcept
{
    for (int ch = 0; ch < buffer.getNumChannels (); ++ch)
    {
        const float* const data = buffer.getReadPointer (ch);
        for (int i = 0; i < buffer.getNumSamples (); ++i)
            if (! std::isfinite (data[i]))
                return false;
    }
    return true;
}

EngineCommand toneCommand (bool enabled) noexcept
{
    EngineCommand c {};
    c.type = EngineCommandType::setTestToneEnabled;
    c.value.i = enabled ? 1 : 0;
    return c;
}
} // namespace

TEST_CASE ("graph/engine-graph: tone disabled renders silence, counter advances", "[unit]")
{
    EngineGraph graph;
    graph.prepareToPlay (48000.0, 128);

    juce::AudioBuffer<float> buffer (2, 128);
    juce::MidiBuffer midi;

    for (int i = 0; i < 8; ++i)
    {
        buffer.clear ();
        graph.getProcessor ().processBlock (buffer, midi);
        REQUIRE (allFinite (buffer));
    }

    const EngineSnapshot& s1 = graph.snapshots ().read ();
    REQUIRE (s1.peakL == 0.0f); // tone off → source clears to silence
    REQUIRE (s1.peakR == 0.0f);
    REQUIRE (s1.rmsL == 0.0f);
    REQUIRE (s1.blockCounter >= 8); // advanced once per processed block

    // The counter keeps advancing on further blocks.
    const std::uint64_t before = s1.blockCounter;
    for (int i = 0; i < 4; ++i)
    {
        buffer.clear ();
        graph.getProcessor ().processBlock (buffer, midi);
    }
    const EngineSnapshot& s2 = graph.snapshots ().read ();
    REQUIRE (s2.blockCounter > before);
}

TEST_CASE ("graph/engine-graph: enabled tone produces non-silent metered output", "[unit]")
{
    // The reliable headless proxy for "test tone through graph to device out"
    // (Phase 2 success criterion) is the master's published EngineSnapshot meter,
    // NOT the caller's output buffer: driving the graph's processBlock directly
    // (no AudioProcessorPlayer / device pull) does not populate the caller buffer
    // from the audio-output node, but the master meters the live signal mid-graph
    // and publishes it. Signal presence is therefore asserted on the snapshot.
    EngineGraph graph;
    graph.prepareToPlay (48000.0, 128);

    juce::AudioBuffer<float> buffer (2, 128);
    juce::MidiBuffer midi;

    REQUIRE (graph.commands ().push (toneCommand (true)));

    // Process well past the one-block command→tone latency so the phasor is
    // running and the meter tap has a full block of signal.
    for (int i = 0; i < 16; ++i)
    {
        buffer.clear ();
        graph.getProcessor ().processBlock (buffer, midi);
        REQUIRE (allFinite (buffer)); // output must never carry NaN/Inf
    }

    const EngineSnapshot& s = graph.snapshots ().read ();
    REQUIRE (s.peakL > 0.0f); // non-silence through the graph (metered at master)
    REQUIRE (s.peakR > 0.0f);
    REQUIRE (s.rmsL > 0.0f);
    REQUIRE (s.rmsR > 0.0f);

    // Debug tone is ~-12 dBFS; comfortably inside [0, 1] at unity master gain.
    REQUIRE (s.peakL <= 1.0f);
    REQUIRE (s.peakR <= 1.0f);
}

TEST_CASE ("graph/engine-graph: safety limiter bounds a hot signal", "[unit]")
{
    // The fixed tone level (~-12 dBFS) boosted +40 dB reaches ~25.0 (linear) at
    // the master. With the default-ON safety limiter it must be held at/under the
    // 0 dBFS ceiling; with the limiter OFF the same signal passes through far
    // above 0 dBFS. Comparing the two proves the limiter — not merely a quiet
    // source — is what bounds the output. Verified on the published meter (see the
    // note in the enabled-tone case about the headless caller buffer).
    EngineGraph graph;
    graph.prepareToPlay (48000.0, 128);

    juce::AudioBuffer<float> buffer (2, 128);
    juce::MidiBuffer midi;

    REQUIRE (graph.commands ().push (toneCommand (true)));

    EngineCommand hotGain {};
    hotGain.type = EngineCommandType::setMasterGainDb;
    hotGain.value.f = 40.0f;
    REQUIRE (graph.commands ().push (hotGain));

    const auto settle = [&] {
        for (int i = 0; i < 80; ++i) // > 20 ms gain ramp + limiter release
        {
            buffer.clear ();
            graph.getProcessor ().processBlock (buffer, midi);
            REQUIRE (allFinite (buffer)); // scrub keeps every sample finite
        }
    };

    // Limiter ON (default): the hot signal is clamped to the 0 dBFS ceiling.
    settle ();
    const float limitedPeak = graph.snapshots ().read ().peakL;
    REQUIRE (limitedPeak > 0.0f);
    REQUIRE (limitedPeak <= 1.0001f);

    // Limiter OFF: the identical +40 dB signal now runs far above 0 dBFS.
    EngineCommand limiterOff {};
    limiterOff.type = EngineCommandType::setLimiterEnabled;
    limiterOff.value.i = 0;
    REQUIRE (graph.commands ().push (limiterOff));

    settle ();
    const float unlimitedPeak = graph.snapshots ().read ().peakL;
    REQUIRE (std::isfinite (unlimitedPeak));
    REQUIRE (unlimitedPeak > 2.0f); // demonstrably unbounded without the limiter
}

TEST_CASE ("graph/engine-graph: processes at multiple block sizes without crashing", "[unit]")
{
    for (const int blockSize : { 32, 128, 512 })
    {
        EngineGraph graph;
        graph.prepareToPlay (48000.0, blockSize);

        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;

        REQUIRE (graph.commands ().push (toneCommand (true)));

        for (int i = 0; i < 8; ++i)
        {
            buffer.clear ();
            graph.getProcessor ().processBlock (buffer, midi);
            REQUIRE (allFinite (buffer));
        }

        const EngineSnapshot& s = graph.snapshots ().read ();
        REQUIRE (std::isfinite (s.peakL));
        REQUIRE (std::isfinite (s.rmsL));
        REQUIRE (s.blockCounter >= 8);

        graph.releaseResources ();
    }
}
