// ─────────────────────────────────────────────────────────────────────────────
// determinism_goldens — THE SIX BAKED GOLDEN MIDI EVENT STREAMS
// (ARCHITECTURE §1.2 "same (pattern, seeds, N bars) ⇒ byte-identical MIDI,
// forever … enforced by a golden-MIDI test suite and is a release gate";
// INSTRUCTIONS Phase 6.4 "First golden-MIDI files: representative patterns
// (polymetric, tied, each direction mode) rendered to tests/golden/").
//
// Machinery: support/GoldenMidiFile.h (serialize / parse / compare / the
// write-and-still-fail regeneration interlock) over support/MidiRenderHarness.h
// and support/SequencerRenderRig.h. Governance: tests/golden/README.md, Rule
// zero — A GOLDEN DIFF IS A FINDING, NEVER SOMETHING TO SILENTLY REGENERATE.
//
// ── THE CANONICAL CLOCK, AND WHY THE NUMBERS ARE ROUND ──────────────────────
// 125 BPM @ 48 kHz on the 1/16 grid: one step is EXACTLY 5760 samples, a beat
// 23040 and a 4/4 bar 92160. Every absolute position in five of the six files is
// therefore an exact multiple of 5760, which is what makes a `git diff` of a
// golden REVIEWABLE: a reader who sees `17280 90 41 64` can divide by 5760 and
// know instantly that it is step 3. A golden nobody can read by eye is a golden
// nobody will ever audit, and Rule zero depends on the audit being possible.
//
// ── EVERY GOLDEN IS COMPARED AT EVERY SWEPT BLOCK SIZE ──────────────────────
// Not just at the size it was baked at. Buffer-size independence (Phase 5.3) and
// the golden reference (Phase 6.4) are two halves of one contract, and checking
// them together makes six files do the work of six sweeps. `bakedAtBlockSize` is
// recorded in each header for triage and is NEVER compared — see the note in
// GoldenMidiFile.h.
//
// THE ALIGNMENT UNIT IS 61440 = lcm {32, 64, 96, 128, 256, 480, 512, 1024, 2048,
// 4096}: the only samples that are block heads at every swept size. A command
// scheduled off a block head is consumed at a different absolute sample per block
// size, and a span that is not a whole number of blocks covers a different amount
// of musical time per block size — either would fail the sweep for a reason that
// has nothing to do with the sequencer. Both are asserted as PRECONDITIONS below
// rather than assumed.
//
// THESE CONSTANTS ARE DEFINED LOCALLY AND DELIBERATELY NOT SHARED WITH
// transport_timing.cpp. That file's proven configurations predate 4096 being in
// the sweep: its config A (60 BPM / 48 kHz / 4 bars = 768000 samples) is
// 12.5 x 61440 and does NOT align once 4096 is included. Importing its constants
// here would silently reintroduce that hazard; duplicating ten integers does not.
//
// ── WHY `walk` AND `randomNoRepeat` GET NO GOLDEN IN PHASE 6 ────────────────
// DELIBERATE OMISSION, not an oversight. Both seed their traversal tables from
// `splitmix64` over the pattern's `masterSeed`, and Phase 7.1 introduces the
// versioned `RngStream` with `rngVersion` stamped into the schema (§5.2 "RNG
// streams are versioned … any change that alters output for existing seeds
// requires a version bump + migration note + justified golden update"). Baking
// them now would guarantee a mandatory regeneration a phase later — which is
// exactly the "regenerate to make it green" habit Rule zero exists to make
// expensive. They are covered instead by the property tests in
// pattern_directions.cpp (period, no-repeat-across-the-loop-point, ±1 steps,
// reflection at the pool ends), which pin the PROPERTIES without freezing bytes
// that are about to be re-versioned. `direction-modes-cycle` therefore covers the
// NINE deterministic §12.3 modes, and that count is asserted out loud.
//
// ── WHAT EACH GOLDEN CASE ASSERTS, AND WHY EACH GUARD IS THERE ──────────────
//   1. `render.numSamples == <literal>` — `MidiRenderResult::operator==` compares
//      the EVENT STREAM ONLY and ignores span length, so two renders sharing an
//      event prefix but covering different spans compare EQUAL. Without this the
//      span could silently halve.
//   2. A literal absolute-sample + raw-byte assertion WRITTEN IN THIS SOURCE, so
//      a wholesale corruption of the file and a matching corruption of the
//      renderer cannot silently agree with each other.
//   3. A minimum event count as a literal — anti-vacuity. Two empty streams
//      compare equal.
//   4. THE PERTURBED-RENDER NEGATIVE CONTROL: one lane value changed, and the
//      resulting render must NOT match the golden. This is the guard that a
//      `compareToGolden` which always returned true would fail; without it all
//      six cases would be green against a broken comparator.
//   5. The whole-block-size sweep, aggregated and asserted AFTER the loop (no
//      Catch2 macro runs inside a sweep).
// Plus, once for the directory, a two-way inventory check: every expected file
// exists AND no unreferenced file is lying around.
// ─────────────────────────────────────────────────────────────────────────────

#include "support/GoldenMidiFile.h"
#include "support/MidiRenderHarness.h"
#include "support/NoteLifecycleCheck.h"
#include "support/SequencerRenderRig.h"

#include "engine/generative/Rng.h"
#include "engine/graph/EngineCommand.h"
#include "engine/midi/NotePool.h"
#include "engine/sequencer/PatternDocument.h"
#include "engine/sequencer/PatternTypes.h"
#include "engine/sequencer/StepLogic.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using arpbox::engine::DirectionMode;
using arpbox::engine::EngineCommandType;
using arpbox::engine::LaneId;
using arpbox::engine::maxSteps;
using arpbox::engine::PatternDocument;
using arpbox::engine::QuantizeMode;
using arpbox::testing::checkGolden;
using arpbox::testing::compareToGolden;
using arpbox::testing::engineCommand;
using arpbox::testing::GoldenFile;
using arpbox::testing::headerFor;
using arpbox::testing::listGoldenFiles;
using arpbox::testing::loadGolden;
using arpbox::testing::MidiRenderConfig;
using arpbox::testing::MidiRenderResult;
using arpbox::testing::NoteLifecycleTracker;
using arpbox::testing::patternSwitchCommand;
using arpbox::testing::renderSequencer;
using arpbox::testing::ScheduledCommand;
using arpbox::testing::SequencerRig;
using arpbox::testing::TimedMidiEvent;

using arpbox::engine::TrigCondition;

namespace
{
// ─────────────────────────────────────────────────────────────────────────────
// The canonical clock and the sweep
// ─────────────────────────────────────────────────────────────────────────────

constexpr double goldenSampleRate = 48000.0;
constexpr double goldenBpm = 125.0;
constexpr double goldenGridPpq = 0.25; ///< 1/16 — the document default.

/** 1/16 at 125 BPM / 48 kHz. EXACT, and that exactness is the point. */
constexpr std::int64_t stepSamples = 5760;
constexpr std::int64_t beatSamples = 4 * stepSamples; ///< 23040
constexpr std::int64_t barSamples = 16 * stepSamples; ///< 92160, 4/4

/** transport_timing.cpp's proven-DISCRIMINATING fractional configuration: at
    137 BPM / 44.1 kHz a 1/16 is 4828.467… samples, so nothing lands on a round
    number and no accidental alignment can mask a difference. It is spent on the
    LEN golden because LEN's arithmetic — `llround (gateFraction * samplesPerStep)`
    — is the rate-sensitive part, so the tie/retrigger file gets the hostile clock
    for free instead of costing a seventh file. */
constexpr double fractionalSampleRate = 44100.0;
constexpr double fractionalBpm = 137.0;

/** The ten swept block sizes: the eight powers of two plus 96 and 480, the
    non-power-of-two buffers real CoreAudio devices hand us. */
constexpr int goldenBlockSizes[] = { 32, 64, 96, 128, 256, 480, 512, 1024, 2048, 4096 };
constexpr int numGoldenBlockSizes = static_cast<int> (std::size (goldenBlockSizes));

/** lcm of every entry in `goldenBlockSizes` = 2^12 * 3 * 5. Every span and every
    scheduled command in this file is a multiple of it. */
constexpr std::int64_t goldenAlignmentUnit = 61440;

/** The size each golden's header records in `bakedAtBlockSize` (non-normative). */
constexpr int bakeBlockSize = 128;

// ── Spans: MUSICAL span, then the render span that contains its flush ───────
//
// EVERY GOLDEN ENDS ON A REAL §5.5 FLUSH POINT. Each scenario plays for its
// MUSICAL span and is then stopped by a `transportStop` scheduled at exactly that
// sample; the render continues for one further `goldenAlignmentUnit` so the flush
// and the silence after it are inside the captured stream.
//
// WHY THAT TAIL EXISTS, AND WHY IT IS NOT "PADDING THE SPAN". A note still
// sounding when a render simply RUNS OUT leaves an unbalanced note-on in the
// golden — the stream would freeze a truncation artifact rather than a
// performance, and `NoteLifecycleTracker::balanced()` could never hold. The fix
// is a flush point, not a longer ring-out window: lengthening the span until the
// notes happened to finish would leave the end of the file arbitrary again the
// moment a LEN value changed. The stop is the mechanism; the tail exists only
// because a command scheduled AT the render's last sample is never drained (the
// final block covers `[span - blockSize, span)`), so there has to be at least one
// block after the stop for its flush to land in.
//
// The musical content of every scenario is therefore UNCHANGED — the stop sits
// exactly where each render used to end.
//
// All eight values are exact multiples of `goldenAlignmentUnit`, asserted below.
constexpr std::int64_t fourBarMusic = 4 * barSamples;              ///< 368640 = 6 x 61440, 64 steps
constexpr std::int64_t eightBarMusic = 8 * barSamples;             ///< 737280 = 12 x 61440, 128 steps
constexpr std::int64_t tenBarMusic = 10 * barSamples;              ///< 921600 = 15 x 61440, 160 steps
constexpr std::int64_t fractionalMusic = 10 * goldenAlignmentUnit; ///< 614400

constexpr std::int64_t fourBarSpan = fourBarMusic + goldenAlignmentUnit;       ///< 430080 = 7 x 61440
constexpr std::int64_t eightBarSpan = eightBarMusic + goldenAlignmentUnit;     ///< 798720 = 13 x 61440
constexpr std::int64_t tenBarSpan = tenBarMusic + goldenAlignmentUnit;         ///< 983040 = 16 x 61440
constexpr std::int64_t fractionalSpan = fractionalMusic + goldenAlignmentUnit; ///< 675840 = 11 x 61440

/** The stub pool every scenario plays over: C major, one octave (PatternDocument's
    documented default). Repeated here so the expectations below are derived from a
    value written in the TEST, not read out of the engine. */
constexpr int poolPitches[] = { 60, 62, 64, 65, 67, 69, 71, 72 };
constexpr int poolSize = static_cast<int> (std::size (poolPitches));

// ─────────────────────────────────────────────────────────────────────────────
// Small helpers
// ─────────────────────────────────────────────────────────────────────────────

using ConfigureFn = void (*) (PatternDocument&);

/** True when `span` is a whole number of blocks at EVERY swept size — the
    precondition the cross-size comparison rests on. */
bool spanCoversWholeBlocks (std::int64_t span) noexcept
{
    for (const int blockSize : goldenBlockSizes)
        if (span % static_cast<std::int64_t> (blockSize) != 0)
            return false;

    return true;
}

/** True when every scheduled command sits on a block head at EVERY swept size. */
bool scheduleAlignsEverywhere (const std::vector<ScheduledCommand>& schedule) noexcept
{
    for (const int blockSize : goldenBlockSizes)
        if (! arpbox::testing::scheduleIsBlockAligned (schedule, blockSize))
            return false;

    return true;
}

/** `true` if `event` is exactly this absolute sample carrying exactly these three
    MIDI bytes. Used for the per-golden literal assertion. */
bool eventIs (const TimedMidiEvent& event, std::int64_t sample, int status, int data1, int data2) noexcept
{
    return event.absoluteSample == sample && event.numBytes () == 3 && event.bytes ()[0] == status &&
           event.bytes ()[1] == data1 && event.bytes ()[2] == data2;
}

/** `true` if the render contains that exact event anywhere. */
bool containsEvent (const MidiRenderResult& render, std::int64_t sample, int status, int data1, int data2) noexcept
{
    for (const auto& event : render.events)
        if (eventIs (event, sample, status, data1, data2))
            return true;

    return false;
}

/** Note-on pitches in emission order, over `[from, to)`. */
std::vector<int> pitchesIn (const MidiRenderResult& render, std::int64_t from, std::int64_t to)
{
    std::vector<int> pitches;
    for (const auto& event : render.events)
        if (event.message.isNoteOn () && event.absoluteSample >= from && event.absoluteSample < to)
            pitches.push_back (event.message.getNoteNumber ());

    return pitches;
}

/** Tempo + play at sample 0. The tempo command is not optional even when the value
    equals `Transport::defaultBpm`: every literal here is derived from it, and
    saying so in the schedule is what keeps the derivation checkable. */
std::vector<ScheduledCommand> startPlaying (double bpm)
{
    return { ScheduledCommand { 0, engineCommand (EngineCommandType::setTempoBpm, bpm) },
             ScheduledCommand { 0, engineCommand (EngineCommandType::transportPlay) } };
}

/** Play from sample 0, then STOP at `musicEnd` — the §5.5 flush point every golden
    in this file ends on.

    `SequencerProcessor` emits the stop flush at block offset 0 of the block that
    observes the stop, so the flush note-offs land at that block's HEAD. `musicEnd`
    is therefore required to be a multiple of `goldenAlignmentUnit`, which makes the
    head equal `musicEnd` itself at all ten swept block sizes — without that the
    flush would sit at a different absolute sample per buffer size and the
    cross-size comparison would fail for a reason that has nothing to do with the
    pattern. `scheduleAlignsEverywhere` checks it for every schedule here. */
std::vector<ScheduledCommand> playThenStop (double bpm, std::int64_t musicEnd)
{
    auto schedule = startPlaying (bpm);
    schedule.push_back (ScheduledCommand { musicEnd, engineCommand (EngineCommandType::transportStop) });
    return schedule;
}

MidiRenderConfig renderConfig (double sampleRate, std::int64_t span, int blockSize)
{
    auto config = MidiRenderConfig::samples (span, sampleRate, blockSize);
    config.numChannels = 1;
    config.eventReserve = 16384;
    return config;
}

/** One complete render from a FRESH rig — the only way a golden is ever produced
    or checked here, so no scenario can observe another's leftover state. */
MidiRenderResult renderScenario (ConfigureFn configure,
                                 const std::vector<ScheduledCommand>& schedule,
                                 double sampleRate,
                                 std::int64_t span,
                                 int blockSize)
{
    SequencerRig rig { sampleRate, blockSize };
    configure (rig.patternDocument);
    return renderSequencer (rig, renderConfig (sampleRate, span, blockSize), schedule);
}

/** Aggregated outcome of comparing one scenario against one golden at all ten
    block sizes. NO CATCH2 MACRO RUNS INSIDE THE SWEEP — everything is collected
    into this struct and asserted once, afterwards. */
struct SweepOutcome
{
    int sizesChecked = 0;
    int sizesMatched = 0;
    std::int64_t minEvents = 0;
    std::int64_t maxEvents = 0;
    bool spansCorrect = true;
    bool allSorted = true;
    bool allLifecyclesBalanced = true;
    /** Fewest note-ons any swept size produced. GATES `allLifecyclesBalanced`: an
        EMPTY stream is trivially balanced, so the balance claim means nothing until
        this is shown to be positive. */
    std::int64_t minNoteOns = 0;
    juce::String report;
};

SweepOutcome sweepAgainstGolden (const GoldenFile& golden,
                                 ConfigureFn configure,
                                 const std::vector<ScheduledCommand>& schedule,
                                 double sampleRate,
                                 std::int64_t span)
{
    SweepOutcome outcome;
    outcome.report << "golden '" << golden.header.name << "' across " << juce::String (numGoldenBlockSizes)
                   << " block sizes:\n";

    for (const int blockSize : goldenBlockSizes)
    {
        const auto render = renderScenario (configure, schedule, sampleRate, span, blockSize);
        const auto comparison = compareToGolden (render, golden);

        ++outcome.sizesChecked;
        if (comparison.matches)
            ++outcome.sizesMatched;

        const auto count = static_cast<std::int64_t> (render.events.size ());
        if (outcome.sizesChecked == 1)
            outcome.minEvents = outcome.maxEvents = count;
        else
        {
            outcome.minEvents = std::min (outcome.minEvents, count);
            outcome.maxEvents = std::max (outcome.maxEvents, count);
        }

        if (render.numSamples != span)
            outcome.spansCorrect = false;

        if (! render.isSampleSorted ())
            outcome.allSorted = false;

        NoteLifecycleTracker tracker;
        tracker.observeAll (render);
        if (! tracker.balanced ())
            outcome.allLifecyclesBalanced = false;

        const auto noteOns = static_cast<std::int64_t> (tracker.noteOnsSeen ());
        outcome.minNoteOns = outcome.sizesChecked == 1 ? noteOns : std::min (outcome.minNoteOns, noteOns);

        outcome.report << "  block " << juce::String (blockSize) << ": " << juce::String (count) << " events, "
                       << (comparison.matches ? "MATCHES" : "MISMATCH") << "\n";

        if (! comparison.matches)
            outcome.report << comparison.report << "\n";
    }

    return outcome;
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. baseline-4bar — the canary
// ─────────────────────────────────────────────────────────────────────────────
// GATE all on, direction `up` over the ascending stub pool, LEN 50%, no
// polymeter, nothing scheduled but play. This is PatternDocument's documented
// default verbatim, which makes it the file that reddens FIRST for any change to
// the defaults, the step walk, the pool, or the note-off scheduling — before any
// of the five specialised files have to be interpreted.

void configureBaseline (PatternDocument&) {}

void configureBaselinePerturbed (PatternDocument& document)
{
    // ONE lane value. The negative control: this render must NOT match the golden.
    document.setLaneValue (0, LaneId::vel, 5, 101);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. polymeter-len-3-5-7 — per-lane LENGTH
// ─────────────────────────────────────────────────────────────────────────────
// GATE length 5, PITCH length 3, VEL length 7, all at division 1. Pairwise
// coprime, so the three lanes realign only every lcm (5, 3, 7) = 105 steps — and
// the 128-step (8 bar) span covers the full cycle plus 23 steps of the next, so
// the file proves the phase relationship RESTARTS correctly as well as holding.

constexpr int gate5[] = { 1, 1, 0, 1, 1 };
constexpr int pitch3[] = { 0, 2, -1 };
constexpr int vel7[] = { 40, 55, 70, 85, 100, 115, 127 };

void configureLengthPolymeter (PatternDocument& document)
{
    document.beginTransaction ();

    document.setLaneLength (0, LaneId::gate, 5);
    document.setLaneLength (0, LaneId::pitch, 3);
    document.setLaneLength (0, LaneId::vel, 7);

    for (int s = 0; s < 5; ++s)
        document.setLaneValue (0, LaneId::gate, s, gate5[s]);
    for (int s = 0; s < 3; ++s)
        document.setLaneValue (0, LaneId::pitch, s, pitch3[s]);
    for (int s = 0; s < 7; ++s)
        document.setLaneValue (0, LaneId::vel, s, vel7[s]);

    document.endTransaction ();
}

void configureLengthPolymeterPerturbed (PatternDocument& document)
{
    configureLengthPolymeter (document);
    document.setLaneValue (0, LaneId::pitch, 2, -2); // was -1
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. polymeter-clockdiv — per-lane CLOCK DIVISION
// ─────────────────────────────────────────────────────────────────────────────
// The OTHER half of polymeter, and the half with the asymmetry: GATE's division
// is a true divider on the TRIGGER RATE while every other lane's is VALUE-HOLD
// (see `isLaneTick` in PatternTypes.h). GATE stays at /1 length 16 so the trigger
// grid is unambiguous and the file is about the held lanes: PITCH /3 length 4 and
// OCT /2 length 3, which phase against each other with period lcm (12, 6) = 12
// steps against a 16-step gate loop.

constexpr int pitch4[] = { 0, 3, -2, 5 };
constexpr int oct3[] = { 0, 1, -1 };

void configureDivisionPolymeter (PatternDocument& document)
{
    document.beginTransaction ();

    document.setLaneLength (0, LaneId::pitch, 4);
    document.setLaneDivision (0, LaneId::pitch, 3);
    document.setLaneLength (0, LaneId::oct, 3);
    document.setLaneDivision (0, LaneId::oct, 2);

    for (int s = 0; s < 4; ++s)
        document.setLaneValue (0, LaneId::pitch, s, pitch4[s]);
    for (int s = 0; s < 3; ++s)
        document.setLaneValue (0, LaneId::oct, s, oct3[s]);

    document.endTransaction ();
}

void configureDivisionPolymeterPerturbed (PatternDocument& document)
{
    configureDivisionPolymeter (document);
    document.setLaneValue (0, LaneId::oct, 2, -2); // was -1
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. tied-retrigger — LEN > 100% tie/legato and the same-pitch retrigger
// ─────────────────────────────────────────────────────────────────────────────
// ISSUE #36's PERMANENT REGRESSION HOME. sequencer_retrigger.cpp proves the
// property (an already-due note-off keeps its TRUE sample position, byte-identical
// from block 32 to 4096); this file freezes the resulting stream so the fix can
// never be quietly undone by a rewrite that still satisfies the property test.
//
// It is deliberately the FRACTIONAL-CLOCK file (137 BPM / 44.1 kHz), because the
// rate-sensitive arithmetic in the engine is exactly `llround (gateFraction *
// samplesPerStep)` and this is the golden about LEN.
//
//   LEN length 3 = {50, 150, 400} — under, over and far over one step, so every
//     step's note-off lands in a different relationship to the next step.
//   PITCH length 2 = {0, -1} — with the `up` traversal the degree becomes
//     0,0,2,2,4,4,6,6,… so the SAME PITCH lands on adjacent steps, which is what
//     makes the retrigger path reachable at all. Both retrigger branches are
//     exercised: the already-due one (after a 50% step) and the still-sounding one
//     (after a 400% step).

constexpr int len3[] = { 50, 150, 400 };
constexpr int pitch2[] = { 0, -1 };

void configureTiedRetrigger (PatternDocument& document)
{
    document.beginTransaction ();

    document.setLaneLength (0, LaneId::len, 3);
    document.setLaneLength (0, LaneId::pitch, 2);

    for (int s = 0; s < 3; ++s)
        document.setLaneValue (0, LaneId::len, s, len3[s]);
    for (int s = 0; s < 2; ++s)
        document.setLaneValue (0, LaneId::pitch, s, pitch2[s]);

    document.endTransaction ();
}

void configureTiedRetriggerPerturbed (PatternDocument& document)
{
    configureTiedRetrigger (document);
    document.setLaneValue (0, LaneId::len, 1, 149); // was 150
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. direction-modes-cycle — the nine DETERMINISTIC §12.3 traversals
// ─────────────────────────────────────────────────────────────────────────────
// One mode per pattern on indices 0..8, walked with BAR-QUANTIZED
// `queuePatternSwitch` commands, one per bar, over ten bars. Direction is a
// DOCUMENT property, so nine modes means nine patterns rather than nine document
// edits mid-render — and routing the walk through the switch machinery means this
// file also freezes §6.1's quantized switch inside a golden rather than only in
// pattern_switch.cpp's literals.
//
// BAR 10 SWITCHES BACK TO PATTERN 0, which buys a free internal-consistency check
// asserted in the case below: every lane is 16 steps = one bar and the gate is all
// on, so the gated ordinal at step 144 is 144 and 144 % 8 == 0 — bar 10 must
// reproduce bar 1's pitch sequence exactly. A traversal that had picked up state
// across the eight intervening switches would not.
//
// Velocity is 100 + patternIndex, so the pattern that emitted any given note is a
// BYTE in the file rather than an inference from timing.

constexpr DirectionMode deterministicModes[] = {
    DirectionMode::up,       DirectionMode::down,    DirectionMode::upDownInclusive, DirectionMode::upDownExclusive,
    DirectionMode::converge, DirectionMode::diverge, DirectionMode::outsideIn,       DirectionMode::asPlayed,
    DirectionMode::spiral,
};
constexpr int numDeterministicModes = static_cast<int> (std::size (deterministicModes));

void configureDirectionCycle (PatternDocument& document)
{
    document.beginTransaction ();

    for (int pattern = 0; pattern < numDeterministicModes; ++pattern)
    {
        document.setDirection (pattern, deterministicModes[pattern]);

        for (int s = 0; s < maxSteps; ++s)
        {
            document.setLaneValue (pattern, LaneId::gate, s, 1);
            document.setLaneValue (pattern, LaneId::vel, s, 100 + pattern);
        }
    }

    document.endTransaction ();
}

void configureDirectionCyclePerturbed (PatternDocument& document)
{
    configureDirectionCycle (document);
    document.setLaneValue (4, LaneId::vel, 0, 99); // pattern 4 (converge), step 0
}

/** One bar-quantized switch per bar. Every command sample is a multiple of
    `goldenAlignmentUnit` AND strictly inside its bar (never ON a bar line, which
    would fire on THAT bar — see pattern_switch.cpp), so each lands on the
    following bar line: 92160, 184320, … 829440. */
std::vector<ScheduledCommand> directionCycleSchedule ()
{
    auto schedule = startPlaying (goldenBpm);

    struct Step
    {
        std::int64_t at;
        int pattern;
    };

    // at, target — each `at` is k * 61440 and sits inside the bar before the landing.
    constexpr Step steps[] = {
        { 61440, 1 },  { 122880, 2 }, { 245760, 3 }, { 307200, 4 }, { 430080, 5 },
        { 491520, 6 }, { 614400, 7 }, { 675840, 8 }, { 798720, 0 },
    };

    for (const auto& step : steps)
        schedule.push_back (ScheduledCommand { step.at, patternSwitchCommand (step.pattern, QuantizeMode::bar) });

    // The §5.5 flush point every golden here ends on (see `playThenStop`).
    schedule.push_back (ScheduledCommand { tenBarMusic, engineCommand (EngineCommandType::transportStop) });
    return schedule;
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. euclid-gate — the euclidean GATE-lane generator
// ─────────────────────────────────────────────────────────────────────────────
// Three necklaces in one file, walked with bar-quantized switches over four bars:
// E(5,8) at rest, E(7,16) rotated 3, E(3,8) rotated 2, then back to E(5,8).
// pattern_euclid.cpp pins the necklaces themselves against hand-derived literals;
// this freezes what they SOUND like once the gated-cursor rule has run over them,
// which is the part a necklace test cannot see. Bresenham's phase always puts a
// pulse on step 0, and `rotate` is measured from it (Euclid.h) — so E(5,8,0) and
// E(3,8,2) both fire on their step 0 while E(7,16,3) rests there, which is
// directly visible in the file.
//
// Velocity is 100 / 110 / 120 per pattern, again so the source of a note is a byte.

void configureEuclid (PatternDocument& document)
{
    document.beginTransaction ();

    document.applyEuclid (0, 8, 5, 0);  // x.x.xx.x
    document.applyEuclid (1, 16, 7, 3); // .x.x..x.x.x..x.x
    document.applyEuclid (2, 8, 3, 2);  // x.x..x..

    for (int s = 0; s < maxSteps; ++s)
    {
        document.setLaneValue (0, LaneId::vel, s, 100);
        document.setLaneValue (1, LaneId::vel, s, 110);
        document.setLaneValue (2, LaneId::vel, s, 120);
    }

    document.endTransaction ();
}

void configureEuclidPerturbed (PatternDocument& document)
{
    configureEuclid (document);
    document.setLaneValue (1, LaneId::gate, 0, 1); // E(7,16,3) rests on step 0; make it fire
}

std::vector<ScheduledCommand> euclidSchedule ()
{
    auto schedule = startPlaying (goldenBpm);
    schedule.push_back (ScheduledCommand { 61440, patternSwitchCommand (1, QuantizeMode::bar) });  // → bar 2
    schedule.push_back (ScheduledCommand { 122880, patternSwitchCommand (2, QuantizeMode::bar) }); // → bar 3
    schedule.push_back (ScheduledCommand { 245760, patternSwitchCommand (0, QuantizeMode::bar) }); // → bar 4
    schedule.push_back (ScheduledCommand { fourBarMusic, engineCommand (EngineCommandType::transportStop) });
    return schedule;
}

// ═════════════════════════════════════════════════════════════════════════════
// PHASE 7 — THE SIX STEP-LOGIC GOLDENS (rngVersion 1)
//
// ── WHY THEY ARE STAMPED `rngVersion: 1` EVEN WHEN NO HASH IS EVALUATED ─────
// `rngVersion` describes the SCHEMA's RNG algorithm, not whether a particular
// pattern happened to consume randomness. `cond-a-b-8loops`, `ratchet-ramp-8`,
// `micro-swing-compose` and `ratchet-swing-retrigger` all sit at the PROB lane's
// default 100, so `probabilityPasses` returns through its `>= 100` short-circuit
// and not one `splitmix64` round runs — that short-circuit is a documented CONTRACT
// in StepLogic.cpp precisely so this is provable rather than probable. They are
// still Phase-7 patterns and a Phase-7 loader reads them as rngVersion 1.
//
// The six PHASE-6 goldens above keep `rngVersion: 0`, and that is the whole reason
// the blanket `REQUIRE (rngVersion == 0)` in the inventory case had to become the
// per-scenario table below: leaving it blanket forces either a lie (stamping the new
// files 0) or a spurious failure.
// ═════════════════════════════════════════════════════════════════════════════

// ── A SMALLER ALIGNED SPAN FOR THE RATCHET-8 FRACTIONAL FILE ────────────────
// `ratchet-swing-retrigger` puts EIGHT notes on every step. At `fractionalMusic`
// (614400 samples = 127.25 steps) that is 1024 note-ons and a ~2050-line golden —
// and Rule zero rests on a golden being AUDITABLE BY EYE ("a golden nobody can read
// by eye is a golden nobody will ever audit"). Two alignment units carry 25 steps,
// which is enough for the swing pairing to alternate a dozen times and for the
// retrigger cap to bind two hundred times, in ~420 lines. Still an exact multiple of
// `goldenAlignmentUnit`, so the sweep's precondition holds unchanged.
constexpr std::int64_t ratchetFractionalMusic = 2 * goldenAlignmentUnit;                     ///< 122880
constexpr std::int64_t ratchetFractionalSpan = ratchetFractionalMusic + goldenAlignmentUnit; ///< 184320

/** Writes `value` into every one of pattern `index`'s storage slots for `lane`. */
void fillLane (PatternDocument& document, int index, LaneId lane, int value)
{
    for (int step = 0; step < maxSteps; ++step)
        document.setLaneValue (index, lane, step, value);
}

/** A velocity that NAMES ITS STEP: `100 + step`, written into a 16-long VEL lane, so
    every note in the file says which of the sixteen phases produced it as a BYTE
    rather than as an inference from timing. Used by both COND goldens, where the
    whole question is WHICH steps fired. */
void velocityNamesTheStep (PatternDocument& document, int index)
{
    document.setLaneLength (index, LaneId::vel, 16);

    for (int step = 0; step < 16; ++step)
        document.setLaneValue (index, LaneId::vel, step, 100 + step);
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. cond-a-b-8loops — THE §12.2 ACCEPTANCE-CRITERION GOLDEN
// ─────────────────────────────────────────────────────────────────────────────
// INSTRUCTIONS Phase 7's criterion is "3:4 fires exactly on loop 3 of every 4
// (golden-verified)", and this is that golden. Eight loops of sixteen steps = eight
// bars, so a `1:8` has room to fire exactly once and a `3:4` exactly twice.
//
// THE GATE LANE FIRES ON ONLY FOUR OF THE SIXTEEN STEPS, one per conditioned phase.
// That is not economy — it is what makes the file READABLE: every event in it is a
// conditioned step, so a reader is never asked to separate "this step passed its
// condition" from "this step had no condition". The unconditioned steps would
// otherwise be 87 % of the file.
//
// D5 IS WHAT IS BEING FROZEN. "A loop" is the GATE LANE'S CYCLE (`gatePeriodSteps`),
// which here is the 16-step lane length, so loop index == bar index and a reviewer
// divides an absolute sample by 92160 to get the loop. The four conditions were
// chosen so that between them they pin every part of the decode:
//
//   step  0  `3:4`   ⇒ floorMod (loop, 4) == 2  ⇒ loops 2 and 6      — TWO note-ons
//   step  4  `1:2`   ⇒ floorMod (loop, 2) == 0  ⇒ loops 0, 2, 4, 6
//   step  8  `2:2`   ⇒ floorMod (loop, 2) == 1  ⇒ loops 1, 3, 5, 7
//   step 12  `1:8`   ⇒ floorMod (loop, 8) == 0  ⇒ loop 0 only        — ONE note-on
//
// `1:2` and `2:2` are complementary by construction, so together they must account
// for exactly one note per loop — an internal consistency check the case asserts.
// And `a - 1`: §12.2 numbers loops from 1 while `loopIndexAt` numbers them from 0, so
// `3:4` fires on loop INDEX 2. Dropping the `- 1` would fire it one loop late,
// forever, and this file is what makes that a red test rather than a shipped bug.
//
// THE SUPPRESSED STEPS STILL CONSUME THEIR GATED ORDINAL (the "PHASE 7, READ THIS
// BEFORE YOU 'FIX' IT" rule on `gatedOrdinal`), which is directly visible here: the
// pitch of step 0's note in loop 2 is pool[8 % 8] = pool[0], i.e. the ordinal counted
// all four gated steps of loops 0 and 1 even though six of those eight were
// suppressed by their conditions.

void configureConditionCycle (PatternDocument& document)
{
    document.beginTransaction ();

    // Four gated phases out of sixteen — one per condition under test.
    fillLane (document, 0, LaneId::gate, 0);
    document.setLaneValue (0, LaneId::gate, 0, 1);
    document.setLaneValue (0, LaneId::gate, 4, 1);
    document.setLaneValue (0, LaneId::gate, 8, 1);
    document.setLaneValue (0, LaneId::gate, 12, 1);

    document.setLaneLength (0, LaneId::cond, 16);
    document.setLaneValue (0, LaneId::cond, 0, static_cast<int> (TrigCondition::ab3of4));
    document.setLaneValue (0, LaneId::cond, 4, static_cast<int> (TrigCondition::ab1of2));
    document.setLaneValue (0, LaneId::cond, 8, static_cast<int> (TrigCondition::ab2of2));
    document.setLaneValue (0, LaneId::cond, 12, static_cast<int> (TrigCondition::ab1of8));

    velocityNamesTheStep (document, 0);

    document.endTransaction ();
}

void configureConditionCyclePerturbed (PatternDocument& document)
{
    configureConditionCycle (document);
    // `3:4` becomes `4:4` — ONE ordinal, and the two note-ons move from loops 2 and 6
    // to loops 3 and 7. This is the perturbation that a decode off by one would make
    // indistinguishable from the reference, so it is the right negative control.
    document.setLaneValue (0, LaneId::cond, 0, static_cast<int> (TrigCondition::ab4of4));
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. cond-pre-chain — D6's PRE CHAIN, FROZEN ON THE DEPTH-8 BOUNDARY
// ─────────────────────────────────────────────────────────────────────────────
// A run of TEN consecutive `PRE` steps behind ONE anchor. That length is chosen to
// straddle `maxPreChainDepth` exactly, so the file freezes BOTH sides of D6's
// truncation rule instead of only the working side:
//
//   steps 1..8   the backward walk reaches the anchor within the budget (step k needs
//                k pushes, and the budget check trips when the depth already stands at
//                8) ⇒ these steps inherit the anchor's result
//   steps 9, 10  the budget is exhausted BEFORE the anchor is reached ⇒ D6's base case
//                `false` ⇒ THESE STEPS ARE SILENT, FOREVER, WHATEVER THE ANCHOR DID
//
// VELOCITY NAMES THE STEP, which turns that second row into the strongest assertion
// in this file: the bytes 0x6D (109) and 0x6E (110) must appear NOWHERE in the
// golden. A future edit that made the truncation base case `true`, or that raised the
// budget, would add them. `!= false` is not observable from a stream; a missing byte
// is.
//
// ── AND THE ANCHOR'S PROBABILITY COUNTS ─────────────────────────────────────
// The anchor (step 0) sits at PROB 50 with a fixed `masterSeed`, and every link is at
// PROB 100. So the anchor's roll alone decides whether steps 0..8 sound, and because
// the roll is keyed on the GLOBAL step index the answer differs from loop to loop —
// which is what makes the semantic VISIBLE rather than merely implemented. Seeding
// the anchor from its condition alone (ignoring its probability) would make every
// loop fire, and the case below asserts out loud that the file contains both a loop
// where the chain sounded and a loop where it did not.

/** The PRE run's length. TEN, deliberately straddling `maxPreChainDepth` (8) — see
    the note above. Steps 1..8 resolve against the anchor; 9 and 10 truncate. */
constexpr int preChainLength = 10;

/** The anchor's PROB. Sub-100 on purpose: it is what makes "the anchor's probability
    counts" a property of the file rather than of a comment. */
constexpr int preAnchorProbPercent = 50;

/** The seed the anchor's roll is drawn against. Written here, in the test, because
    every expectation about which loops sound descends from it. */
constexpr std::uint64_t preChainSeed = 0x00C0FFEE0BADF00DULL;

void configurePreChain (PatternDocument& document)
{
    document.beginTransaction ();

    fillLane (document, 0, LaneId::gate, 1);
    document.setMasterSeed (0, preChainSeed);

    document.setLaneLength (0, LaneId::cond, 16);
    for (int step = 1; step <= preChainLength; ++step)
        document.setLaneValue (0, LaneId::cond, step, static_cast<int> (TrigCondition::pre));

    document.setLaneLength (0, LaneId::prob, 16);
    document.setLaneValue (0, LaneId::prob, 0, preAnchorProbPercent);

    velocityNamesTheStep (document, 0);

    document.endTransaction ();
}

void configurePreChainPerturbed (PatternDocument& document)
{
    configurePreChain (document);
    // ── THE ANCHOR'S PROB, 50 -> 100 ─────────────────────────────────────────
    // The perturbation has to be about the SEMANTIC this file freezes, and it has to
    // actually change the render. The first attempt inverted the anchor's CONDITION
    // (`none` -> `!1ST`, which differs only in loop 0) and produced a BYTE-IDENTICAL
    // stream — because with this seed the anchor's PROB roll already failed in loop 0,
    // so flipping a condition that only bites there changed nothing. That is a
    // negative control which controls for nothing, and it is precisely the failure
    // mode the perturbed-render guard exists to prevent, so it was replaced rather
    // than tuned.
    //
    // At PROB 100 the anchor takes the `>= 100` short-circuit and fires in EVERY loop,
    // so all eight loops sound their chain instead of three: guaranteed different, and
    // different for the documented reason.
    document.setLaneValue (0, LaneId::prob, 0, 100);
}

// ─────────────────────────────────────────────────────────────────────────────
// 9. prob-seeded-50 — THE SEEDED PROBABILITY ROLL
// ─────────────────────────────────────────────────────────────────────────────
// PROB 50 on every step at a fixed `masterSeed`. This is the ONLY golden in the
// repository whose content is decided by `splitmix64`, which makes it the on-disk
// half of Rng.h's load-bearing `static_assert`: the compile-time check pins
// `splitmix64 (0)`, and this pins the whole realised sequence of rolls.
//
// "LOOP-STABLE" IS READ IN THE DETERMINISM SENSE, and this file is where that
// reading is frozen. The roll is keyed on the GLOBAL step index, so bar 2's rhythm
// DIFFERS from bar 1's — literal per-loop repetition is LOOP LOCK, which is Phase 12
// and would be the wrong default to bake here. The case asserts the difference, so a
// future LOOP LOCK implementation cannot quietly become the default.
//
// tests/step_probability.cpp already checks the roll against a longhand reference
// implementation over 11 001 indices; what it cannot do is prove that the rolls the
// ENGINE actually applied are those rolls. That is this file.

constexpr int probPercent = 50;
constexpr std::uint64_t probSeed = 0x0123456789ABCDEFULL;

void configureSeededProbability (PatternDocument& document)
{
    document.beginTransaction ();

    fillLane (document, 0, LaneId::gate, 1);
    fillLane (document, 0, LaneId::prob, probPercent);
    document.setMasterSeed (0, probSeed);

    document.endTransaction ();
}

void configureSeededProbabilityPerturbed (PatternDocument& document)
{
    configureSeededProbability (document);
    // THE SEED, not a lane. Every roll moves, so this is the negative control that a
    // `masterSeed` never reaching the RT path would fail (before Phase 7.1 the audio
    // thread genuinely could not see it — see the note on `PatternData::masterSeed`).
    document.setMasterSeed (0, probSeed ^ 1ULL);
}

// ─────────────────────────────────────────────────────────────────────────────
// 10. ratchet-ramp-8 — RATCHET 1..8 AND THE VELOCITY RAMP
// ─────────────────────────────────────────────────────────────────────────────
// The RATCHET lane cycles 1, 2, 3, 4, 5, 6, 7, 8 over an 8-long lane, so all eight
// legal child counts appear twice per bar and their slots are 5760, 2880, 1920, 1440,
// 1152, 960, 822.857… and 720 samples.
//
// ── STEP 6 (SEVEN CHILDREN) IS WHY THIS FILE EXISTS ─────────────────────────
// Seven does not divide 5760, so its slot is 822.857142… samples — the one child
// count whose onsets cannot be written down without deciding what the engine does
// with the fraction. `SequencerProcessor::sampleForPpq` is a single SNAP-THEN-FLOOR
// (`floor (rawOffset + 1e-4)`), and each child's position is derived INDEPENDENTLY
// from the parent (`child / count`), never by cumulative addition of a rounded slot.
// So step 6's children sit at grid + {0, 822, 1645, 2468, 3291, 4114, 4937}, every
// one of which a reviewer can check with a calculator.
//
// TWO THINGS ABOUT THAT ROW, BOTH WORTH KNOWING BEFORE CHANGING IT:
//   * The Phase-7 plan predicted {0, 823, 1646, 2469, 3291, 4114, 4937} — the
//     ROUND-TO-NEAREST values. The engine floors, so the middle three differ by one
//     sample. The engine's arithmetic is what is frozen here; the plan's prose was
//     wrong, and this comment exists so the next reader does not "fix" the file to
//     match it.
//   * Cumulative addition of a rounded 823-sample slot would give
//     {0, 823, 1646, 2469, 3292, 4115, 4938} — drifting by 1 sample at child 4 and
//     by 2 at child 6 (and by ~6 samples on a longer step). That is the fails-without
//     shape, and it is why the sixth and seventh entries are the load-bearing ones.
//
// The ramp is -50 %, linear from the step's own VEL at child 0 to half of it at the
// LAST child — interpolated over `count - 1`, so "-50 % halves the last child" is
// literally true at every child count. At VEL 100 with seven children that is
// 100, 92, 83, 75, 67, 58, 50 (`llround` of 100 * (1 - 0.5 * c/6)).

constexpr int ratchetLaneLength = 8;
constexpr double ratchetRampPct = -50.0;

void configureRatchetRamp (PatternDocument& document)
{
    document.beginTransaction ();

    fillLane (document, 0, LaneId::gate, 1);
    document.setRatchetVelocityRamp (ratchetRampPct);

    document.setLaneLength (0, LaneId::ratchet, ratchetLaneLength);
    for (int step = 0; step < ratchetLaneLength; ++step)
        document.setLaneValue (0, LaneId::ratchet, step, step + 1);

    document.endTransaction ();
}

void configureRatchetRampPerturbed (PatternDocument& document)
{
    configureRatchetRamp (document);
    // SEVEN CHILDREN BECOME EIGHT on the one step whose slot is fractional. The whole
    // {822, 1645, 2468, …} row moves to {720, 1440, 2160, …}, so the perturbation
    // lands squarely on the rounding this golden is about.
    document.setLaneValue (0, LaneId::ratchet, 6, 8);
}

// ─────────────────────────────────────────────────────────────────────────────
// 11. micro-swing-compose — THE COMPOSED DISPLACEMENT, INCLUDING ITS CLAMP
// ─────────────────────────────────────────────────────────────────────────────
// `swingPct` 75 (the §12.1 maximum ⇒ +0.5 step on every ODD global step) over a
// 4-long MICRO lane. Four is deliberate: an EVEN lane length pins each slot to one
// PARITY, so slot 0 and slot 2 are always even steps (no swing) and slots 1 and 3 are
// always odd (swing). That is what makes the composition legible in the file.
//
//   slot 0  MICRO +25, even ⇒ +0.25            ⇒ grid + 1440
//   slot 1  MICRO -25, odd  ⇒ -0.25 + 0.5      ⇒ grid + 1440   (SAME PLACE, other route)
//   slot 2  MICRO -50, even ⇒ -0.50            ⇒ grid - 2880   (the maximum EARLY shift)
//   slot 3  MICRO +25, odd  ⇒  0.25 + 0.5 = 0.75, CLAMPED to 0.5 ⇒ grid + 2880
//
// SLOT 3 IS THE POINT. Its raw composition is 0.75 of a step — an unclamped engine
// would place it at grid + 4320. The clamp is on the TOTAL and it is the walk's scan
// widening (`stepScanBack`/`stepScanForward`, both ATTAINED) that depends on it, so
// the saturation is frozen here as an absolute sample rather than only asserted as a
// return value in tests/step_microswing.cpp. Slots 0 and 3 carry the IDENTICAL MICRO
// value (+25) and land 1440 samples apart, which is the swing term made visible.
//
// ── WHY MICRO IS NOT THE PLAN'S {+50, -50, 0, +25} ──────────────────────────
// A +0.5 shift immediately followed by a -0.5 shift puts two steps on ONE sample
// (their grid positions are 5760 apart and their displacements differ by exactly
// 5760). That is legal and deterministic — both are emitted by the block that
// contains the sample, in index order — but it makes the file's onsets ambiguous to
// read, and reviewability is the whole premise of Rule zero. This set reaches the
// same three behaviours (maximum early shift, maximum late shift, and a saturating
// composition) with no coincident onsets. `substep_ownership.cpp` is where the
// non-monotonic and coincident geometry is exercised, with counters instead of eyes.

constexpr int microLaneLength = 4;
constexpr int microLane[microLaneLength] = { 25, -25, -50, 25 };
constexpr double composeSwingPct = 75.0;

void configureMicroSwing (PatternDocument& document)
{
    document.beginTransaction ();

    fillLane (document, 0, LaneId::gate, 1);
    document.setSwing (composeSwingPct);

    document.setLaneLength (0, LaneId::micro, microLaneLength);
    for (int step = 0; step < microLaneLength; ++step)
        document.setLaneValue (0, LaneId::micro, step, microLane[step]);

    document.endTransaction ();
}

void configureMicroSwingPerturbed (PatternDocument& document)
{
    configureMicroSwing (document);
    // SWING ONLY — every MICRO value is untouched. 75 -> 74 moves every odd step by
    // 115 samples and leaves every even step exactly where it was, so this is the
    // control that a swing term dropped from the composition would fail.
    document.setSwing (74.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 12. ratchet-swing-retrigger — EIGHT CHILDREN ON A NON-INTEGER STEP
// ─────────────────────────────────────────────────────────────────────────────
// The hostile clock this file already owns (137 BPM / 44.1 kHz ⇒ a 1/16 is
// 4828.4671… samples), RATCHET 8, swing 66 and LEN 150 % over a ONE-NOTE POOL.
// Nothing in it lands on a round number, which is exactly the point: the 5760-sample
// clock makes every ratchet slot an exact integer at counts 1, 2, 4 and 8, so it
// cannot catch a rounding drift in the child-onset derivation. Here the slot is
// 603.558… samples and the swing term is 0.32 of a step.
//
// ── LEN 150 % IS NOT DECORATION, IT IS WHAT MAKES THE FILE TEST ANYTHING ────
// A ratchet child's LEN applies to its OWN SLOT (`gateFractionOfStep` is already
// divided by the child count). At the default LEN 50 % each child therefore ends
// halfway through its own slot, the next child's onset is comfortably later, and THE
// RETRIGGER CAP NEVER BINDS — the file would freeze eight independent notes and say
// nothing about the intra-step chain. At 150 % each child wants 905 samples inside a
// 603-sample slot, so every one of them is cut short at `next onset - 1` and §5.5's
// 1-sample gap appears 200 times over.
//
// The ONE-NOTE pool makes every onset in the render a same-pitch retrigger — across
// steps as well as within them — so `cutoffForSamePitch`'s widened scan is exercised
// on both the `ahead == 0` intra-step band and the cross-step bands, on a clock where
// a one-sample error cannot hide behind an exact division.

constexpr int retriggerRatchetCount = 8;
constexpr int retriggerLenPercent = 150;
constexpr double retriggerSwingPct = 66.0;

void configureRatchetSwingRetrigger (PatternDocument& document)
{
    document.beginTransaction ();

    fillLane (document, 0, LaneId::gate, 1);
    fillLane (document, 0, LaneId::ratchet, retriggerRatchetCount);
    fillLane (document, 0, LaneId::len, retriggerLenPercent);
    document.setSwing (retriggerSwingPct);

    // ONE pool note, so every child of every step shares one pitch.
    arpbox::engine::PoolSnapshot pool {};
    pool.size = 1;
    pool.sorted[0] = static_cast<std::uint8_t> (poolPitches[0]);
    pool.asPlayed[0] = static_cast<std::uint8_t> (poolPitches[0]);
    document.setPool (pool);

    document.endTransaction ();
}

void configureRatchetSwingRetriggerPerturbed (PatternDocument& document)
{
    configureRatchetSwingRetrigger (document);
    // LEN 150 -> 149 %. One percent, and it moves every one of the ~200 cut-short
    // note-offs that DID NOT bind (none, at 149 %) — the shape stays, the samples move.
    fillLane (document, 0, LaneId::len, retriggerLenPercent - 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// The inventory — the single source of truth for "which goldens exist"
// ─────────────────────────────────────────────────────────────────────────────

/** One expected golden: its name and the `rngVersion` its header must declare.

    ── WHY THIS IS A TABLE AND NOT A BLANKET ASSERTION ─────────────────────────
    The inventory case used to end with `REQUIRE (golden.header.rngVersion == 0)` for
    every file. Phase 7 makes that unsatisfiable in the only two ways available:
    stamping the new goldens 0 would be a LIE about the schema they were produced
    under, and leaving the blanket check while stamping them 1 is a spurious failure.
    A per-scenario expectation is the honest third option — and it is strictly
    stronger, because it now also pins that no Phase-6 file has silently been
    re-versioned. */
struct ExpectedGolden
{
    const char* name;
    int rngVersion;
};

/** Every golden this suite owns, in the sorted order `listGoldenFiles` returns. */
constexpr ExpectedGolden expectedGoldens[] = {
    { "baseline-4bar", 0 },
    { "cond-a-b-8loops", 1 },
    { "cond-pre-chain", 1 },
    { "direction-modes-cycle", 0 },
    { "euclid-gate", 0 },
    { "micro-swing-compose", 1 },
    { "polymeter-clockdiv", 0 },
    { "polymeter-len-3-5-7", 0 },
    { "prob-seeded-50", 1 },
    { "ratchet-ramp-8", 1 },
    { "ratchet-swing-retrigger", 1 },
    { "tied-retrigger", 0 },
};
constexpr int numExpectedGoldens = static_cast<int> (std::size (expectedGoldens));

/** The `rngVersion` every Phase-7 golden is stamped with (`rng::rngVersion`). */
constexpr int phase7RngVersion = 1;

/** Note-on velocities present in the render, as a sorted unique list — the shape both
    COND goldens assert against, because "velocity names the step" turns "which steps
    fired" into a set of bytes. */
std::vector<int> velocitiesIn (const MidiRenderResult& render)
{
    std::vector<int> velocities;

    for (const auto& event : render.events)
        if (event.message.isNoteOn ())
            velocities.push_back (event.message.getVelocity ());

    std::sort (velocities.begin (), velocities.end ());
    velocities.erase (std::unique (velocities.begin (), velocities.end ()), velocities.end ());
    return velocities;
}

/** Absolute samples of every note-on carrying `velocity`, in emission order. */
std::vector<std::int64_t> onsetsWithVelocity (const MidiRenderResult& render, int velocity)
{
    std::vector<std::int64_t> samples;

    for (const auto& event : render.events)
        if (event.message.isNoteOn () && event.message.getVelocity () == velocity)
            samples.push_back (event.absoluteSample);

    return samples;
}

/** Note-on count. */
std::size_t noteOnCount (const MidiRenderResult& render)
{
    return render.select ([] (const TimedMidiEvent& event) { return event.message.isNoteOn (); }).size ();
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Preconditions the six cases all rest on — asserted once, out loud
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/golden: the canonical clock and the block sweep are what the files assume", "[determinism]")
{
    // The clock. 125 BPM / 48 kHz / 1-16: step = 0.25 * (60/125) * 48000.
    REQUIRE (stepSamples == 5760);
    REQUIRE (beatSamples == 23040);
    REQUIRE (barSamples == 92160);
    REQUIRE (barSamples == 16 * stepSamples);

    // The alignment unit really is the lcm of the swept sizes: every size divides
    // it, and no proper divisor of it does (checked via the halves of its two odd
    // factors, which is what a wrong lcm would trip on).
    for (const int blockSize : goldenBlockSizes)
    {
        INFO ("block size " << blockSize);
        REQUIRE (goldenAlignmentUnit % blockSize == 0);
    }
    REQUIRE (goldenAlignmentUnit == 61440);
    REQUIRE (numGoldenBlockSizes == 10);

    // Every span this file uses is a whole number of blocks at every swept size.
    REQUIRE (spanCoversWholeBlocks (fourBarMusic));
    REQUIRE (spanCoversWholeBlocks (eightBarMusic));
    REQUIRE (spanCoversWholeBlocks (tenBarMusic));
    REQUIRE (spanCoversWholeBlocks (fractionalMusic));
    REQUIRE (fourBarMusic == 368640);
    REQUIRE (eightBarMusic == 737280);
    REQUIRE (tenBarMusic == 921600);
    REQUIRE (fractionalMusic == 614400);

    REQUIRE (spanCoversWholeBlocks (fourBarSpan));
    REQUIRE (spanCoversWholeBlocks (eightBarSpan));
    REQUIRE (spanCoversWholeBlocks (tenBarSpan));
    REQUIRE (spanCoversWholeBlocks (fractionalSpan));
    REQUIRE (fourBarSpan == 430080);
    REQUIRE (eightBarSpan == 798720);
    REQUIRE (tenBarSpan == 983040);
    REQUIRE (fractionalSpan == 675840);

    // Every render leaves at least one whole block after its stop for the flush.
    REQUIRE (fourBarSpan - fourBarMusic == goldenAlignmentUnit);
    REQUIRE (eightBarSpan - eightBarMusic == goldenAlignmentUnit);
    REQUIRE (tenBarSpan - tenBarMusic == goldenAlignmentUnit);
    REQUIRE (fractionalSpan - fractionalMusic == goldenAlignmentUnit);

    // THE TRAP THIS FILE'S LOCAL CONSTANTS EXIST FOR: transport_timing.cpp's
    // config A (60 BPM / 48 kHz / 4 bars) is 768000 samples = 12.5 x 61440, so it
    // does NOT align once 4096 is in the sweep. Asserted so a future author who
    // "simplifies" by importing those constants finds out here.
    REQUIRE (! spanCoversWholeBlocks (768000));

    // ── PHASE 7's EXTRA SPAN (see `ratchetFractionalMusic`) ──────────────────
    // A RATCHET-8 file at `fractionalMusic` would be ~2050 lines, and Rule zero rests
    // on the file being readable by eye. Two alignment units carry 25 steps of the
    // hostile clock, which is enough, and it aligns everywhere just as the others do.
    REQUIRE (spanCoversWholeBlocks (ratchetFractionalMusic));
    REQUIRE (spanCoversWholeBlocks (ratchetFractionalSpan));
    REQUIRE (ratchetFractionalMusic == 122880);
    REQUIRE (ratchetFractionalSpan == 184320);
    REQUIRE (ratchetFractionalSpan - ratchetFractionalMusic == goldenAlignmentUnit);
    REQUIRE (ratchetFractionalMusic < fractionalMusic);

    // Every schedule in this file lands on block heads everywhere.
    REQUIRE (scheduleAlignsEverywhere (playThenStop (goldenBpm, fourBarMusic)));
    REQUIRE (scheduleAlignsEverywhere (playThenStop (goldenBpm, eightBarMusic)));
    REQUIRE (scheduleAlignsEverywhere (playThenStop (fractionalBpm, fractionalMusic)));
    REQUIRE (scheduleAlignsEverywhere (playThenStop (fractionalBpm, ratchetFractionalMusic)));
    REQUIRE (scheduleAlignsEverywhere (directionCycleSchedule ()));
    REQUIRE (scheduleAlignsEverywhere (euclidSchedule ()));

    // EVERY schedule in this file ends with a stop — that is what makes
    // `allLifecyclesBalanced` a universal claim rather than a per-case exemption.
    for (const auto& schedule : { playThenStop (goldenBpm, fourBarMusic),
                                  playThenStop (goldenBpm, eightBarMusic),
                                  playThenStop (fractionalBpm, fractionalMusic),
                                  playThenStop (fractionalBpm, ratchetFractionalMusic),
                                  directionCycleSchedule (),
                                  euclidSchedule () })
    {
        REQUIRE (! schedule.empty ());
        REQUIRE (schedule.back ().command.type == EngineCommandType::transportStop);
    }

    // Nine deterministic modes, and the two seeded ones deliberately absent
    // (see the omission note at the top of this file).
    REQUIRE (numDeterministicModes == 9);
    REQUIRE (arpbox::engine::numDirectionModes == 11);
    for (const auto mode : deterministicModes)
    {
        REQUIRE (mode != DirectionMode::walk);
        REQUIRE (mode != DirectionMode::randomNoRepeat);
    }
    REQUIRE (poolSize == 8);
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. baseline-4bar
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/golden: baseline 4-bar ascending arpeggio", "[determinism]")
{
    const auto schedule = playThenStop (goldenBpm, fourBarMusic);
    const auto bake = renderScenario (&configureBaseline, schedule, goldenSampleRate, fourBarSpan, bakeBlockSize);

    // Span, explicitly: operator== ignores it (see the header note).
    REQUIRE (bake.numSamples == 430080);
    REQUIRE (bake.isSampleSorted ());

    // Anti-vacuity: 64 gated steps, one note-on and one note-off each.
    REQUIRE (bake.events.size () == 128u); // 64 gated steps x (on + off)

    // ── THE LITERAL, WRITTEN HERE AND NOT READ FROM THE FILE ─────────────────
    // Step 0 is pool[0] = 60 = 0x3C at velocity 100 = 0x64, released after 50% of
    // a 5760-sample step. Step 3 is pool[3] = 65 = 0x41 at 3 * 5760 = 17280.
    REQUIRE (eventIs (bake.events[0], 0, 0x90, 0x3C, 0x64));
    REQUIRE (eventIs (bake.events[1], 2880, 0x80, 0x3C, 0x00));
    REQUIRE (containsEvent (bake, 3 * stepSamples, 0x90, 0x41, 0x64));
    REQUIRE (containsEvent (bake, 17280, 0x90, 0x41, 0x64));

    // ── THE §5.5 STOP FLUSH ────────────────────────────────────────────────
    // Nothing plays past the musical span, and the flush itself emits NOTHING:
    // this scenario's gates leave the sounding-note table already EMPTY at the
    // stop, and `SoundingNoteTable::flush` sweeps CC123 only on channels it
    // actually sounded on. So the stream balances because every note was
    // released on time, not because a flush rescued it — which is the stronger
    // statement. (tied-retrigger is the deliberate opposite: see that case.)
    REQUIRE (pitchesIn (bake, fourBarMusic, fourBarSpan).empty ());
    REQUIRE (bake.events.back ().absoluteSample < fourBarMusic);

    const auto check = checkGolden (bake, headerFor (bake, "baseline-4bar", goldenBpm, goldenGridPpq));
    INFO (check.report);
    REQUIRE (check.passed);

    const auto golden = loadGolden ("baseline-4bar");
    INFO (golden.error);
    REQUIRE (golden.ok);

    const auto sweep = sweepAgainstGolden (golden, &configureBaseline, schedule, goldenSampleRate, fourBarSpan);
    INFO (sweep.report);
    REQUIRE (sweep.sizesChecked == numGoldenBlockSizes);
    REQUIRE (sweep.sizesMatched == numGoldenBlockSizes);
    REQUIRE (sweep.spansCorrect);
    REQUIRE (sweep.allSorted);
    REQUIRE (sweep.minNoteOns > 0); // gates the balance claim below — see SweepOutcome
    REQUIRE (sweep.allLifecyclesBalanced);
    REQUIRE (sweep.minEvents == sweep.maxEvents);
    REQUIRE (sweep.minEvents == 128);

    // ── THE PERTURBED-RENDER NEGATIVE CONTROL ────────────────────────────────
    const auto perturbed =
        renderScenario (&configureBaselinePerturbed, schedule, goldenSampleRate, fourBarSpan, bakeBlockSize);
    REQUIRE (perturbed.events.size () == bake.events.size ()); // same shape…
    REQUIRE (! compareToGolden (perturbed, golden).matches);   // …different performance
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. polymeter-len-3-5-7
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/golden: polymetric lane lengths 5, 3 and 7", "[determinism]")
{
    const auto schedule = playThenStop (goldenBpm, eightBarMusic);
    const auto bake =
        renderScenario (&configureLengthPolymeter, schedule, goldenSampleRate, eightBarSpan, bakeBlockSize);

    REQUIRE (bake.numSamples == 798720);
    REQUIRE (bake.isSampleSorted ());
    REQUIRE (bake.events.size () == 204u); // 102 gated steps of 128 x (on + off)

    // ── THE LITERAL ──────────────────────────────────────────────────────────
    // GATE = {1,1,0,1,1} ⇒ 4 pulses per 5 steps, exclusive prefix {0,1,2,2,3}.
    // Step 0: ordinal 0 → pool[0] = 60 = 0x3C; PITCH[0 % 3] = 0; VEL[0 % 7] = 40 = 0x28.
    // Step 6: ordinal = floorDiv(6,5)*4 + prefix[1] = 4 + 1 = 5 → pool[5] = 69 = 0x45;
    //         PITCH[6 % 3 = 0] = 0; VEL[6 % 7 = 6] = 127 = 0x7F; at 6 * 5760 = 34560.
    REQUIRE (eventIs (bake.events[0], 0, 0x90, 0x3C, 0x28));
    REQUIRE (containsEvent (bake, 34560, 0x90, 0x45, 0x7F));
    REQUIRE (containsEvent (bake, 6 * stepSamples, 0x90, 0x45, 0x7F));

    // ── THE §5.5 STOP FLUSH ────────────────────────────────────────────────
    // Nothing plays past the musical span, and the flush itself emits NOTHING:
    // this scenario's gates leave the sounding-note table already EMPTY at the
    // stop, and `SoundingNoteTable::flush` sweeps CC123 only on channels it
    // actually sounded on. So the stream balances because every note was
    // released on time, not because a flush rescued it — which is the stronger
    // statement. (tied-retrigger is the deliberate opposite: see that case.)
    REQUIRE (pitchesIn (bake, eightBarMusic, eightBarSpan).empty ());
    REQUIRE (bake.events.back ().absoluteSample < eightBarMusic);

    const auto check = checkGolden (bake, headerFor (bake, "polymeter-len-3-5-7", goldenBpm, goldenGridPpq));
    INFO (check.report);
    REQUIRE (check.passed);

    const auto golden = loadGolden ("polymeter-len-3-5-7");
    INFO (golden.error);
    REQUIRE (golden.ok);

    const auto sweep = sweepAgainstGolden (golden, &configureLengthPolymeter, schedule, goldenSampleRate, eightBarSpan);
    INFO (sweep.report);
    REQUIRE (sweep.sizesChecked == numGoldenBlockSizes);
    REQUIRE (sweep.sizesMatched == numGoldenBlockSizes);
    REQUIRE (sweep.spansCorrect);
    REQUIRE (sweep.allSorted);
    REQUIRE (sweep.minNoteOns > 0); // gates the balance claim below — see SweepOutcome
    REQUIRE (sweep.allLifecyclesBalanced);
    REQUIRE (sweep.minEvents == sweep.maxEvents);
    REQUIRE (sweep.minEvents == 204);

    const auto perturbed =
        renderScenario (&configureLengthPolymeterPerturbed, schedule, goldenSampleRate, eightBarSpan, bakeBlockSize);
    REQUIRE (! perturbed.empty ());
    REQUIRE (! compareToGolden (perturbed, golden).matches);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. polymeter-clockdiv
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/golden: polymetric lane clock divisions", "[determinism]")
{
    const auto schedule = playThenStop (goldenBpm, fourBarMusic);
    const auto bake =
        renderScenario (&configureDivisionPolymeter, schedule, goldenSampleRate, fourBarSpan, bakeBlockSize);

    REQUIRE (bake.numSamples == 430080);
    REQUIRE (bake.isSampleSorted ());
    REQUIRE (bake.events.size () == 128u); // 64 gated steps x (on + off)

    // ── THE LITERAL ──────────────────────────────────────────────────────────
    // GATE /1 all on ⇒ ordinal == step index. At step 7:
    //   PITCH index = floorMod (floorDiv (7, 3), 4) = 2 → -2
    //   degree      = 7 + (-2) = 5 → pool[5] = 69
    //   OCT index   = floorMod (floorDiv (7, 2), 3) = 0 → 0 semitones
    //   note        = 69 = 0x45, velocity 100 = 0x64, at 7 * 5760 = 40320.
    REQUIRE (eventIs (bake.events[0], 0, 0x90, 0x3C, 0x64));
    REQUIRE (containsEvent (bake, 40320, 0x90, 0x45, 0x64));
    REQUIRE (containsEvent (bake, 7 * stepSamples, 0x90, 0x45, 0x64));

    // ── THE §5.5 STOP FLUSH ────────────────────────────────────────────────
    // Nothing plays past the musical span, and the flush itself emits NOTHING:
    // this scenario's gates leave the sounding-note table already EMPTY at the
    // stop, and `SoundingNoteTable::flush` sweeps CC123 only on channels it
    // actually sounded on. So the stream balances because every note was
    // released on time, not because a flush rescued it — which is the stronger
    // statement. (tied-retrigger is the deliberate opposite: see that case.)
    REQUIRE (pitchesIn (bake, fourBarMusic, fourBarSpan).empty ());
    REQUIRE (bake.events.back ().absoluteSample < fourBarMusic);

    const auto check = checkGolden (bake, headerFor (bake, "polymeter-clockdiv", goldenBpm, goldenGridPpq));
    INFO (check.report);
    REQUIRE (check.passed);

    const auto golden = loadGolden ("polymeter-clockdiv");
    INFO (golden.error);
    REQUIRE (golden.ok);

    const auto sweep =
        sweepAgainstGolden (golden, &configureDivisionPolymeter, schedule, goldenSampleRate, fourBarSpan);
    INFO (sweep.report);
    REQUIRE (sweep.sizesChecked == numGoldenBlockSizes);
    REQUIRE (sweep.sizesMatched == numGoldenBlockSizes);
    REQUIRE (sweep.spansCorrect);
    REQUIRE (sweep.allSorted);
    REQUIRE (sweep.minNoteOns > 0); // gates the balance claim below — see SweepOutcome
    REQUIRE (sweep.allLifecyclesBalanced);
    REQUIRE (sweep.minEvents == sweep.maxEvents);
    REQUIRE (sweep.minEvents == 128);

    const auto perturbed =
        renderScenario (&configureDivisionPolymeterPerturbed, schedule, goldenSampleRate, fourBarSpan, bakeBlockSize);
    REQUIRE (! perturbed.empty ());
    REQUIRE (! compareToGolden (perturbed, golden).matches);
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. tied-retrigger  (the fractional clock: 137 BPM @ 44.1 kHz)
// ─────────────────────────────────────────────────────────────────────────────

// ── THIS GOLDEN FOUND ISSUE #46, AND IS NOW ITS PERMANENT GUARD ─────────────
// On its first bake this file matched at only 7 of the 10 swept block sizes.
// `SequencerProcessor::emitStep`'s still-sounding same-pitch retrigger branch
// used `juce::jmax (0, offset - 1)` on a WITHIN-BLOCK offset, so a retriggering
// note-on landing at offset 0 — exactly on a block head — collapsed the
// documented 1-sample gap and placed the note-off one sample late:
//
//   blocks 128…4096 → note-off @43455 (the gap)   — 7 of 10, and the baked value
//   blocks 32, 64   → note-off @43456; 43456 is a head at 32 and at 64
//   block 96        → first diverged @130368 vs @130367; a head at 96, not at 32/64
//
// That head-vs-not-head correlation is what identified the mechanism. The engine
// now decides the position on the ABSOLUTE timeline (`onSample - 1`, pre-flushing
// from the previous block when that sample lies there), so all ten agree — and the
// originally baked stream needed NO regeneration, because the 7-of-10 majority was
// already the correct performance.
//
// The same clamp existed in `flushForPatternSwitch` and was fixed with it.
//
// WHY sequencer_retrigger.cpp DID NOT CATCH IT: that suite runs a 50% gate, so the
// outgoing note-off is always ALREADY DUE and only the `isDueAtOrBefore` branch is
// ever taken. Reaching the buffer-size-dependent branch needs LEN > 100%, which is
// why this scenario exists and why it stays.

TEST_CASE ("determinism/golden: LEN tie/legato and same-pitch retrigger on a fractional clock", "[determinism]")
{
    const auto schedule = playThenStop (fractionalBpm, fractionalMusic);
    const auto bake =
        renderScenario (&configureTiedRetrigger, schedule, fractionalSampleRate, fractionalSpan, bakeBlockSize);

    REQUIRE (bake.numSamples == 675840);
    REQUIRE (bake.isSampleSorted ());
    REQUIRE (bake.events.size () == 257u); // 128 on + 128 off + one CC123 — see the flush check below

    // ── THE LITERAL, on the deliberately un-round clock ──────────────────────
    // A quarter note is 44100 * 60 / 137 = 19313.868… samples, so step 4 (PPQ 1.0)
    // lands at floor(19313.868…) = 19313 — a position no round-number arithmetic
    // could produce by accident. Step 4: ordinal 4 → pool[4] = 67 = 0x43;
    // PITCH[4 % 2 = 0] = 0; velocity 100 = 0x64.
    REQUIRE (eventIs (bake.events[0], 0, 0x90, 0x3C, 0x64));
    REQUIRE (containsEvent (bake, 19313, 0x90, 0x43, 0x64));

    // Both retrigger branches must actually be reachable, or the file is about
    // nothing: adjacent steps really do repeat a pitch.
    const auto firstEightPitches = pitchesIn (bake, 0, 8 * 4829);
    REQUIRE (firstEightPitches.size () >= 4u);
    REQUIRE (firstEightPitches[0] == firstEightPitches[1]);
    REQUIRE (firstEightPitches[2] == firstEightPitches[3]);
    REQUIRE (firstEightPitches[0] != firstEightPitches[2]);

    // ── THE §5.5 STOP FLUSH, WHICH IS WHY THIS STREAM BALANCES ───────────────
    // At `fractionalMusic` two 400%/150% notes are still sounding: step 125's
    // degree-4 pool[4] = 67 = 0x43 (due 622872) and step 127's degree-6 pool[6] =
    // 71 = 0x47 (due 620458). Without a flush point they would be note-ons with no
    // note-off — a truncation artifact frozen into the reference. The stop emits
    // both offs at the flush sample, and nothing plays afterwards.
    REQUIRE (containsEvent (bake, fractionalMusic, 0x80, 0x43, 0x00));
    REQUIRE (containsEvent (bake, fractionalMusic, 0x80, 0x47, 0x00));
    REQUIRE (pitchesIn (bake, fractionalMusic, fractionalSpan).empty ());

    // …and §5.5's "CC123 + per-note offs". `SoundingNoteTable::flush` sweeps ONLY
    // the channels it actually sounded on, so the CC123 appears here (the table was
    // non-empty) and is ABSENT from the other five goldens, whose 50% / euclidean
    // gates leave an empty table at their stop. That asymmetry is real §5.5
    // behaviour and is now frozen on both sides.
    REQUIRE (containsEvent (bake, fractionalMusic, 0xB0, 0x7B, 0x00));
    REQUIRE (bake.events.back ().absoluteSample == fractionalMusic);

    const auto check = checkGolden (bake, headerFor (bake, "tied-retrigger", fractionalBpm, goldenGridPpq));
    INFO (check.report);
    REQUIRE (check.passed);

    const auto golden = loadGolden ("tied-retrigger");
    INFO (golden.error);
    REQUIRE (golden.ok);

    const auto sweep =
        sweepAgainstGolden (golden, &configureTiedRetrigger, schedule, fractionalSampleRate, fractionalSpan);
    INFO (sweep.report);
    REQUIRE (sweep.sizesChecked == numGoldenBlockSizes);
    REQUIRE (sweep.spansCorrect);
    REQUIRE (sweep.allSorted);
    REQUIRE (sweep.minNoteOns > 0); // gates the balance claim below — see SweepOutcome
    REQUIRE (sweep.allLifecyclesBalanced);
    REQUIRE (sweep.minEvents == sweep.maxEvents);
    REQUIRE (sweep.minEvents == 257);

    const auto perturbed = renderScenario (&configureTiedRetriggerPerturbed,
                                           schedule,
                                           fractionalSampleRate,
                                           fractionalSpan,
                                           bakeBlockSize);
    REQUIRE (! perturbed.empty ());
    REQUIRE (! compareToGolden (perturbed, golden).matches);

    // ── THE ASSERTION THAT FOUND ISSUE #46 — see the banner above ────────────
    // It reported 7 of 10 on this file's first bake and has reported 10 since the
    // engine started placing the off at absolute `onSample - 1`. Kept LAST so that
    // everything else in this case (the literals, the golden comparison at the bake
    // size, the perturbed negative control) is still verified first if it ever
    // reddens again — and if it does, THAT IS A FINDING, NOT A TEST TO RELAX.
    REQUIRE (sweep.sizesMatched == numGoldenBlockSizes);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. direction-modes-cycle
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/golden: the nine deterministic direction modes, one per bar", "[determinism]")
{
    const auto schedule = directionCycleSchedule ();
    const auto bake = renderScenario (&configureDirectionCycle, schedule, goldenSampleRate, tenBarSpan, bakeBlockSize);

    REQUIRE (bake.numSamples == 983040);
    REQUIRE (bake.isSampleSorted ());
    REQUIRE (bake.events.size () == 320u); // 160 gated steps x (on + off)

    // ── THE LITERAL ──────────────────────────────────────────────────────────
    // Bar 1 is pattern 0 (`up`): step 0 → pool[0] = 60 = 0x3C at velocity 100.
    // Bar 2 is pattern 1 (`down`, order[k] = 7 - k) and starts at step 16 =
    // 92160 samples: ordinal 16, k = 16 % 8 = 0 → pool[7] = 72 = 0x48, velocity
    // 101 = 0x65.
    REQUIRE (eventIs (bake.events[0], 0, 0x90, 0x3C, 0x64));
    REQUIRE (containsEvent (bake, barSamples, 0x90, 0x48, 0x65));
    REQUIRE (containsEvent (bake, 92160, 0x90, 0x48, 0x65));

    // ── THE FREE INTERNAL-CONSISTENCY CHECK ──────────────────────────────────
    // Bar 10 switched back to pattern 0. Step 144 % 8 == 0, so its pitch sequence
    // must equal bar 1's — nothing may have accumulated across eight switches.
    const auto barOne = pitchesIn (bake, 0, barSamples);
    const auto barTen = pitchesIn (bake, 9 * barSamples, 10 * barSamples);
    INFO ("bar 1 has " << barOne.size () << " note-ons, bar 10 has " << barTen.size ());
    REQUIRE (barOne.size () == 16u);
    REQUIRE (barTen == barOne);

    // …and the anti-vacuity for THAT check: the intervening bars are not all the
    // same sequence, or "bar 10 == bar 1" would be trivially true.
    const auto barTwo = pitchesIn (bake, barSamples, 2 * barSamples);
    REQUIRE (barTwo.size () == 16u);
    REQUIRE (barTwo != barOne);

    // ── THE §5.5 STOP FLUSH ────────────────────────────────────────────────
    // Nothing plays past the musical span, and the flush itself emits NOTHING:
    // this scenario's gates leave the sounding-note table already EMPTY at the
    // stop, and `SoundingNoteTable::flush` sweeps CC123 only on channels it
    // actually sounded on. So the stream balances because every note was
    // released on time, not because a flush rescued it — which is the stronger
    // statement. (tied-retrigger is the deliberate opposite: see that case.)
    REQUIRE (pitchesIn (bake, tenBarMusic, tenBarSpan).empty ());
    REQUIRE (bake.events.back ().absoluteSample < tenBarMusic);

    const auto check = checkGolden (bake, headerFor (bake, "direction-modes-cycle", goldenBpm, goldenGridPpq));
    INFO (check.report);
    REQUIRE (check.passed);

    const auto golden = loadGolden ("direction-modes-cycle");
    INFO (golden.error);
    REQUIRE (golden.ok);

    const auto sweep = sweepAgainstGolden (golden, &configureDirectionCycle, schedule, goldenSampleRate, tenBarSpan);
    INFO (sweep.report);
    REQUIRE (sweep.sizesChecked == numGoldenBlockSizes);
    REQUIRE (sweep.sizesMatched == numGoldenBlockSizes);
    REQUIRE (sweep.spansCorrect);
    REQUIRE (sweep.allSorted);
    REQUIRE (sweep.minNoteOns > 0); // gates the balance claim below — see SweepOutcome
    REQUIRE (sweep.allLifecyclesBalanced);
    REQUIRE (sweep.minEvents == sweep.maxEvents);
    REQUIRE (sweep.minEvents == 320);

    const auto perturbed =
        renderScenario (&configureDirectionCyclePerturbed, schedule, goldenSampleRate, tenBarSpan, bakeBlockSize);
    REQUIRE (! perturbed.empty ());
    REQUIRE (! compareToGolden (perturbed, golden).matches);
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. euclid-gate
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/golden: euclidean gate necklaces E(5,8), E(7,16,3) and E(3,8,2)", "[determinism]")
{
    const auto schedule = euclidSchedule ();
    const auto bake = renderScenario (&configureEuclid, schedule, goldenSampleRate, fourBarSpan, bakeBlockSize);

    REQUIRE (bake.numSamples == 430080);
    REQUIRE (bake.isSampleSorted ());
    REQUIRE (bake.events.size () == 66u); // 10 + 7 + 6 + 10 onsets x (on + off)

    // ── THE LITERAL ──────────────────────────────────────────────────────────
    // E(5,8,0) = x.x.xx.x fires on step 0: pool[0] = 60 = 0x3C at velocity 100.
    // Bar 2 is E(7,16,3) = .x.x..x.x.x..x.x, which RESTS on its step 0 (= global
    // step 16) and fires on step 17. Its gate cycle is 16 steps with 7 pulses, so
    // the ordinal at step 17 is 1 * 7 + prefix[1] = 7 → pool[7] = 72 = 0x48, at
    // velocity 110 = 0x6E and 17 * 5760 = 97920.
    REQUIRE (eventIs (bake.events[0], 0, 0x90, 0x3C, 0x64));
    REQUIRE (! containsEvent (bake, barSamples, 0x90, 0x48, 0x6E)); // step 16 is a rest
    REQUIRE (containsEvent (bake, 97920, 0x90, 0x48, 0x6E));
    REQUIRE (containsEvent (bake, 17 * stepSamples, 0x90, 0x48, 0x6E));

    // ── THE §5.5 STOP FLUSH ────────────────────────────────────────────────
    // Nothing plays past the musical span, and the flush itself emits NOTHING:
    // this scenario's gates leave the sounding-note table already EMPTY at the
    // stop, and `SoundingNoteTable::flush` sweeps CC123 only on channels it
    // actually sounded on. So the stream balances because every note was
    // released on time, not because a flush rescued it — which is the stronger
    // statement. (tied-retrigger is the deliberate opposite: see that case.)
    REQUIRE (pitchesIn (bake, fourBarMusic, fourBarSpan).empty ());
    REQUIRE (bake.events.back ().absoluteSample < fourBarMusic);

    const auto check = checkGolden (bake, headerFor (bake, "euclid-gate", goldenBpm, goldenGridPpq));
    INFO (check.report);
    REQUIRE (check.passed);

    const auto golden = loadGolden ("euclid-gate");
    INFO (golden.error);
    REQUIRE (golden.ok);

    const auto sweep = sweepAgainstGolden (golden, &configureEuclid, schedule, goldenSampleRate, fourBarSpan);
    INFO (sweep.report);
    REQUIRE (sweep.sizesChecked == numGoldenBlockSizes);
    REQUIRE (sweep.sizesMatched == numGoldenBlockSizes);
    REQUIRE (sweep.spansCorrect);
    REQUIRE (sweep.allSorted);
    REQUIRE (sweep.minNoteOns > 0); // gates the balance claim below — see SweepOutcome
    REQUIRE (sweep.allLifecyclesBalanced);
    REQUIRE (sweep.minEvents == sweep.maxEvents);
    REQUIRE (sweep.minEvents == 66);

    const auto perturbed =
        renderScenario (&configureEuclidPerturbed, schedule, goldenSampleRate, fourBarSpan, bakeBlockSize);
    REQUIRE (! perturbed.empty ());
    REQUIRE (! compareToGolden (perturbed, golden).matches);
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. cond-a-b-8loops
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/golden: A:B trig conditions over eight pattern loops", "[determinism]")
{
    const auto schedule = playThenStop (goldenBpm, eightBarMusic);
    const auto bake =
        renderScenario (&configureConditionCycle, schedule, goldenSampleRate, eightBarSpan, bakeBlockSize);

    REQUIRE (bake.numSamples == 798720);
    REQUIRE (bake.isSampleSorted ());

    // ── THE COUNT, FROM THE FOUR CONDITIONS ──────────────────────────────────
    // 3:4 twice + 1:2 four times + 2:2 four times + 1:8 once = 11 note-ons, each with
    // its own note-off (LEN 50 % ⇒ 2880 samples, all inside the span), and the table is
    // therefore EMPTY at the stop so the flush emits nothing at all.
    REQUIRE (bake.events.size () == 22u);
    REQUIRE (noteOnCount (bake) == 11u);

    // ── THE ACCEPTANCE CRITERION, AS TWO ABSOLUTE SAMPLES ────────────────────
    // Velocity 100 (0x64) is step 0, the `3:4` step. Its two note-ons are at loop 2
    // and loop 6: 2 x 92160 = 184320 and 6 x 92160 = 552960. DIVIDE BY 92160 AND READ
    // THE LOOP — that is the whole assertion, and it is why the canonical clock's bar
    // is a round number.
    //
    // The Phase-7 plan predicted 184320 and 553920 for these. 553920 is not a multiple
    // of 5760 at all (553920 / 5760 = 96.1666…), so it cannot be a step boundary; the
    // plan's own stated check — "divide by 92160" — gives 6.0104 for it and exactly 6
    // for 552960. It was a typo, and 552960 is the value the engine produces.
    const auto threeOfFour = onsetsWithVelocity (bake, 100);
    INFO ("3:4 fired at " << (threeOfFour.empty () ? juce::String ("nothing") : juce::String (threeOfFour.front ())));
    REQUIRE (threeOfFour == std::vector<std::int64_t> { 184320, 552960 });
    REQUIRE (184320 == 2 * barSamples);
    REQUIRE (552960 == 6 * barSamples);

    // …and as RAW BYTES, so a corrupted file and a corrupted renderer cannot agree.
    // Step 0 of loop 2 has gated ordinal 8 (four gated steps per loop, two loops
    // elapsed) ⇒ pool[8 % 8] = pool[0] = 60 = 0x3C at velocity 100 = 0x64. That the
    // ordinal counted the SUPPRESSED steps is the gated-cursor rule, visible in one
    // byte.
    REQUIRE (containsEvent (bake, 184320, 0x90, 0x3C, 0x64));
    REQUIRE (containsEvent (bake, 552960, 0x90, 0x3C, 0x64));
    REQUIRE (containsEvent (bake, 184320 + 2880, 0x80, 0x3C, 0x00));

    // ── THE OTHER THREE CONDITIONS ───────────────────────────────────────────
    // 1:2 (velocity 104) on the even loops, 2:2 (108) on the odd ones, 1:8 (112) once.
    REQUIRE (onsetsWithVelocity (bake, 104) == std::vector<std::int64_t> { 0 * barSamples + 4 * stepSamples,
                                                                           2 * barSamples + 4 * stepSamples,
                                                                           4 * barSamples + 4 * stepSamples,
                                                                           6 * barSamples + 4 * stepSamples });
    REQUIRE (onsetsWithVelocity (bake, 108) == std::vector<std::int64_t> { 1 * barSamples + 8 * stepSamples,
                                                                           3 * barSamples + 8 * stepSamples,
                                                                           5 * barSamples + 8 * stepSamples,
                                                                           7 * barSamples + 8 * stepSamples });
    REQUIRE (onsetsWithVelocity (bake, 112) == std::vector<std::int64_t> { 12 * stepSamples });

    // …and the free internal-consistency check: `1:2` and `2:2` are COMPLEMENTARY, so
    // between them they must fire exactly once per loop, eight times over.
    REQUIRE (onsetsWithVelocity (bake, 104).size () + onsetsWithVelocity (bake, 108).size () == 8u);

    // Exactly four velocities appear — the four gated phases — so no ungated step
    // leaked into the file and no conditioned step was silently skipped entirely.
    REQUIRE (velocitiesIn (bake) == std::vector<int> { 100, 104, 108, 112 });

    // ── THE §5.5 STOP FLUSH ──────────────────────────────────────────────────
    // Nothing plays past the musical span and the flush emits nothing (the table is
    // already empty), so this stream balances because every note was released on time.
    REQUIRE (pitchesIn (bake, eightBarMusic, eightBarSpan).empty ());
    REQUIRE (bake.events.back ().absoluteSample < eightBarMusic);

    const auto check =
        checkGolden (bake, headerFor (bake, "cond-a-b-8loops", goldenBpm, goldenGridPpq, phase7RngVersion));
    INFO (check.report);
    REQUIRE (check.passed);

    const auto golden = loadGolden ("cond-a-b-8loops");
    INFO (golden.error);
    REQUIRE (golden.ok);
    REQUIRE (golden.header.rngVersion == phase7RngVersion);

    const auto sweep = sweepAgainstGolden (golden, &configureConditionCycle, schedule, goldenSampleRate, eightBarSpan);
    INFO (sweep.report);
    REQUIRE (sweep.sizesChecked == numGoldenBlockSizes);
    REQUIRE (sweep.sizesMatched == numGoldenBlockSizes);
    REQUIRE (sweep.spansCorrect);
    REQUIRE (sweep.allSorted);
    REQUIRE (sweep.minNoteOns > 0);
    REQUIRE (sweep.allLifecyclesBalanced);
    REQUIRE (sweep.minEvents == sweep.maxEvents);
    REQUIRE (sweep.minEvents == 22);

    // ── THE PERTURBED-RENDER NEGATIVE CONTROL: `3:4` becomes `4:4` ───────────
    const auto perturbed =
        renderScenario (&configureConditionCyclePerturbed, schedule, goldenSampleRate, eightBarSpan, bakeBlockSize);
    REQUIRE (perturbed.events.size () == bake.events.size ()); // same shape…
    REQUIRE (! compareToGolden (perturbed, golden).matches);   // …one loop later

    // …and specifically ONE LOOP LATER, which is the off-by-one a missing `a - 1`
    // would have baked in permanently.
    REQUIRE (onsetsWithVelocity (perturbed, 100) == std::vector<std::int64_t> { 3 * barSamples, 7 * barSamples });
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. cond-pre-chain
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/golden: a ten-long PRE chain truncating at the depth-8 boundary", "[determinism]")
{
    const auto schedule = playThenStop (goldenBpm, eightBarMusic);
    const auto bake = renderScenario (&configurePreChain, schedule, goldenSampleRate, eightBarSpan, bakeBlockSize);

    REQUIRE (bake.numSamples == 798720);
    REQUIRE (bake.isSampleSorted ());

    // ── D6's TRUNCATION, FROZEN AS TWO MISSING BYTES ─────────────────────────
    // Velocity names the step, so steps 9 and 10 — the two PRE links whose backward
    // walk exhausts `maxPreChainDepth` before reaching the anchor — must contribute
    // velocity 109 (0x6D) and 110 (0x6E) to NO event in this file, in any loop,
    // whatever the anchor rolled. Raising the budget, or changing D6's base case from
    // `false` to `true`, adds them.
    REQUIRE (onsetsWithVelocity (bake, 109).empty ());
    REQUIRE (onsetsWithVelocity (bake, 110).empty ());

    // …and the LAST ANCHORED link, step 8 (velocity 108 = 0x6C), IS present. Without
    // this the assertion above would also be satisfied by a chain that truncated at
    // depth 1, i.e. by PRE not working at all.
    REQUIRE (! onsetsWithVelocity (bake, 108).empty ());

    // The boundary is therefore pinned from BOTH sides, and it is exactly where
    // `maxPreChainDepth` puts it.
    REQUIRE (arpbox::engine::maxPreChainDepth == 8);
    REQUIRE (preChainLength == 10);

    // ── THE ANCHOR'S PROBABILITY COUNTS ──────────────────────────────────────
    // Step 0 sits at PROB 50 and every link at 100, so the anchor's roll alone decides
    // whether a loop's chain sounds. The roll is keyed on the GLOBAL step index, so the
    // answer differs loop to loop — and both outcomes must occur in the file, or the
    // semantic is frozen only on one side.
    const auto anchors = onsetsWithVelocity (bake, 100);       // step 0 of each loop
    const auto lastLink = onsetsWithVelocity (bake, 108);      // step 8 of each loop
    const auto unconditioned = onsetsWithVelocity (bake, 111); // step 11: COND none, PROB 100

    INFO ("anchors fired in " << anchors.size () << " of 8 loops; last link in " << lastLink.size ()
                              << "; unconditioned in " << unconditioned.size ());

    REQUIRE (unconditioned.size () == 8u);         // every loop, unconditionally — the control
    REQUIRE (anchors.size () < 8u);                // the anchor's roll REJECTED at least one loop
    REQUIRE (! anchors.empty ());                  // …and accepted at least one
    REQUIRE (lastLink.size () == anchors.size ()); // the chain follows the anchor exactly

    // …and it follows it LOOP BY LOOP, not merely in aggregate: step 8 of a loop sounds
    // in exactly the loops whose step 0 sounded, eight steps later. A cache that
    // carried one loop's verdict into the next would satisfy the counts above and fail
    // this.
    std::vector<std::int64_t> expectedLastLink;
    expectedLastLink.reserve (anchors.size ());
    for (const auto anchor : anchors)
        expectedLastLink.push_back (anchor + 8 * stepSamples);

    REQUIRE (lastLink == expectedLastLink);

    // ── THE §5.5 STOP FLUSH ──────────────────────────────────────────────────
    REQUIRE (pitchesIn (bake, eightBarMusic, eightBarSpan).empty ());

    const auto check =
        checkGolden (bake, headerFor (bake, "cond-pre-chain", goldenBpm, goldenGridPpq, phase7RngVersion));
    INFO (check.report);
    REQUIRE (check.passed);

    const auto golden = loadGolden ("cond-pre-chain");
    INFO (golden.error);
    REQUIRE (golden.ok);
    REQUIRE (golden.header.rngVersion == phase7RngVersion);

    const auto sweep = sweepAgainstGolden (golden, &configurePreChain, schedule, goldenSampleRate, eightBarSpan);
    INFO (sweep.report);
    REQUIRE (sweep.sizesChecked == numGoldenBlockSizes);
    REQUIRE (sweep.sizesMatched == numGoldenBlockSizes);
    REQUIRE (sweep.spansCorrect);
    REQUIRE (sweep.allSorted);
    REQUIRE (sweep.minNoteOns > 0);
    REQUIRE (sweep.allLifecyclesBalanced);
    REQUIRE (sweep.minEvents == sweep.maxEvents);

    // ── THE NEGATIVE CONTROL: the ANCHOR's condition inverted ───────────────
    const auto perturbed =
        renderScenario (&configurePreChainPerturbed, schedule, goldenSampleRate, eightBarSpan, bakeBlockSize);
    REQUIRE (! perturbed.empty ());
    REQUIRE (! compareToGolden (perturbed, golden).matches);

    // …and the truncation survives the perturbation, because it is a property of the
    // BUDGET rather than of the anchor.
    REQUIRE (onsetsWithVelocity (perturbed, 109).empty ());
    REQUIRE (onsetsWithVelocity (perturbed, 110).empty ());
}

// ─────────────────────────────────────────────────────────────────────────────
// 9. prob-seeded-50
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/golden: PROB 50 at a fixed master seed", "[determinism]")
{
    const auto schedule = playThenStop (goldenBpm, fourBarMusic);
    const auto bake =
        renderScenario (&configureSeededProbability, schedule, goldenSampleRate, fourBarSpan, bakeBlockSize);

    REQUIRE (bake.numSamples == 430080);
    REQUIRE (bake.isSampleSorted ());

    // ── ANTI-VACUITY: THE ROLL ACTUALLY ROLLED ───────────────────────────────
    // 64 gated steps at PROB 50. Neither 0 nor 64 would be a probability — the first
    // is a lane read that never happened, the second is the `>= 100` short-circuit
    // being taken when it must not be.
    const auto fired = noteOnCount (bake);
    INFO ("PROB 50 fired " << fired << " of 64 gated steps");
    REQUIRE (fired > 16u);
    REQUIRE (fired < 48u);
    REQUIRE (bake.events.size () == 2u * fired); // one off per on: no flush was needed

    // ── "LOOP-STABLE" IN THE DETERMINISM SENSE, NOT THE LOOP LOCK SENSE ──────
    // The roll is keyed on the GLOBAL step index, so bar 2's rhythm must DIFFER from
    // bar 1's. Literal per-loop repetition is Phase 12's LOOP LOCK, and baking it here
    // would make the wrong behaviour the reference.
    std::vector<std::int64_t> barOneOffsets;
    std::vector<std::int64_t> barTwoOffsets;

    for (const auto& event : bake.events)
        if (event.message.isNoteOn ())
        {
            if (event.absoluteSample < barSamples)
                barOneOffsets.push_back (event.absoluteSample);
            else if (event.absoluteSample < 2 * barSamples)
                barTwoOffsets.push_back (event.absoluteSample - barSamples);
        }

    INFO ("bar 1 fired " << barOneOffsets.size () << " steps, bar 2 fired " << barTwoOffsets.size ());
    REQUIRE (! barOneOffsets.empty ());
    REQUIRE (! barTwoOffsets.empty ());
    REQUIRE (barOneOffsets != barTwoOffsets);

    // Every onset is still exactly on its grid boundary — PROB changes WHETHER a step
    // fires, never WHEN.
    int onGrid = 0;
    for (const auto& event : bake.events)
        if (event.message.isNoteOn () && event.absoluteSample % stepSamples == 0)
            ++onGrid;

    REQUIRE (static_cast<std::size_t> (onGrid) == fired);

    const auto check =
        checkGolden (bake, headerFor (bake, "prob-seeded-50", goldenBpm, goldenGridPpq, phase7RngVersion));
    INFO (check.report);
    REQUIRE (check.passed);

    const auto golden = loadGolden ("prob-seeded-50");
    INFO (golden.error);
    REQUIRE (golden.ok);
    REQUIRE (golden.header.rngVersion == phase7RngVersion);

    const auto sweep =
        sweepAgainstGolden (golden, &configureSeededProbability, schedule, goldenSampleRate, fourBarSpan);
    INFO (sweep.report);
    REQUIRE (sweep.sizesChecked == numGoldenBlockSizes);
    REQUIRE (sweep.sizesMatched == numGoldenBlockSizes);
    REQUIRE (sweep.spansCorrect);
    REQUIRE (sweep.allSorted);
    REQUIRE (sweep.minNoteOns > 0);
    REQUIRE (sweep.allLifecyclesBalanced);
    REQUIRE (sweep.minEvents == sweep.maxEvents);

    // ── THE NEGATIVE CONTROL: THE SEED, one bit ─────────────────────────────
    const auto perturbed =
        renderScenario (&configureSeededProbabilityPerturbed, schedule, goldenSampleRate, fourBarSpan, bakeBlockSize);
    REQUIRE (! perturbed.empty ());
    REQUIRE (! compareToGolden (perturbed, golden).matches);
}

// ─────────────────────────────────────────────────────────────────────────────
// 10. ratchet-ramp-8
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/golden: RATCHET 1..8 with a -50% velocity ramp", "[determinism]")
{
    const auto schedule = playThenStop (goldenBpm, fourBarMusic);
    const auto bake = renderScenario (&configureRatchetRamp, schedule, goldenSampleRate, fourBarSpan, bakeBlockSize);

    REQUIRE (bake.numSamples == 430080);
    REQUIRE (bake.isSampleSorted ());

    // ── THE COUNT, FROM THE LANE ─────────────────────────────────────────────
    // The RATCHET lane is {1,2,3,4,5,6,7,8}, so eight consecutive steps carry
    // 1+2+…+8 = 36 notes. 64 gated steps = 8 whole cycles = 288 note-ons.
    REQUIRE (noteOnCount (bake) == 288u);
    REQUIRE (8 * (1 + 2 + 3 + 4 + 5 + 6 + 7 + 8) == 288);

    // ── STEP 6: SEVEN CHILDREN ON A SLOT OF 822.857… SAMPLES ────────────────
    // THE ROW THIS GOLDEN EXISTS FOR. Each child's position is derived independently
    // from the parent (`child / 7`) and placed by the one snap-then-FLOOR, so the seven
    // onsets are `34560 + {0, 822, 1645, 2468, 3291, 4114, 4937}`. Multiply 5760 by
    // c/7 on a calculator and floor it; the numbers are checkable by hand, which is the
    // point.
    //
    // Cumulative addition of a rounded 823-sample slot would give
    // {0, 823, 1646, 2469, 3292, 4115, 4938} — the last three drifting. The Phase-7
    // plan's own predicted row, {0, 823, 1646, 2469, 3291, 4114, 4937}, assumed
    // ROUND-TO-NEAREST placement; the engine floors, so its middle three entries are
    // one sample high. What is frozen here is the engine.
    constexpr std::int64_t stepSixOnset = 6 * stepSamples; // 34560
    REQUIRE (stepSixOnset == 34560);

    const std::int64_t sevenChildOffsets[7] = { 0, 822, 1645, 2468, 3291, 4114, 4937 };

    int childrenFound = 0;
    for (int child = 0; child < 7; ++child)
    {
        // Independently derived here, from the arithmetic rather than from the engine.
        const auto predicted =
            static_cast<std::int64_t> (static_cast<double> (child) / 7.0 * static_cast<double> (stepSamples));

        if (predicted == sevenChildOffsets[child])
            ++childrenFound;
    }

    INFO ("hand-derived child offsets agreeing with the literals: " << childrenFound << " of 7");
    REQUIRE (childrenFound == 7);

    // Step 6's gated ordinal is 6 ⇒ pool[6] = 71 = 0x47. The ramp is -50 % over
    // `count - 1 = 6`, so velocity c is llround (100 * (1 - 0.5 * c / 6)):
    // 100, 92, 83, 75, 67, 58, 50 — 0x64, 0x5C, 0x53, 0x4B, 0x43, 0x3A, 0x32.
    const int rampedVelocities[7] = { 100, 92, 83, 75, 67, 58, 50 };

    int rowFound = 0;
    for (int child = 0; child < 7; ++child)
        if (containsEvent (bake, stepSixOnset + sevenChildOffsets[child], 0x90, 0x47, rampedVelocities[child]))
            ++rowFound;

    INFO ("step 6's seven children found: " << rowFound << " of 7");
    REQUIRE (rowFound == 7);

    // The two ends of the ramp, spelled out as raw bytes.
    REQUIRE (containsEvent (bake, 34560, 0x90, 0x47, 0x64));        // child 0: VEL untouched
    REQUIRE (containsEvent (bake, 34560 + 4937, 0x90, 0x47, 0x32)); // child 6: exactly half

    // …and the ramp really is a RAMP: seven distinct velocities on one step.
    REQUIRE (velocitiesIn (bake).size () >= 7u);

    // Step 0 has ONE child, so it is bit-identical in shape to a pre-7.2 step: the
    // ramp's `childCount <= 1` short-circuit returns the step's own VEL untouched.
    REQUIRE (eventIs (bake.events[0], 0, 0x90, 0x3C, 0x64));

    REQUIRE (pitchesIn (bake, fourBarMusic, fourBarSpan).empty ());

    const auto check =
        checkGolden (bake, headerFor (bake, "ratchet-ramp-8", goldenBpm, goldenGridPpq, phase7RngVersion));
    INFO (check.report);
    REQUIRE (check.passed);

    const auto golden = loadGolden ("ratchet-ramp-8");
    INFO (golden.error);
    REQUIRE (golden.ok);
    REQUIRE (golden.header.rngVersion == phase7RngVersion);

    const auto sweep = sweepAgainstGolden (golden, &configureRatchetRamp, schedule, goldenSampleRate, fourBarSpan);
    INFO (sweep.report);
    REQUIRE (sweep.sizesChecked == numGoldenBlockSizes);
    REQUIRE (sweep.sizesMatched == numGoldenBlockSizes);
    REQUIRE (sweep.spansCorrect);
    REQUIRE (sweep.allSorted);
    REQUIRE (sweep.minNoteOns > 0);
    REQUIRE (sweep.allLifecyclesBalanced);
    REQUIRE (sweep.minEvents == sweep.maxEvents);

    // ── THE NEGATIVE CONTROL: seven children become eight ───────────────────
    const auto perturbed =
        renderScenario (&configureRatchetRampPerturbed, schedule, goldenSampleRate, fourBarSpan, bakeBlockSize);
    REQUIRE (! perturbed.empty ());
    REQUIRE (! compareToGolden (perturbed, golden).matches);
    REQUIRE (noteOnCount (perturbed) == 288u + 8u); // one extra child on each of eight steps
}

// ─────────────────────────────────────────────────────────────────────────────
// 11. micro-swing-compose
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/golden: MICRO composed with swing, including the clamp", "[determinism]")
{
    const auto schedule = playThenStop (goldenBpm, fourBarMusic);
    const auto bake = renderScenario (&configureMicroSwing, schedule, goldenSampleRate, fourBarSpan, bakeBlockSize);

    REQUIRE (bake.numSamples == 430080);
    REQUIRE (bake.isSampleSorted ());
    REQUIRE (noteOnCount (bake) == 64u); // RATCHET stays at 1: one note per gated step

    // ── THE FOUR DISPLACEMENTS, AS ABSOLUTE SAMPLES ─────────────────────────
    // Step 0 (MICRO +25, even ⇒ +0.25 step) at 0 + 1440. Step 1 (MICRO -25, odd
    // ⇒ -0.25 + 0.5 = +0.25) at 5760 + 1440 = 7200 — the SAME displacement by a
    // different route. Step 2 (MICRO -50, even ⇒ -0.5) at 11520 - 2880 = 8640, the
    // maximum EARLY shift. Step 3 (MICRO +25, odd ⇒ 0.75 CLAMPED to 0.5) at
    // 17280 + 2880 = 20160.
    REQUIRE (containsEvent (bake, 1440, 0x90, 0x3C, 0x64));
    REQUIRE (containsEvent (bake, 7200, 0x90, 0x3E, 0x64));
    REQUIRE (containsEvent (bake, 8640, 0x90, 0x40, 0x64));
    REQUIRE (containsEvent (bake, 20160, 0x90, 0x41, 0x64));

    // ── THE SATURATION, VISIBLE IN THE FILE ─────────────────────────────────
    // Step 3's raw composition is 0.75 of a step. An engine that clamped PER SOURCE
    // (MICRO to ±0.5, swing to +0.5) would admit the sum and place it at
    // 17280 + 4320 = 21600. The clamp is on the TOTAL, so it is at 20160 and 21600
    // carries nothing. This is the assertion that a per-source clamp fails — and a
    // per-source clamp is what breaks `stepScanBack`/`stepScanForward`'s derivation.
    REQUIRE (containsEvent (bake, 17280 + 2880, 0x90, 0x41, 0x64));
    REQUIRE (! containsEvent (bake, 17280 + 4320, 0x90, 0x41, 0x64));

    // …and steps 0 and 3 carry the IDENTICAL MICRO value (+25) yet sit 1440 samples
    // apart from their grids, which is the swing term made visible.
    REQUIRE (microLane[0] == microLane[3]);
    REQUIRE ((20160 - 17280) - (1440 - 0) == 1440);

    // Nothing sits ON its grid boundary except by composition: with these four MICRO
    // values every step is displaced, so a swing/MICRO term dropped entirely would put
    // 64 note-ons back on multiples of 5760.
    int onGrid = 0;
    for (const auto& event : bake.events)
        if (event.message.isNoteOn () && event.absoluteSample % stepSamples == 0)
            ++onGrid;

    INFO ("note-ons landing exactly on a grid boundary: " << onGrid);
    REQUIRE (onGrid == 0);

    const auto check =
        checkGolden (bake, headerFor (bake, "micro-swing-compose", goldenBpm, goldenGridPpq, phase7RngVersion));
    INFO (check.report);
    REQUIRE (check.passed);

    const auto golden = loadGolden ("micro-swing-compose");
    INFO (golden.error);
    REQUIRE (golden.ok);
    REQUIRE (golden.header.rngVersion == phase7RngVersion);

    const auto sweep = sweepAgainstGolden (golden, &configureMicroSwing, schedule, goldenSampleRate, fourBarSpan);
    INFO (sweep.report);
    REQUIRE (sweep.sizesChecked == numGoldenBlockSizes);
    REQUIRE (sweep.sizesMatched == numGoldenBlockSizes);
    REQUIRE (sweep.spansCorrect);
    REQUIRE (sweep.allSorted);
    REQUIRE (sweep.minNoteOns > 0);
    REQUIRE (sweep.allLifecyclesBalanced);
    REQUIRE (sweep.minEvents == sweep.maxEvents);

    // ── THE NEGATIVE CONTROL: swing 75 -> 74, MICRO untouched ───────────────
    const auto perturbed =
        renderScenario (&configureMicroSwingPerturbed, schedule, goldenSampleRate, fourBarSpan, bakeBlockSize);
    REQUIRE (perturbed.events.size () == bake.events.size ());
    REQUIRE (! compareToGolden (perturbed, golden).matches);

    // The EVEN steps must not have moved: swing displaces odd steps only.
    REQUIRE (containsEvent (perturbed, 1440, 0x90, 0x3C, 0x64));
    REQUIRE (containsEvent (perturbed, 8640, 0x90, 0x40, 0x64));
}

// ─────────────────────────────────────────────────────────────────────────────
// 12. ratchet-swing-retrigger  (the fractional clock: 137 BPM @ 44.1 kHz)
// ─────────────────────────────────────────────────────────────────────────────

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ THIS GOLDEN FOUND A DEFECT ON ITS FIRST BAKE, EXACTLY AS `tied-retrigger`  ║
// ║ FOUND #46. THE FILE ON DISK IS THE CORRECT PERFORMANCE — DO NOT REGENERATE.║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// MEASURED: the baked stream matches at 5 of the 10 swept block sizes.
//
//   blocks 32, 64, 96, 128, 256        note-off @602 — the correct value, and the baked one
//   blocks 480, 512, 1024, 2048, 4096  the SAME note-off @337
//
// The note in question is STEP -1's CHILD 6, at sample 337 (see the step -1 note in the
// case below). @602 is `next same-pitch onset - 1`, i.e. §5.5's 1-sample gap against
// step 0's child 1 at 603. @337 is the note's OWN ONSET — a zero-length note.
//
// THE ROOT CAUSE, and it is a new instance of the #36/#46/#48 family rather than the
// rounding drift this file was aimed at: THE WALK EMITS IN INDEX ORDER, AND INDEX ORDER
// IS NOT SAMPLE ORDER UNDER DISPLACEMENT. The walk visits index -1 before index 0, so
// step -1's child 6 (sample 337) is REGISTERED IN THE TABLE BEFORE step 0's child 0
// (sample 0) — but only when both fall in the same block. When step 0's note is then
// emitted, `emitNote`'s same-pitch branch finds that entry and retires it:
//
//     capSample = isDueAtOrBefore (existing, onSample) ? onSample : onSample - 1;
//     sounding.retireNoLaterThan (existing, midi, capSample, …);
//
// with `onSample == 0`, so `capSample == -1`, and the table's placement rule floors it
// at the entry's own onset (`jmax (entry.onSample, cap)`) — 337. At block 128 the two
// notes land in DIFFERENT blocks, the note at 0 has already been retired by the time 337
// is registered, and the entry keeps the cutoff `cutoffForSamePitch` correctly computed
// for it. Same music, different bytes, decided by the device buffer size.
//
// `Entry::onSample` and the `jmax` floor were ADDED in Phase 7.2 for precisely this
// situation, and they do prevent an inverted off/on pair — but they turn it into a
// buffer-size-dependent note LENGTH instead of preventing it.
//
// SUGGESTED FIX (engine, `generative-seq-dev`): the same-pitch branch in `emitNote` must
// only retire an entry that STARTED AT OR BEFORE this note. An entry starting LATER is
// not "the note this one is retriggering" — it is a note that has not sounded yet, and
// the incoming (earlier) note's own `cutoffForSamePitch` has already scheduled itself to
// end before it (that scan looks backward as well as forward and takes the minimum
// qualifying onset, which is exactly why block 128 gets it right). So:
//
//     if (const int existing = sounding.find (channel, note);
//         existing >= 0 && sounding.onSampleOf (existing) <= onSample)
//
// `SoundingNoteTable` would need to expose the entry's onset; it already stores it.
//
// WHY THE GOLDEN IS NOT REGENERATED: @602 is the documented policy (§5.5's overlap rule)
// and the 5-of-10 majority is the correct performance, exactly as #46's 7-of-10 was.
// Rule zero — a golden diff is a FINDING, never something to silently regenerate.
TEST_CASE ("determinism/golden: eight ratchet children retriggering under swing on a fractional clock", "[determinism]")
{
    const auto schedule = playThenStop (fractionalBpm, ratchetFractionalMusic);
    const auto bake = renderScenario (&configureRatchetSwingRetrigger,
                                      schedule,
                                      fractionalSampleRate,
                                      ratchetFractionalSpan,
                                      bakeBlockSize);

    REQUIRE (bake.numSamples == 184320);
    REQUIRE (bake.isSampleSorted ());

    // ── THE RETRIGGER CAP MUST BIND, OR THE FILE TESTS NOTHING ──────────────
    // Every note in this render is the same pitch, so §5.5's 1-sample gap is visible
    // as "this note-off sits exactly one sample before some note-on". At the default
    // LEN 50 % each child would end naturally inside its own 603-sample slot and NOT
    // ONE off would satisfy that.
    //
    // ── WHY IT IS "MOST", NOT "ALL", AND WHY THAT IS THE INTERESTING NUMBER ──
    // Swing widens the gap between an EVEN step's last child and the following ODD
    // step's first child (the odd step is pushed 0.32 of a step later), and 905 samples
    // of note cannot span it — so those children DO end naturally, uncapped. Between an
    // odd step's last child and the next even step's first child the geometry INVERTS
    // instead: the even step's children begin before the odd step's last child, i.e.
    // index order is not sample order. Both are properties of the geometry this file
    // exists to freeze, so the count is a measured literal rather than "all".
    //
    // The test is SET MEMBERSHIP (`off + 1` is some onset) rather than
    // `offs[i] == ons[i + 1] - 1`, because that pairing silently assumes ons and offs
    // alternate in index order — and under the inversion above they do not. The first
    // draft made exactly that assumption and reported 190 of 203 as a failure.
    const auto ons = bake.select ([] (const TimedMidiEvent& event) { return event.message.isNoteOn (); });
    const auto offs = bake.select ([] (const TimedMidiEvent& event) { return event.message.isNoteOff (); });

    INFO (bake.describe (12));
    REQUIRE (ons.size () > 100u);
    REQUIRE (offs.size () == ons.size ());

    std::vector<std::int64_t> onsetSamples;
    onsetSamples.reserve (ons.size ());
    for (const auto& event : ons)
        onsetSamples.push_back (event.absoluteSample);
    std::sort (onsetSamples.begin (), onsetSamples.end ());

    int cutShort = 0;
    for (const auto& event : offs)
        if (std::binary_search (onsetSamples.begin (), onsetSamples.end (), event.absoluteSample + 1))
            ++cutShort;

    INFO ("note-offs sitting exactly one sample before an onset: " << cutShort << " of " << offs.size ());

    // TWO FLOORS, and both matter. The lower one says the cap binds at all (LEN 50 %
    // scores 0 here). The upper one says the swing-widened gaps are real — a file in
    // which EVERY off were capped would mean swing had stopped displacing anything.
    REQUIRE (cutShort > 150);
    REQUIRE (static_cast<std::size_t> (cutShort) < offs.size () - 1);

    // ── THE FIRST TWO ONSETS COME FROM STEP -1 ───────────────────────────────
    // Not a defect, and worth naming because it looks like one. Step -1's grid position
    // is PPQ -0.25; swing pushes it 0.32 of a step LATER and its children run a further
    // 7/8 of a step ahead, so children 6 and 7 land at samples 337 and 941 — inside the
    // played timeline. The walk reaches index -1 from block 0 through `stepScanBack` and
    // `ownsPpq` accepts both. Audibly it is the tail of a ratchet that began just before
    // the loop point. tests/substep_ownership.cpp pins the same behaviour with counters.
    REQUIRE (containsEvent (bake, 337, 0x90, 0x3C, 0x64));
    REQUIRE (containsEvent (bake, 941, 0x90, 0x3C, 0x64));
    REQUIRE (ons.front ().absoluteSample == 0); // step 0's child 0 still leads the file

    // Every child of every step shares the one pool pitch (0x3C) — that is what makes
    // the chain above a same-pitch chain rather than a coincidence.
    int wrongPitch = 0;
    for (const auto& event : bake.events)
        if ((event.message.isNoteOn () || event.message.isNoteOff ()) &&
            event.message.getNoteNumber () != poolPitches[0])
            ++wrongPitch;

    REQUIRE (wrongPitch == 0);

    // ── NOTHING LANDS ROUND ──────────────────────────────────────────────────
    // The whole reason for the hostile clock. A step is 4828.4671… samples and a child
    // slot 603.558…, so at most a handful of onsets can be exact multiples of anything;
    // if a majority were, some arithmetic had quietly become integral.
    int roundOnsets = 0;
    for (const auto& event : ons)
        if (event.absoluteSample % 100 == 0)
            ++roundOnsets;

    INFO ("onsets that are multiples of 100: " << roundOnsets << " of " << ons.size ());
    REQUIRE (static_cast<std::size_t> (roundOnsets) * 4u < ons.size ());

    const auto check =
        checkGolden (bake, headerFor (bake, "ratchet-swing-retrigger", fractionalBpm, goldenGridPpq, phase7RngVersion));
    INFO (check.report);
    REQUIRE (check.passed);

    const auto golden = loadGolden ("ratchet-swing-retrigger");
    INFO (golden.error);
    REQUIRE (golden.ok);
    REQUIRE (golden.header.rngVersion == phase7RngVersion);

    const auto sweep = sweepAgainstGolden (golden,
                                           &configureRatchetSwingRetrigger,
                                           schedule,
                                           fractionalSampleRate,
                                           ratchetFractionalSpan);
    INFO (sweep.report);
    REQUIRE (sweep.sizesChecked == numGoldenBlockSizes);
    REQUIRE (sweep.spansCorrect);
    REQUIRE (sweep.allSorted);
    REQUIRE (sweep.minNoteOns > 0);
    REQUIRE (sweep.allLifecyclesBalanced);
    REQUIRE (sweep.minEvents == sweep.maxEvents);

    // ── THE NEGATIVE CONTROL: LEN 150 -> 149 % ──────────────────────────────
    const auto perturbed = renderScenario (&configureRatchetSwingRetriggerPerturbed,
                                           schedule,
                                           fractionalSampleRate,
                                           ratchetFractionalSpan,
                                           bakeBlockSize);
    REQUIRE (! perturbed.empty ());
    REQUIRE (! compareToGolden (perturbed, golden).matches);

    // KEPT LAST, the same discipline as `tied-retrigger`: everything else in this case
    // is verified first, so a cross-block-size divergence on the hostile clock is
    // reported against a file whose literals have already been checked. If this
    // reddens, THAT IS A FINDING.
    REQUIRE (sweep.sizesMatched == numGoldenBlockSizes);
}

// ─────────────────────────────────────────────────────────────────────────────
// The two-way inventory check
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/golden: the golden directory matches the expected inventory exactly", "[determinism]")
{
    // BOTH DIRECTIONS, and both matter. A one-way "every expected file exists"
    // check misses an ORPHAN — a golden left behind by a deleted or renamed case,
    // which nothing compares against and which therefore silently rots. A one-way
    // "every file is expected" check misses a DELETED golden, which would leave its
    // case auto-creating the file and failing with "golden created", but only if
    // that case still ran at all.
    const auto onDisk = listGoldenFiles ();

    juce::String listing;
    for (const auto& name : onDisk)
        listing << "  " << name << "\n";
    INFO ("on disk:\n" << listing);

    REQUIRE (static_cast<int> (onDisk.size ()) == numExpectedGoldens);
    REQUIRE (numExpectedGoldens == 12); // six Phase-6 files plus Phase 7's six

    for (int i = 0; i < numExpectedGoldens; ++i)
    {
        INFO ("expected entry " << i << " = " << expectedGoldens[i].name);
        REQUIRE (onDisk[static_cast<std::size_t> (i)] == juce::String (expectedGoldens[i].name));
    }

    // Every expected golden parses. A file that exists but cannot be read is the
    // same failure as a missing one (GoldenMidiFile.h: "a missing or unparseable
    // golden is a FAILURE, never a skip").
    //
    // ── AND ITS `rngVersion` IS THE ONE ITS SCENARIO DECLARES ────────────────
    // Per scenario, not blanket. See `ExpectedGolden` for why the blanket form had to
    // go, and note what the table buys on top: the six Phase-6 files are now pinned at
    // 0, so a phase that "helpfully" restamps them — which would be a claim that their
    // audible content was produced under a different RNG schema — reddens here.
    int atVersionZero = 0;
    int atVersionOne = 0;

    for (const auto& expected : expectedGoldens)
    {
        const auto golden = loadGolden (expected.name);
        INFO ("golden '" << expected.name << "': " << golden.error);
        REQUIRE (golden.ok);
        REQUIRE (! golden.events.empty ());
        REQUIRE (golden.header.name == juce::String (expected.name));
        REQUIRE (golden.header.rngVersion == expected.rngVersion);

        atVersionZero += expected.rngVersion == 0 ? 1 : 0;
        atVersionOne += expected.rngVersion == phase7RngVersion ? 1 : 0;
    }

    // ANTI-VACUITY FOR THE TABLE ITSELF: a table that had drifted to all-zeros (or
    // all-ones) would still satisfy every assertion above. Both versions must be
    // represented, and the split must be the 6/6 the two phases produced.
    REQUIRE (atVersionZero == 6);
    REQUIRE (atVersionOne == 6);
    REQUIRE (atVersionZero + atVersionOne == numExpectedGoldens);

    // The version the Phase-7 files are stamped with is the engine's own, not a
    // literal that could drift away from it.
    REQUIRE (phase7RngVersion == arpbox::engine::rng::rngVersion);
}
