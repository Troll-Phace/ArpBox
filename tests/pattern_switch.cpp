// ─────────────────────────────────────────────────────────────────────────────
// pattern_switch — Phase 6.1's QUANTIZED PATTERN SWITCH and its §5.5 flush
// (ARCHITECTURE §5.2 "Quantized apply", §5.5 "MIDI correctness invariants";
// INSTRUCTIONS Phase 6 success criterion "Pattern switch lands exactly on the
// chosen quantize boundary").
//
// ── THE ONE CONTROL THAT MAKES THIS SUITE A GUARD ───────────────────────────
// Four modes, four hand-computed boundary literals — and, crucially, FOUR
// DISTINCT ones. Asserting each mode against its own literal in isolation does
// NOT catch the single most likely defect in a quantizer, which is the enum
// being ignored entirely: a `resolvePendingSwitch` that always used, say, the
// step ceiling would still have to fail four separate literal assertions, but a
// test author who chose a command sample where two or more modes coincide would
// never notice. So the command sample below is chosen so that
// `instant < beat < bar < patternEnd` strictly, and the distinctness itself is
// asserted out loud after the sweep.
//
// ── THE MUSICAL SETUP, AND WHY EVERY NUMBER IS WHAT IT IS ───────────────────
// 120 BPM @ 48 kHz on the 1/16 grid: one step is EXACTLY 6000 samples, a beat
// 24000 and a 4/4 bar 96000, so every boundary below is an exact integer sample
// and can be asserted literally rather than approximately.
//
//   * Pattern A (index 0) keeps the documented defaults except its GATE lane
//     LENGTH, which is 10. That makes its loop 10 steps = 2.5 quarters — which
//     is what pulls `patternEnd` off both the beat grid and the bar grid. At the
//     default length of 16 the pattern loop is exactly one bar and `bar` and
//     `patternEnd` would be indistinguishable.
//   * Pattern B (index 1) is A with GATE turned on and VEL 111 instead of 100.
//     The two patterns therefore differ in EXACTLY ONE MIDI BYTE, so "which
//     pattern emitted this note" is a byte comparison and not an inference.
//     Their pitch sequences are deliberately identical (both gate every step, so
//     both have gated ordinal == step index over the same 8-note stub pool),
//     which means a switch changes the velocity byte and nothing else.
//
// The switch command is pushed at sample 61440 = lcm(32,64,96,128,256,480,512,
// 1024,2048,4096), the smallest sample that is a block head at EVERY swept block
// size — a command scheduled off a block head would be consumed at a different
// absolute sample per block size and the cross-size comparison would fail for a
// reason that has nothing to do with quantization. 61440 samples is PPQ 2.56,
// which is on no step, beat, bar or pattern boundary — so all four modes have
// real work to do, and the command sample is never the boundary sample.
//
// ── GATE LENGTH 50% IS ALSO LOAD-BEARING ────────────────────────────────────
// Every quantize boundary is a step boundary, and at a 50% gate the previous
// note was released half a step earlier — so the switch flush finds an EMPTY
// sounding-note table and emits nothing. That keeps the mode sweep about
// placement only. The flush cases at the bottom of this file deliberately use a
// 150% gate so a note IS always sounding at a step boundary, which is the only
// way the flush becomes observable at all.
// ─────────────────────────────────────────────────────────────────────────────

#include "support/MidiRenderHarness.h"
#include "support/NoteLifecycleCheck.h"
#include "support/SequencerRenderRig.h"

#include "engine/graph/EngineCommand.h"
#include "engine/sequencer/PatternDocument.h"
#include "engine/sequencer/PatternTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using arpbox::engine::EngineCommandType;
using arpbox::engine::LaneId;
using arpbox::engine::PatternDocument;
using arpbox::engine::QuantizeMode;
using arpbox::testing::engineCommand;
using arpbox::testing::MidiRenderConfig;
using arpbox::testing::MidiRenderResult;
using arpbox::testing::NoteLifecycleTracker;
using arpbox::testing::patternSwitchCommand;
using arpbox::testing::renderSequencer;
using arpbox::testing::ScheduledCommand;
using arpbox::testing::scheduleIsBlockAligned;
using arpbox::testing::SequencerRig;
using arpbox::testing::TimedMidiEvent;

namespace
{
constexpr double testSampleRate = 48000.0;
constexpr double testBpm = 120.0;

/** 1/16 note at 120 BPM / 48 kHz — an exact integer, which is why every literal
    in this file is exact. */
constexpr std::int64_t stepSamples = 6000;
constexpr std::int64_t beatSamples = 4 * stepSamples; ///< one quarter note
constexpr std::int64_t barSamples = 16 * stepSamples; ///< 4/4

constexpr int patternA = 0;
constexpr int patternB = 1;

/** `laneDefault (LaneId::vel)` — pattern A's velocity, unmodified. */
constexpr int velocityA = 100;
/** Pattern B's velocity. THE one byte that distinguishes the two patterns. */
constexpr int velocityB = 111;

/** Pattern A's GATE lane length: 10 steps = 2.5 quarters, so its loop lands on
    neither the beat grid nor the bar grid (see the header note). */
constexpr int gateLengthA = 10;

/** lcm of every swept block size — the only samples that are block heads
    everywhere. */
constexpr std::int64_t blockAlignmentUnit = 61440;

/** Where the switch command is pushed: PPQ 2.56, on no musical boundary. */
constexpr std::int64_t commandSample = blockAlignmentUnit;

/** Long enough to contain the latest boundary (120000) with room after it, and a
    multiple of `blockAlignmentUnit` so every block size covers it exactly. */
constexpr std::int64_t sweepSpanSamples = 3 * blockAlignmentUnit; // 184320

/** The ten swept block sizes: the eight powers of two the criterion names plus 96
    and 480, the non-power-of-two buffers real CoreAudio devices hand us. */
constexpr int sweptBlockSizes[] = { 32, 64, 96, 128, 256, 480, 512, 1024, 2048, 4096 };

/** One quantize mode with its hand-computed landing point.

    Derivation, with startPpq = 61440 / 24000 = 2.56 and stepPpq = 0.25 (the
    snapped ceiling's 1e-6 tolerance never changes any of these, since none of the
    quotients is within 1e-6 of an integer):

      instant     targetPpq = startPpq            = 2.56
                  step = ceil (2.56 / 0.25)       = 11   → 66000
      beat        targetPpq = ceil (2.56)         = 3.0
                  step = 3.0 / 0.25               = 12   → 72000
      bar         targetPpq = ceil (2.56/4) * 4   = 4.0
                  step = 4.0 / 0.25               = 16   → 96000
      patternEnd  periodPpq = 10 steps * 0.25     = 2.5
                  targetPpq = ceil (2.56/2.5)*2.5 = 5.0
                  step = 5.0 / 0.25               = 20   → 120000 */
struct QuantizeCase
{
    QuantizeMode mode;
    const char* name;
    std::int64_t adoptStep;
    std::int64_t boundarySample;
};

constexpr QuantizeCase quantizeCases[] = {
    { QuantizeMode::instant, "instant", 11, 11 * stepSamples },
    { QuantizeMode::beat, "beat", 12, 12 * stepSamples },
    { QuantizeMode::bar, "bar", 16, 16 * stepSamples },
    { QuantizeMode::patternEnd, "patternEnd", 20, 20 * stepSamples },
};

/** Writes the two-pattern setup described in the header into `document`, as ONE
    transaction (so it costs one undo entry and one snapshot build rather than
    ~200).

    @param lenPercent  LEN lane value for both patterns, as a percentage of the
                       step (§12.1). 50 keeps the table empty at every step
                       boundary; anything above 100 guarantees a note is sounding
                       there, which is what makes a flush observable. */
void configureSwitchPatterns (PatternDocument& document, int lenPercent)
{
    document.beginTransaction ();

    // Pattern A's loop length — the whole reason `patternEnd` is distinguishable.
    document.setLaneLength (patternA, LaneId::gate, gateLengthA);

    for (int step = 0; step < arpbox::engine::maxSteps; ++step)
    {
        document.setLaneValue (patternA, LaneId::gate, step, 1);
        document.setLaneValue (patternB, LaneId::gate, step, 1);
        document.setLaneValue (patternB, LaneId::vel, step, velocityB);
        document.setLaneValue (patternA, LaneId::len, step, lenPercent);
        document.setLaneValue (patternB, LaneId::len, step, lenPercent);
    }

    document.endTransaction ();
}

/** A rig at the file's fixed rate with the two-pattern setup already published. */
std::unique_ptr<SequencerRig> makeSwitchRig (int blockSize, int lenPercent = 50)
{
    auto rig = std::make_unique<SequencerRig> (testSampleRate, blockSize);
    configureSwitchPatterns (rig->patternDocument, lenPercent);
    return rig;
}

/** Tempo + play at sample 0. The tempo command is NOT optional even though 120 is
    `Transport::defaultBpm`: every literal in this file is derived from 120 BPM, and
    saying so in the schedule is what keeps the derivation checkable. */
std::vector<ScheduledCommand> startPlaying ()
{
    return { ScheduledCommand { 0, engineCommand (EngineCommandType::setTempoBpm, testBpm) },
             ScheduledCommand { 0, engineCommand (EngineCommandType::transportPlay) } };
}

MidiRenderConfig spanConfig (std::int64_t spanSamples, int blockSize)
{
    auto config = MidiRenderConfig::samples (spanSamples, testSampleRate, blockSize);
    config.numChannels = 1;
    config.eventReserve = 16384;
    return config;
}

std::vector<TimedMidiEvent> noteOnsOf (const MidiRenderResult& render)
{
    return render.select ([] (const TimedMidiEvent& event) { return event.message.isNoteOn (); });
}

std::vector<TimedMidiEvent> noteOffsOf (const MidiRenderResult& render)
{
    return render.select ([] (const TimedMidiEvent& event) { return event.message.isNoteOff (); });
}

std::vector<TimedMidiEvent> allNotesOffOf (const MidiRenderResult& render)
{
    return render.select ([] (const TimedMidiEvent& event) { return event.message.isControllerOfType (123); });
}

/** Note-ons carrying `velocity` — i.e. "the notes pattern A played" / "…B played". */
std::vector<TimedMidiEvent> notesFromPattern (const MidiRenderResult& render, int velocity)
{
    return render.select ([velocity] (const TimedMidiEvent& event)
                          { return event.message.isNoteOn () && event.message.getVelocity () == velocity; });
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// A. The four quantize modes, against hand-computed absolute-sample literals
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/pattern-switch: each quantize mode lands on its own hand-computed boundary", "[midi-conformance]")
{
    // Precondition on the arithmetic every literal rests on.
    REQUIRE (stepSamples == 6000);
    REQUIRE (beatSamples == 24000);
    REQUIRE (barSamples == 96000);

    std::vector<std::int64_t> observedBoundaries;

    for (const auto& quantize : quantizeCases)
    {
        INFO ("quantize mode: " << quantize.name);

        std::int64_t firstBoundary = -1;
        MidiRenderResult reference;
        std::vector<std::uint8_t> referenceBytes;

        for (const int blockSize : sweptBlockSizes)
        {
            INFO ("block size " << blockSize);

            // Preconditions, asserted rather than assumed: the span is covered by
            // whole blocks and the command lands on a block head at THIS size.
            REQUIRE (sweepSpanSamples % blockSize == 0);
            REQUIRE (commandSample % blockSize == 0);

            auto schedule = startPlaying ();
            schedule.push_back (ScheduledCommand { commandSample, patternSwitchCommand (patternB, quantize.mode) });
            REQUIRE (scheduleIsBlockAligned (schedule, blockSize));

            auto rig = makeSwitchRig (blockSize);
            const auto render = renderSequencer (*rig, spanConfig (sweepSpanSamples, blockSize), schedule);

            REQUIRE (render.numSamples == sweepSpanSamples);
            REQUIRE (render.isSampleSorted ());

            const auto fromA = notesFromPattern (render, velocityA);
            const auto fromB = notesFromPattern (render, velocityB);
            INFO (render.describe (8));

            // ── ANTI-VACUITY ────────────────────────────────────────────────
            // Both patterns really played, every note-on came from one of them,
            // and the command did not happen to sit on its own boundary (in which
            // case the render would prove nothing about quantization at all).
            REQUIRE (! fromA.empty ());
            REQUIRE (! fromB.empty ());
            REQUIRE (fromA.size () + fromB.size () == noteOnsOf (render).size ());
            REQUIRE (commandSample != quantize.boundarySample);

            // ── THE BOUNDARY, AS AN ABSOLUTE-SAMPLE LITERAL ─────────────────
            const std::int64_t lastA = fromA.back ().absoluteSample;
            const std::int64_t firstB = fromB.front ().absoluteSample;

            REQUIRE (firstB == quantize.boundarySample);
            REQUIRE (firstB == quantize.adoptStep * stepSamples);
            REQUIRE (lastA < firstB);
            // The switch is CLEAN, not a fade: nothing from A after the boundary.
            REQUIRE (lastA == quantize.boundarySample - stepSamples);
            REQUIRE (fromB.back ().absoluteSample > firstB); // B kept playing

            // A 50% gate means the table is empty at every step boundary, so the
            // switch flush emits nothing at all. (The flush cases below cover the
            // opposite configuration.)
            REQUIRE (allNotesOffOf (render).empty ());
            REQUIRE (rig->sequencer.activePattern () == patternB);

            NoteLifecycleTracker tracker;
            tracker.observeAll (render);
            INFO (tracker.describe ());
            REQUIRE (tracker.noteOnsSeen () > 0);
            REQUIRE (tracker.balanced ());

            // ── BUFFER-SIZE INDEPENDENCE ────────────────────────────────────
            if (firstBoundary < 0)
            {
                firstBoundary = firstB;
                reference = render;
                referenceBytes = render.toByteStream ();
                REQUIRE (! referenceBytes.empty ());
            }
            else
            {
                REQUIRE (firstB == firstBoundary); // THE criterion, at every size
                INFO (reference.describeDifference (render));
                REQUIRE (render == reference);
                REQUIRE (render.toByteStream () == referenceBytes);
            }
        }

        observedBoundaries.push_back (firstBoundary);
    }

    // ── THE NEGATIVE CONTROL ─────────────────────────────────────────────────
    // Four modes, four DIFFERENT landing points, strictly increasing. Without
    // this, an implementation that ignored `QuantizeMode` entirely could still be
    // made to satisfy every literal above by a test that picked a coincidental
    // command sample.
    REQUIRE (observedBoundaries.size () == 4u);
    REQUIRE (observedBoundaries[0] == 66000);  // instant
    REQUIRE (observedBoundaries[1] == 72000);  // beat
    REQUIRE (observedBoundaries[2] == 96000);  // bar
    REQUIRE (observedBoundaries[3] == 120000); // patternEnd
    REQUIRE (observedBoundaries[0] < observedBoundaries[1]);
    REQUIRE (observedBoundaries[1] < observedBoundaries[2]);
    REQUIRE (observedBoundaries[2] < observedBoundaries[3]);
    REQUIRE (std::adjacent_find (observedBoundaries.begin (), observedBoundaries.end ()) == observedBoundaries.end ());
}

TEST_CASE ("sequencer/pattern-switch: a bar switch asked for ON a bar line fires on THAT bar", "[midi-conformance]")
{
    // THE OFF-BY-ONE-BAR CASE, and the reason `resolvePendingSwitch` uses a snapped
    // CEILING rather than `ppqOfLastBarStart () + quarterNotesPerBar`: at a start
    // position that is exactly a bar line the latter form returns the bar that has
    // just STARTED, adds four quarters, and lands the switch a whole bar late — a
    // full 2 seconds at 120 BPM, which a user experiences as the button not working.
    constexpr int blockSize = 128;
    constexpr std::int64_t barLineSample = barSamples; // PPQ 4.0, the first bar line
    static_assert (barLineSample % blockSize == 0, "the command must land on a block head");

    auto schedule = startPlaying ();
    schedule.push_back (ScheduledCommand { barLineSample, patternSwitchCommand (patternB, QuantizeMode::bar) });

    auto rig = makeSwitchRig (blockSize);
    const auto render = renderSequencer (*rig, spanConfig (3 * barSamples, blockSize), schedule);

    const auto fromA = notesFromPattern (render, velocityA);
    const auto fromB = notesFromPattern (render, velocityB);
    INFO (render.describe (8));

    REQUIRE (! fromA.empty ());
    REQUIRE (! fromB.empty ());

    // On THIS bar (96000), not the next one (192000).
    REQUIRE (fromB.front ().absoluteSample == barLineSample);
    REQUIRE (fromB.front ().absoluteSample != 2 * barSamples);
    REQUIRE (fromA.back ().absoluteSample == barLineSample - stepSamples);
}

TEST_CASE ("sequencer/pattern-switch: switching to the pattern already playing is a total no-op",
           "[midi-conformance][determinism]")
{
    // A switch to the ACTIVE pattern must not flush. Flushing would cut every
    // sounding note mid-gate for a command the user experiences as "nothing should
    // happen" — an audible click for free.
    //
    // The gate is 150% here on purpose: at a 50% gate the table is empty at every
    // step boundary, so even a spurious flush would emit nothing and this case
    // would pass whatever the engine did. At 150% a note is ALWAYS sounding when a
    // boundary arrives, so a spurious flush is loud: note-offs plus a CC123 sweep.
    constexpr int blockSize = 128;
    constexpr int longGate = 150;
    constexpr std::int64_t span = 4 * barSamples;

    auto withSwitch = startPlaying ();
    withSwitch.push_back (ScheduledCommand { commandSample, patternSwitchCommand (patternA, QuantizeMode::bar) });

    auto switchRig = makeSwitchRig (blockSize, longGate);
    const auto switched = renderSequencer (*switchRig, spanConfig (span, blockSize), withSwitch);

    auto quietRig = makeSwitchRig (blockSize, longGate);
    const auto untouched = renderSequencer (*quietRig, spanConfig (span, blockSize), startPlaying ());

    INFO (untouched.describeDifference (switched));

    // Non-vacuity: this configuration really does keep a note sounding across every
    // step boundary, so a flush WOULD have been visible.
    REQUIRE (! untouched.empty ());
    REQUIRE (notesFromPattern (untouched, velocityA).size () > 8u);

    REQUIRE (allNotesOffOf (switched).empty ()); // no flush happened
    REQUIRE (switched == untouched);             // …and nothing else moved either
    REQUIRE (switched.toByteStream () == untouched.toByteStream ());
    REQUIRE (switchRig->sequencer.activePattern () == patternA);
}

// ─────────────────────────────────────────────────────────────────────────────
// A (cont). The request survives a discontinuity; the RESOLUTION does not
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/pattern-switch: a pending switch survives a stop and re-resolves", "[midi-conformance]")
{
    // `handleDiscontinuities` clears `pendingResolved` but KEEPS `pendingRequested`:
    // a stop rewinds to PPQ 0, so a switch resolved for bar 1 would sit forever in
    // the future, but the user's request has not been withdrawn. This pins both
    // halves — it still fires, and it fires exactly ONCE.
    //
    // "Exactly once" is observable because the gate is 150%: a note is sounding at
    // every step boundary, so any second firing would emit a second CC123 sweep.
    // The only sweep this render may contain is the stop's own.
    constexpr int blockSize = 128;
    constexpr int longGate = 150;
    constexpr std::int64_t switchAt = 23808;  // PPQ 0.992 — resolves to bar 1 (96000)
    constexpr std::int64_t stopAt = 48000;    // before that boundary
    constexpr std::int64_t restartAt = 49152; // one block later, still block-aligned
    constexpr std::int64_t span = 262144;     // well past the first post-restart bar

    static_assert (switchAt % blockSize == 0 && stopAt % blockSize == 0 && restartAt % blockSize == 0,
                   "every command must land on a block head");

    std::vector<ScheduledCommand> schedule = startPlaying ();
    schedule.push_back (ScheduledCommand { switchAt, patternSwitchCommand (patternB, QuantizeMode::bar) });
    schedule.push_back (ScheduledCommand { stopAt, engineCommand (EngineCommandType::transportStop) });
    schedule.push_back (ScheduledCommand { restartAt, engineCommand (EngineCommandType::transportPlay) });

    auto rig = makeSwitchRig (blockSize, longGate);
    const auto render = renderSequencer (*rig, spanConfig (span, blockSize), schedule);

    const auto fromA = notesFromPattern (render, velocityA);
    const auto fromB = notesFromPattern (render, velocityB);
    INFO (render.describe (16));

    // The switch had NOT fired before the stop (bar 1 is at 96000, the stop at
    // 48000) — so every note in the first leg is pattern A's.
    REQUIRE (! fromA.empty ());
    REQUIRE (fromA.back ().absoluteSample < stopAt);

    // …and it DID fire after the restart, on the re-resolved timeline. The stop
    // rewound to PPQ 0, so the next bar boundary at or after the restart's
    // block-start PPQ is PPQ 0 itself: the very first step of the new leg.
    REQUIRE (! fromB.empty ());
    REQUIRE (fromB.front ().absoluteSample == restartAt);
    REQUIRE (rig->sequencer.activePattern () == patternB);

    // FIRED EXACTLY ONCE: the stop's sweep is the only flush in the render. A
    // request left pending would re-resolve to the next bar of the new leg and
    // flush there, producing a second sweep.
    const auto sweeps = allNotesOffOf (render);
    INFO ("sweeps at: " << (sweeps.empty () ? juce::String ("<none>") : sweeps.front ().describe ()));
    REQUIRE (sweeps.size () == 1u);
    REQUIRE (sweeps[0].absoluteSample == stopAt);

    NoteLifecycleTracker tracker;
    tracker.observeAll (render);
    INFO (tracker.describe ());
    REQUIRE (tracker.noteOnsSeen () > 0);
    REQUIRE (tracker.orphanNoteOffs () == 0);
}

TEST_CASE ("sequencer/pattern-switch: a pending switch survives a locate and re-resolves", "[midi-conformance]")
{
    // Same contract on the other discontinuity, and the harder one: a locate keeps
    // the transport PLAYING, so the re-resolution has to happen against a timeline
    // that jumped mid-flight rather than one that restarted from zero.
    //
    // Locate target PPQ 9.0 (exactly representable, and deliberately NOT a bar
    // line): the next bar boundary is PPQ 12.0, three quarters = 72000 samples
    // later, so the switch lands at renderSample 48000 + 72000 = 120000.
    constexpr int blockSize = 128;
    constexpr int longGate = 150;
    constexpr std::int64_t switchAt = 23808; // PPQ 0.992 — resolves to bar 1 (96000)
    constexpr std::int64_t locateAt = 48000;
    constexpr double locateTargetPpq = 9.0;
    constexpr std::int64_t expectedBoundary = locateAt + 3 * beatSamples; // 120000
    constexpr std::int64_t span = 262144;

    static_assert (switchAt % blockSize == 0 && locateAt % blockSize == 0, "commands land on block heads");

    std::vector<ScheduledCommand> schedule = startPlaying ();
    schedule.push_back (ScheduledCommand { switchAt, patternSwitchCommand (patternB, QuantizeMode::bar) });
    schedule.push_back (
        ScheduledCommand { locateAt, engineCommand (EngineCommandType::transportLocate, locateTargetPpq) });

    auto rig = makeSwitchRig (blockSize, longGate);
    const auto render = renderSequencer (*rig, spanConfig (span, blockSize), schedule);

    const auto fromA = notesFromPattern (render, velocityA);
    const auto fromB = notesFromPattern (render, velocityB);
    INFO (render.describe (16));

    REQUIRE (! fromA.empty ());
    REQUIRE (! fromB.empty ());
    REQUIRE (rig->transport.isPlaying ()); // a locate never stops the transport

    // The pre-locate resolution (bar 1 of the ORIGINAL timeline, sample 96000) was
    // discarded; the request re-resolved against PPQ 9.0.
    REQUIRE (fromB.front ().absoluteSample == expectedBoundary);
    REQUIRE (fromB.front ().absoluteSample != barSamples);
    REQUIRE (fromA.back ().absoluteSample < expectedBoundary);
    REQUIRE (rig->sequencer.activePattern () == patternB);

    // Two flushes exactly: the locate's, and the switch's. A third would mean the
    // request fired twice.
    const auto sweeps = allNotesOffOf (render);
    REQUIRE (sweeps.size () == 2u);
    REQUIRE (sweeps[0].absoluteSample == locateAt);
    REQUIRE (sweeps[1].absoluteSample < expectedBoundary);
    REQUIRE (sweeps[1].absoluteSample >= expectedBoundary - 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// F. Flush points (§5.5) — the pattern switch as a note-lifecycle event
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/flush: a pattern switch empties the sounding-note table", "[unit][midi-conformance]")
{
    // §5.5 lists "pattern switch" as a flush point, and the invariant is not "some
    // note-offs were emitted" but "a PER-NOTE off for every sounding note, and the
    // table is empty afterwards". `NoteLifecycleTracker` counts CC123 without ever
    // applying it, precisely so a flush that emitted ONLY the sweep still reads as
    // unbalanced — which is the bug worth catching, since hosted plugins honour
    // CC123 inconsistently.
    //
    // A 150% gate is what makes a note sounding at the boundary in the first place.
    constexpr int blockSize = 128;
    constexpr int longGate = 150;

    // The command lands at 23808 (block 186); an `instant` switch resolves to the
    // next step boundary, PPQ 1.0 = sample 24000, which sits at offset 64 of block
    // 187. Splitting the render at block 187 lets the pre-switch table be READ.
    constexpr std::int64_t switchAt = 23808;
    constexpr std::int64_t boundarySample = 4 * stepSamples; // 24000
    constexpr int blocksBeforeFire = 187;                    // [0, 23936)
    static_assert (switchAt % blockSize == 0, "the command must land on a block head");
    static_assert (blocksBeforeFire * blockSize == 23936, "the fire block starts here");

    SECTION ("the table is empty when the adopt step is a rest in the incoming pattern")
    {
        // Pattern B's GATE is punched off at step 4 so nothing is re-triggered at
        // the boundary — which makes "the table is EMPTY" directly assertable
        // rather than inferred. It also pins a documented behaviour worth having a
        // test for: THE SWITCH FIRES ON THE INDEX, NOT ON THE EMISSION. A pattern
        // whose adopt step happens to be a rest must still switch.
        auto rig = makeSwitchRig (blockSize, longGate);
        rig->patternDocument.setLaneValue (patternB, LaneId::gate, 4, 0);

        auto schedule = startPlaying ();
        schedule.push_back (ScheduledCommand { switchAt, patternSwitchCommand (patternB, QuantizeMode::instant) });

        const auto beforeFire = renderSequencer (*rig, spanConfig (blocksBeforeFire * blockSize, blockSize), schedule);

        // Exactly one note is mid-gate as the fire block begins: step 3 (sample
        // 18000) is held until 18000 + 1.5*6000 = 27000.
        const int soundingBefore = rig->sequencer.soundingNotes ().size ();
        INFO (beforeFire.describe (8));
        REQUIRE (soundingBefore == 1);
        REQUIRE (rig->sequencer.activePattern () == patternA);

        const auto fireBlock = renderSequencer (*rig, spanConfig (blockSize, blockSize));

        INFO (fireBlock.describe ());
        REQUIRE (rig->sequencer.activePattern () == patternB);

        // THE §5.5 assertion.
        REQUIRE (rig->sequencer.soundingNotes ().isEmpty ());

        // A per-note off for every note that was sounding, plus the belt-and-braces
        // sweep — and NOT the sweep on its own.
        const auto offs = noteOffsOf (fireBlock);
        const auto sweeps = allNotesOffOf (fireBlock);
        REQUIRE (static_cast<int> (offs.size ()) == soundingBefore);
        REQUIRE (sweeps.size () == 1u);
        REQUIRE (noteOnsOf (fireBlock).empty ()); // step 4 is a rest in pattern B

        // Both land one sample before the adopt point (the same 1-sample-gap
        // discipline as the same-pitch retrigger), relative to this render's origin
        // of 23936.
        const std::int64_t flushAt = boundarySample - 1 - blocksBeforeFire * blockSize;
        REQUIRE (offs[0].absoluteSample == flushAt);
        REQUIRE (sweeps[0].absoluteSample == flushAt);

        NoteLifecycleTracker tracker;
        tracker.observeAll (beforeFire);
        tracker.observeAll (fireBlock);
        INFO (tracker.describe ());
        REQUIRE (tracker.noteOnsSeen () > 0);
        REQUIRE (tracker.balanced ());
    }

    SECTION ("the incoming pattern's own note lands exactly on the boundary")
    {
        auto rig = makeSwitchRig (blockSize, longGate);

        auto schedule = startPlaying ();
        schedule.push_back (ScheduledCommand { switchAt, patternSwitchCommand (patternB, QuantizeMode::instant) });

        const auto render = renderSequencer (*rig, spanConfig (8 * stepSamples, blockSize), schedule);
        INFO (render.describe (16));

        const auto fromB = notesFromPattern (render, velocityB);
        REQUIRE (! fromB.empty ());
        REQUIRE (fromB.front ().absoluteSample == boundarySample);

        // The outgoing note is released BEFORE the incoming note-on, never at the
        // same instant on the same pitch.
        const auto sweeps = allNotesOffOf (render);
        REQUIRE (sweeps.size () == 1u);
        REQUIRE (sweeps[0].absoluteSample == boundarySample - 1);
    }
}

TEST_CASE ("sequencer/flush: stop, locate and pattern switch in one scripted render stay balanced",
           "[unit][midi-conformance]")
{
    // All three of Phase 6's reachable flush points in a single timeline, at a gate
    // long enough that each one has real work to do. The script ENDS on a stop,
    // which is the point at which §5.5's balance must close.
    constexpr int blockSize = 128;
    constexpr int longGate = 150;

    std::vector<ScheduledCommand> script = startPlaying ();
    script.push_back (ScheduledCommand { 12800, patternSwitchCommand (patternB, QuantizeMode::beat) });
    script.push_back (ScheduledCommand { 25600, engineCommand (EngineCommandType::transportLocate, 5.5) });
    script.push_back (ScheduledCommand { 38400, patternSwitchCommand (patternA, QuantizeMode::instant) });
    script.push_back (ScheduledCommand { 51200, engineCommand (EngineCommandType::transportStop) });
    script.push_back (ScheduledCommand { 52224, engineCommand (EngineCommandType::transportPlay) });
    script.push_back (ScheduledCommand { 64000, patternSwitchCommand (patternB, QuantizeMode::bar) });
    script.push_back (ScheduledCommand { 179200, engineCommand (EngineCommandType::setTempoBpm, 240.0) });
    script.push_back (ScheduledCommand { 230400, engineCommand (EngineCommandType::transportStop) });

    for (const auto& entry : script)
        REQUIRE (entry.atSample % blockSize == 0);

    auto rig = makeSwitchRig (blockSize, longGate);
    const auto render = renderSequencer (*rig, spanConfig (256000, blockSize), script);

    INFO (render.summary ());
    REQUIRE (render.isSampleSorted ());

    NoteLifecycleTracker tracker;
    tracker.observeAll (render);
    INFO (tracker.describe ());

    // Non-vacuity: the script really played, and both patterns were heard.
    REQUIRE (tracker.noteOnsSeen () > 20);
    REQUIRE (! notesFromPattern (render, velocityA).empty ());
    REQUIRE (! notesFromPattern (render, velocityB).empty ());
    REQUIRE (tracker.allNotesOffSeen () >= 3); // at least stop + locate + a switch

    // THE invariant, at the flush point the script ends on.
    REQUIRE (tracker.orphanNoteOffs () == 0);
    REQUIRE (tracker.outstanding () == 0);
    REQUIRE (tracker.balanced ());
    REQUIRE (rig->sequencer.soundingNotes ().isEmpty ());
    REQUIRE (rig->sequencer.soundingNotes ().droppedNoteOnCount () == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// A (cont). Where the switch's flush LANDS, across block sizes
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/pattern-switch: the switch boundary is buffer-size independent even when it flushes",
           "[midi-conformance]")
{
    // The mode sweep at the top of this file can assert BYTE-IDENTICAL streams
    // across block sizes only because its 50% gate leaves the sounding-note table
    // empty at every step boundary, so the switch flush emits nothing. This case is
    // the same switch with a 150% gate, where the flush really fires — and it
    // deliberately asserts LESS.
    //
    // ── A DEFECT THIS CASE ONCE PINNED WITHOUT FREEZING — NOW FIXED (#46/#48) ──
    // WHY THE ASSERTIONS BELOW ARE LOOSER THAN THE ENGINE NOW GUARANTEES. When this
    // case was written, `flushForPatternSwitch` placed the flush at a WITHIN-BLOCK
    // `jmax (0, offset - 1)`. That is one sample before the adopt point — so the
    // outgoing note is released before the incoming note-on — except when the adopt
    // point landed at block OFFSET 0, where the `jmax` collapsed the gap onto the
    // boundary. Whether it collapsed was decided by the DEVICE BUFFER SIZE, making
    // the emitted MIDI buffer-size dependent by one sample: the same family as #36,
    // and a §1.2 violation that had to be settled before any golden containing a
    // pattern switch with overlapping gates was baked.
    //
    // #46 and #48 closed it structurally. The flush point is now decided on the
    // ABSOLUTE timeline and converted once inside `SoundingNoteTable::flush`, and
    // `processBlock` PRE-FLUSHES from the previous block when `adoptSample - 1` lies
    // there — so there is no offset-0 collapse left to hit (see the residual-case note
    // on `SequencerProcessor::flushForPatternSwitch`, which a `bar`-quantized switch
    // like this one cannot reach). The window is closed: at the time of writing all
    // ten swept sizes report the sweep one sample early, and none on the boundary.
    //
    // The assertions were written to stay green on BOTH sides of that fix, and they
    // have deliberately not been tightened here — they still assert only the parts
    // that were invariant even while the defect stood (the adopt point, the event
    // count, the flush count, and that the flush never lands AFTER the boundary),
    // bounding the sweep to a one-sample window rather than pinning it. Pinning it to
    // exactly `expectedBoundary - 1` is a real strengthening now available, and is
    // left as a separate change so it can be verified fails-without on its own.
    constexpr int longGate = 150;
    constexpr std::int64_t expectedBoundary = 16 * stepSamples; // `bar` from PPQ 2.56

    std::int64_t referenceBoundary = -1;
    std::size_t referenceEventCount = 0;
    int sweepsOnBoundary = 0;
    int sweepsOneSampleEarly = 0;

    for (const int blockSize : sweptBlockSizes)
    {
        INFO ("block size " << blockSize);
        REQUIRE (sweepSpanSamples % blockSize == 0);
        REQUIRE (commandSample % blockSize == 0);

        auto schedule = startPlaying ();
        schedule.push_back (ScheduledCommand { commandSample, patternSwitchCommand (patternB, QuantizeMode::bar) });

        auto rig = makeSwitchRig (blockSize, longGate);
        const auto render = renderSequencer (*rig, spanConfig (sweepSpanSamples, blockSize), schedule);

        const auto fromA = notesFromPattern (render, velocityA);
        const auto fromB = notesFromPattern (render, velocityB);
        const auto sweeps = allNotesOffOf (render);
        INFO (render.describe (8));

        // Non-vacuity: the long gate really did leave a note sounding at the
        // boundary, so the flush really did fire.
        REQUIRE (! fromA.empty ());
        REQUIRE (! fromB.empty ());
        REQUIRE (sweeps.size () == 1u);
        REQUIRE (render.isSampleSorted ());

        // THE invariant: the adopt point is the same absolute sample everywhere.
        REQUIRE (fromB.front ().absoluteSample == expectedBoundary);
        REQUIRE (fromA.back ().absoluteSample < expectedBoundary);

        // The flush is never LATE — releasing the outgoing note after the incoming
        // note-on would be an audible defect rather than a one-sample blemish.
        REQUIRE (sweeps[0].absoluteSample <= expectedBoundary);
        REQUIRE (sweeps[0].absoluteSample >= expectedBoundary - 1);

        if (sweeps[0].absoluteSample == expectedBoundary)
            ++sweepsOnBoundary;
        else
            ++sweepsOneSampleEarly;

        if (referenceBoundary < 0)
        {
            referenceBoundary = fromB.front ().absoluteSample;
            referenceEventCount = render.size ();
        }
        else
        {
            REQUIRE (fromB.front ().absoluteSample == referenceBoundary);
            // The event COUNT is invariant even though one event's position is not,
            // which localises the defect to placement rather than to content.
            REQUIRE (render.size () == referenceEventCount);
        }
    }

    // Recorded so the reader can see WHERE the window sits. While #46/#48 stood the
    // sweep landed in both places across the swept sizes; since the fix it is 0 on the
    // boundary and 10 one sample early. This total is the non-vacuity check only —
    // the placement itself is asserted per-size above.
    INFO ("flush on the boundary: " << sweepsOnBoundary << ", one sample early: " << sweepsOneSampleEarly);
    REQUIRE (sweepsOnBoundary + sweepsOneSampleEarly == 10);
}
