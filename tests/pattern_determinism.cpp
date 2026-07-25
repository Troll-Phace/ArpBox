// ─────────────────────────────────────────────────────────────────────────────
// pattern_determinism — the REPRODUCIBLE half of ARCHITECTURE §1.2
// ("Everything random is seeded… same (pattern, seeds, N bars) ⇒ byte-identical
// MIDI, forever").
//
// ── WHY THIS IS NOT ALREADY COVERED ─────────────────────────────────────────
// transport_timing.cpp proves the BUFFER-SIZE-INDEPENDENT half: one engine
// instance, one configuration, many block sizes, identical output. That is a
// different claim from the reproducible half, and it is blind to exactly one
// failure mode — hidden state that persists ACROSS engine instances. A static
// counter, a lazily-initialised cache keyed on nothing, a `std::random_device`
// smuggled into a traversal-table build: all of them survive the buffer-size
// sweep unscathed, because every render in that sweep observes the same
// process-wide state in the same order.
//
// So the shape here is deliberately the mirror image: ONE block size, TWO fresh
// engine instances built from identical edits, byte-compared. And a third,
// DIFFERENT render is run BETWEEN them, so that any process-wide state the engine
// might be carrying has been disturbed before the second render observes it.
//
// The configuration leans on the parts most likely to hold such state: the two
// SEEDED direction modes (`walk` and `randomNoRepeat`, whose traversal tables are
// generated from the pattern's `masterSeed` at snapshot-build time), a fractional
// step grid where nothing lands on an integer, and a quantized pattern switch.
// ─────────────────────────────────────────────────────────────────────────────

#include "support/MidiRenderHarness.h"
#include "support/NoteLifecycleCheck.h"
#include "support/SequencerRenderRig.h"

#include "engine/graph/EngineCommand.h"
#include "engine/sequencer/PatternDocument.h"
#include "engine/sequencer/PatternTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <vector>

using arpbox::engine::DirectionMode;
using arpbox::engine::EngineCommandType;
using arpbox::engine::LaneId;
using arpbox::engine::maxSteps;
using arpbox::engine::PatternDocument;
using arpbox::engine::QuantizeMode;
using arpbox::testing::engineCommand;
using arpbox::testing::MidiRenderConfig;
using arpbox::testing::MidiRenderResult;
using arpbox::testing::NoteLifecycleTracker;
using arpbox::testing::patternSwitchCommand;
using arpbox::testing::renderSequencer;
using arpbox::testing::ScheduledCommand;
using arpbox::testing::SequencerRig;

namespace
{
// 137 BPM @ 44.1 kHz: a 1/16 note is 4828.467… samples, so nothing in this render
// lands on a round number and no accidental alignment can mask a difference.
constexpr double testSampleRate = 44100.0;
constexpr double testBpm = 137.0;
constexpr int testBlockSize = 128;
constexpr std::int64_t spanSamples = 512000;

/** The configuration under test: polymetric lanes, both seeded direction modes,
    two audible patterns and long gates. Applied as ONE transaction so the
    document's undo/publish bookkeeping is exercised the way the UI would. */
void configure (PatternDocument& document)
{
    document.beginTransaction ();

    // Pattern 0 — a brownian `walk` over polymetric lanes.
    document.setDirection (0, DirectionMode::walk);
    document.setMasterSeed (0, 0x0123456789ABCDEFULL);
    document.setLaneLength (0, LaneId::gate, 7);
    document.setLaneLength (0, LaneId::pitch, 5);
    document.setLaneLength (0, LaneId::vel, 3);
    document.setLaneDivision (0, LaneId::oct, 3);

    constexpr int gate7[7] = { 1, 1, 0, 1, 0, 1, 1 };
    for (int s = 0; s < 7; ++s)
        document.setLaneValue (0, LaneId::gate, s, gate7[s]);

    constexpr int pitch5[5] = { 0, 2, -1, 4, -3 };
    for (int s = 0; s < 5; ++s)
        document.setLaneValue (0, LaneId::pitch, s, pitch5[s]);

    constexpr int vel3[3] = { 40, 80, 120 };
    for (int s = 0; s < 3; ++s)
        document.setLaneValue (0, LaneId::vel, s, vel3[s]);

    document.setLaneValue (0, LaneId::oct, 0, -1);
    document.setLaneValue (0, LaneId::oct, 1, 1);

    // Pattern 1 — a euclidean gate lane traversed with `randomNoRepeat`.
    document.setDirection (1, DirectionMode::randomNoRepeat);
    document.setMasterSeed (1, 0xFEDCBA9876543210ULL);
    document.applyEuclid (1, 13, 5, 3);
    for (int s = 0; s < maxSteps; ++s)
        document.setLaneValue (1, LaneId::vel, s, 96);

    // Long gates on both, so flushes have work to do at every boundary.
    for (int s = 0; s < maxSteps; ++s)
    {
        document.setLaneValue (0, LaneId::len, s, 175);
        document.setLaneValue (1, LaneId::len, s, 175);
    }

    document.endTransaction ();
}

/** A different configuration, used only to disturb any process-wide state between
    the two renders being compared. */
void configureDecoy (PatternDocument& document)
{
    document.beginTransaction ();
    document.setDirection (0, DirectionMode::outsideIn);
    document.setMasterSeed (0, 0xAAAAAAAAAAAAAAAAULL);
    document.setLaneLength (0, LaneId::gate, 11);
    document.setLaneLength (0, LaneId::pitch, 4);
    for (int s = 0; s < 11; ++s)
        document.setLaneValue (0, LaneId::gate, s, (s % 3 == 0) ? 1 : 0);
    document.endTransaction ();
}

std::vector<ScheduledCommand> script ()
{
    return { ScheduledCommand { 0, engineCommand (EngineCommandType::setTempoBpm, testBpm) },
             ScheduledCommand { 0, engineCommand (EngineCommandType::transportPlay) },
             ScheduledCommand { 51200, patternSwitchCommand (1, QuantizeMode::bar) },
             ScheduledCommand { 179200, engineCommand (EngineCommandType::transportLocate, 21.375) },
             ScheduledCommand { 281600, patternSwitchCommand (0, QuantizeMode::beat) },
             ScheduledCommand { 358400, engineCommand (EngineCommandType::setTempoBpm, 71.5) },
             ScheduledCommand { 435200, engineCommand (EngineCommandType::transportStop) },
             ScheduledCommand { 448000, engineCommand (EngineCommandType::transportPlay) } };
}

MidiRenderConfig renderConfig ()
{
    auto config = MidiRenderConfig::samples (spanSamples, testSampleRate, testBlockSize);
    config.numChannels = 1;
    config.eventReserve = 16384;
    return config;
}

/** One complete render from a FRESH engine instance. */
MidiRenderResult renderOnce (void (*configureFn) (PatternDocument&))
{
    SequencerRig rig { testSampleRate, testBlockSize };
    configureFn (rig.patternDocument);
    return renderSequencer (rig, renderConfig (), script ());
}
} // namespace

TEST_CASE ("determinism/repeat: two fresh renders of the same configuration are byte-identical", "[determinism]")
{
    for (const auto& entry : script ())
        REQUIRE (entry.atSample % testBlockSize == 0);

    const auto first = renderOnce (&configure);

    // Disturb anything process-wide BETWEEN the two comparable renders. If the
    // engine were carrying hidden state — a static RNG, a memoised traversal table
    // keyed on nothing, a counter — this is what would poison the second render.
    const auto decoy = renderOnce (&configureDecoy);

    const auto second = renderOnce (&configure);

    INFO (first.describeDifference (second));

    // ── ANTI-VACUITY ─────────────────────────────────────────────────────────
    // Two empty renders compare equal, so the render has to be shown to be a real
    // performance first — and the decoy has to be shown to be genuinely different,
    // or it disturbed nothing.
    REQUIRE (first.size () > 80u);
    REQUIRE (first.isSampleSorted ());
    REQUIRE (! decoy.empty ());
    REQUIRE (decoy != first);

    // ── THE CONTRACT ─────────────────────────────────────────────────────────
    REQUIRE (second == first);
    REQUIRE (second.toByteStream () == first.toByteStream ());

    // A third run for good measure, after yet another disturbance.
    const auto disturbedAgain = renderOnce (&configureDecoy);
    REQUIRE (! disturbedAgain.empty ());
    const auto third = renderOnce (&configure);
    INFO (first.describeDifference (third));
    REQUIRE (third == first);

    NoteLifecycleTracker tracker;
    tracker.observeAll (first);
    INFO (tracker.describe ());
    REQUIRE (tracker.noteOnsSeen () > 40);
    REQUIRE (tracker.orphanNoteOffs () == 0);
}

TEST_CASE ("determinism/edit-order: the same pattern reached by a different edit order renders identically",
           "[determinism]")
{
    // The document is a full-state model with an undo stack, a transaction depth and
    // a build counter, and the snapshot is built from that state each time. None of
    // that may leak into the OUTPUT: two documents holding equal state must publish
    // snapshots that play identically, however they got there.
    //
    // This is the guard against a builder that ever became incremental — caching
    // gate prefixes or traversal sets across builds and missing an invalidation.
    const auto direct = renderOnce (&configure);

    SequencerRig windingRig { testSampleRate, testBlockSize };
    auto& document = windingRig.patternDocument;

    // Reach the same state the long way round: apply a different configuration,
    // undo it, apply some edits and undo them, THEN configure.
    configureDecoy (document);
    REQUIRE (document.undo ());
    REQUIRE (document.setLaneValue (5, LaneId::modA, 3, 99));
    REQUIRE (document.setGrid (0.5));
    REQUIRE (document.undo ());
    REQUIRE (document.undo ());
    REQUIRE (document.getUndoDepth () == 0);

    configure (document);

    const auto winding = renderSequencer (windingRig, renderConfig (), script ());

    INFO (direct.describeDifference (winding));
    REQUIRE (! direct.empty ());
    REQUIRE (winding == direct);
    REQUIRE (winding.toByteStream () == direct.toByteStream ());
}
