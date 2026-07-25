// ─────────────────────────────────────────────────────────────────────────────
// hosting_swap_stress — swap-under-playback stress (docs/INSTRUCTIONS.md Phase 4.4
// "swap-under-playback stress"; success criterion "Synth swap mid-note: no click,
// no hang, no stuck note"; ARCHITECTURE §6.2, §6.3, §5.5).
//
// Two tests:
//   1. A single deterministic fade-handshake swap under a held note, asserting no
//      metered spike across the crossfade (click proxy at block resolution — the
//      per-SAMPLE ramp is asserted in hosted_node_unit's fade handshake), completion
//      within a bounded block budget (no hang), and the new synth is live afterward.
//   2. Seeded churn of load / swap / remove / note events over many blocks, asserting
//      finite, limiter-bounded output throughout, no fade-out hang, and — after a
//      final all-notes-off + remove — zero voices and silence (no stuck note).
//
// Signal proxy: the master's EngineSnapshot meter (see HostedSynthGraphSupport.h).
// Every randomized run takes an explicit, printed seed (.claude/rules/testing.md).
// Fakes only.
// ─────────────────────────────────────────────────────────────────────────────

#include "fakes/HostedSynthGraphSupport.h"
#include "fakes/HostingLabSupport.h" // MessageScope

#include "engine/graph/EngineGraph.h"
#include "engine/graph/EngineSnapshot.h"

#include "hosting/HostedPluginNode.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <random>

using arpbox::engine::EngineGraph;
using arpbox::hosting::HostedPluginNode;
using namespace arpbox::testing;

TEST_CASE ("hosting/swap: fade-handshake swap under a held note is spike-free and live", "[hosting-lab]")
{
    MessageScope juceInit;

    EngineGraph graph;
    graph.prepareToPlay (kGraphSampleRate, kGraphBlockSize);

    juce::AudioBuffer<float> buffer (2, kGraphBlockSize);
    juce::MidiBuffer midi;

    // Synth A playing a held note.
    HostedPluginNode* a = nullptr;
    graph.setSynth (makeWrappedBaselineSynth (a));
    settleGraphEdits ();
    REQUIRE (a != nullptr);

    pushNoteOn (graph, 60, 100);
    const float playingPeak = renderBlocks (graph, buffer, midi, 16);
    REQUIRE (playingPeak > 0.0f);

    // ── Swap A → B via the coordinator recipe ────────────────────────────────
    // 1. Fade A out; render until it reports silent (bounded → no hang).
    a->fadeOut ();
    REQUIRE (renderUntilFadeOut (graph, buffer, midi, *a));
    REQUIRE (renderBlocks (graph, buffer, midi, 2) <= playingPeak + 1.0e-4f); // A silent, no spike

    // 2. Build B armed-silent (fadeOut before insertion), swap it in, settle.
    HostedPluginNode* b = nullptr;
    auto nodeB = makeWrappedBaselineSynth (b);
    b->fadeOut ();
    graph.setSynth (std::move (nodeB)); // removes A (freed on the message thread), inserts B
    settleGraphEdits ();
    REQUIRE (b != nullptr);

    // 3. Fade B in and re-trigger (a fresh instance never saw the earlier note-on).
    b->fadeIn ();
    pushNoteOn (graph, 64, 100);

    float maxPeak = 0.0f;
    for (int i = 0; i < 24; ++i)
    {
        const float p = renderBlocks (graph, buffer, midi, 1);
        REQUIRE (allFinite (buffer));
        REQUIRE (std::isfinite (p));
        REQUIRE (p <= 1.0001f); // limiter-bounded throughout
        maxPeak = juce::jmax (maxPeak, p);
    }
    REQUIRE (maxPeak > 0.0f);                   // B is live after the swap
    REQUIRE (maxPeak <= playingPeak + 1.0e-4f); // no crossfade overshoot / spike

    // No stuck note: flush + remove returns the slot to silence.
    graph.allNotesOff ();
    graph.removeSynth ();
    settleGraphEdits ();
    REQUIRE (renderBlocks (graph, buffer, midi, 4) == 0.0f);
    REQUIRE (graph.snapshots ().read ().voiceCount == 0);
}

TEST_CASE ("hosting/swap: seeded load/swap/remove/note churn stays sane and leaves no stuck note", "[hosting-lab]")
{
    const unsigned seed = GENERATE (1u, 1337u, 424242u);
    INFO ("seed = " << seed);

    MessageScope juceInit;
    std::mt19937 rng (seed);

    EngineGraph graph;
    graph.prepareToPlay (kGraphSampleRate, kGraphBlockSize);

    juce::AudioBuffer<float> buffer (2, kGraphBlockSize);
    juce::MidiBuffer midi;

    HostedPluginNode* current = nullptr; // non-owning; valid while wired in the graph

    constexpr int kIterations = 64;
    std::uniform_int_distribution<int> action (0, 9);
    std::uniform_int_distribution<int> pitch (48, 72);

    for (int it = 0; it < kIterations; ++it)
    {
        const int a = action (rng);

        if (a <= 2) // load / swap (30%)
        {
            if (current != nullptr)
            {
                // Coordinator recipe: fade the outgoing node out, wait for silence.
                current->fadeOut ();
                REQUIRE (renderUntilFadeOut (graph, buffer, midi, *current)); // no hang
            }

            HostedPluginNode* next = nullptr;
            auto node = makeWrappedBaselineSynth (next);
            next->fadeOut (); // armed silent so fadeIn ramps up click-free
            graph.setSynth (std::move (node));
            settleGraphEdits ();
            next->fadeIn ();
            current = next;
        }
        else if (a == 3) // remove (10%)
        {
            graph.allNotesOff ();
            graph.removeSynth ();
            settleGraphEdits ();
            current = nullptr;
        }
        else if (a <= 6) // note-on (30%)
        {
            pushNoteOn (graph, pitch (rng));
        }
        else if (a <= 8) // note-off (20%)
        {
            pushNoteOff (graph, pitch (rng));
        }
        else // explicit flush (10%)
        {
            graph.allNotesOff ();
        }

        // Every iteration renders and re-checks the hard invariants.
        const float peak = renderBlocks (graph, buffer, midi, 3);
        REQUIRE (allFinite (buffer));
        REQUIRE (std::isfinite (peak));
        REQUIRE (peak <= 1.0001f); // never runs away past the master ceiling
    }

    // Teardown: flush every held note and remove the synth → no stuck note, silence.
    graph.allNotesOff ();
    if (current != nullptr)
    {
        graph.removeSynth ();
        settleGraphEdits ();
        current = nullptr;
    }

    const float finalPeak = renderBlocks (graph, buffer, midi, 8);
    REQUIRE (allFinite (buffer));
    REQUIRE (finalPeak == 0.0f);
    REQUIRE (graph.snapshots ().read ().voiceCount == 0);
}
