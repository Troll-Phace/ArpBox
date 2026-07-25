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

#include "engine/graph/EngineCommand.h"
#include "engine/sequencer/PatternDocument.h"
#include "engine/sequencer/PatternTypes.h"

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

// ─────────────────────────────────────────────────────────────────────────────
// The inventory — the single source of truth for "which goldens exist"
// ─────────────────────────────────────────────────────────────────────────────

/** Every golden this suite owns, in the sorted order `listGoldenFiles` returns. */
const char* const expectedGoldens[] = {
    "baseline-4bar",      "direction-modes-cycle", "euclid-gate",
    "polymeter-clockdiv", "polymeter-len-3-5-7",   "tied-retrigger",
};
constexpr int numExpectedGoldens = static_cast<int> (std::size (expectedGoldens));
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

    // Every schedule in this file lands on block heads everywhere.
    REQUIRE (scheduleAlignsEverywhere (playThenStop (goldenBpm, fourBarMusic)));
    REQUIRE (scheduleAlignsEverywhere (playThenStop (goldenBpm, eightBarMusic)));
    REQUIRE (scheduleAlignsEverywhere (playThenStop (fractionalBpm, fractionalMusic)));
    REQUIRE (scheduleAlignsEverywhere (directionCycleSchedule ()));
    REQUIRE (scheduleAlignsEverywhere (euclidSchedule ()));

    // EVERY schedule in this file ends with a stop — that is what makes
    // `allLifecyclesBalanced` a universal claim rather than a per-case exemption.
    for (const auto& schedule : { playThenStop (goldenBpm, fourBarMusic),
                                  playThenStop (goldenBpm, eightBarMusic),
                                  playThenStop (fractionalBpm, fractionalMusic),
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

    for (int i = 0; i < numExpectedGoldens; ++i)
    {
        INFO ("expected entry " << i << " = " << expectedGoldens[i]);
        REQUIRE (onDisk[static_cast<std::size_t> (i)] == juce::String (expectedGoldens[i]));
    }

    // Every expected golden parses. A file that exists but cannot be read is the
    // same failure as a missing one (GoldenMidiFile.h: "a missing or unparseable
    // golden is a FAILURE, never a skip").
    for (const auto* name : expectedGoldens)
    {
        const auto golden = loadGolden (name);
        INFO ("golden '" << name << "': " << golden.error);
        REQUIRE (golden.ok);
        REQUIRE (! golden.events.empty ());
        REQUIRE (golden.header.name == juce::String (name));
        REQUIRE (golden.header.rngVersion == 0);
    }
}
