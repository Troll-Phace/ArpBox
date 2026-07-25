// ─────────────────────────────────────────────────────────────────────────────
// hosting_synth_e2e — the headline Phase 4 oracle (docs/INSTRUCTIONS.md Phase 4.4
// "fake-synth end-to-end (note event → rendered audio non-silence)"; ARCHITECTURE
// §3.3, §6.3). A HostedPluginNode-wrapped baseline instrument is inserted into a
// real EngineGraph via setSynth and driven headlessly: a QWERTY/pad note-on routes
// MIDI-In → synth → Master and produces NON-SILENT metered output; note-off returns
// it to silence. The live MIDI-in voice count tracks the held note.
//
// Signal proxy: the master's published EngineSnapshot meter (peakL), not the caller
// buffer — see tests/graph_smoke.cpp / HostedSynthGraphSupport.h for why.
// Fakes only (.claude/rules/testing.md).
// ─────────────────────────────────────────────────────────────────────────────

#include "fakes/HostedSynthGraphSupport.h"
#include "fakes/HostingLabSupport.h" // MessageScope

#include "engine/graph/EngineGraph.h"
#include "engine/graph/EngineSnapshot.h"

#include <catch2/catch_test_macros.hpp>

using arpbox::engine::EngineGraph;
using namespace arpbox::testing;

TEST_CASE ("hosting/synth-e2e: note-on drives the hosted synth to non-silent audio", "[hosting-lab]")
{
    MessageScope juceInit; // async setSynth needs the message loop to apply

    EngineGraph graph;
    graph.prepareToPlay (kGraphSampleRate, kGraphBlockSize);

    // Insert the wrapped baseline instrument and let the async graph rebuild apply.
    graph.setSynth (makeWrappedBaselineSynth ());
    settleGraphEdits ();

    juce::AudioBuffer<float> buffer (2, kGraphBlockSize);
    juce::MidiBuffer midi;

    SECTION ("synth present but no note held → silence, zero voices, finite output")
    {
        const float peak = renderBlocks (graph, buffer, midi, 8);
        REQUIRE (allFinite (buffer));
        REQUIRE (peak == 0.0f);
        REQUIRE (graph.snapshots ().read ().voiceCount == 0);
    }

    SECTION ("note-on → non-silent metered output and one live voice")
    {
        pushNoteOn (graph, 60, 100);
        const float peak = renderBlocks (graph, buffer, midi, 16); // route + fade settle
        REQUIRE (allFinite (buffer));
        REQUIRE (peak > 0.0f);                                // audio non-silence
        REQUIRE (peak <= 1.0001f);                            // bounded by the master
        REQUIRE (graph.snapshots ().read ().voiceCount == 1); // held note tracked

        SECTION ("note-off → returns to silence and zero voices")
        {
            pushNoteOff (graph, 60);
            const float released = renderBlocks (graph, buffer, midi, 8);
            REQUIRE (allFinite (buffer));
            REQUIRE (released == 0.0f);
            REQUIRE (graph.snapshots ().read ().voiceCount == 0);
        }
    }
}

TEST_CASE ("hosting/synth-e2e: removeSynth returns the slot to silence", "[hosting-lab]")
{
    MessageScope juceInit;

    EngineGraph graph;
    graph.prepareToPlay (kGraphSampleRate, kGraphBlockSize);

    graph.setSynth (makeWrappedBaselineSynth ());
    settleGraphEdits ();

    juce::AudioBuffer<float> buffer (2, kGraphBlockSize);
    juce::MidiBuffer midi;

    // Sound the synth first so removal has something to silence.
    pushNoteOn (graph, 60, 100);
    REQUIRE (renderBlocks (graph, buffer, midi, 16) > 0.0f);

    // Remove the synth and flush; the slot must fall silent (no stuck synth output).
    graph.allNotesOff ();
    graph.removeSynth ();
    settleGraphEdits ();

    const float peak = renderBlocks (graph, buffer, midi, 8);
    REQUIRE (allFinite (buffer));
    REQUIRE (peak == 0.0f);
    REQUIRE (graph.snapshots ().read ().voiceCount == 0);
}
