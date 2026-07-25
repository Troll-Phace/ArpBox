// ─────────────────────────────────────────────────────────────────────────────
// sequencer_offdeterminism — THE BEHAVIOURAL BACKSTOP FOR ONE FAILURE CLASS:
// a note-off placed from a WITHIN-BLOCK OFFSET instead of from its own ABSOLUTE
// SCHEDULE (ARCHITECTURE §1.2 the determinism contract, §5.5 MIDI correctness;
// "THE PLACEMENT RULE" in engine/midi/SoundingNoteTable.h).
//
// ── WHY THIS FILE EXISTS ────────────────────────────────────────────────────
// Phase 6 produced THREE defects with ONE root cause, each found by a different
// mechanism and NONE of them by the test suite:
//
//   #36  `emitStep` retrigger, ALREADY-DUE branch          (found by code review)
//   #46  `emitStep` retrigger, STILL-SOUNDING branch       (found by the golden bake)
//   #48  `flushForPatternSwitch` → `SoundingNoteTable::flush`  (found by code review)
//
// The decisive evidence: with the #48 fix reverted, ALL 189 PERMANENT TESTS STILL
// PASSED. The engine has since been hardened STRUCTURALLY — `retireAt(offset)` and
// `flush(offset)` are deleted and no public method of `SoundingNoteTable` accepts a
// within-block offset — and this file is the behavioural half of that guarantee.
//
// ── WHY EACH EXISTING TEST MISSED THE CLASS, AND WHAT THIS ONE DOES DIFFERENTLY ─
//   • pattern_switch.cpp sweeps with 6000-SAMPLE STEPS, larger than the largest
//     swept block (4096), so two steps never share a block and the shape cannot
//     arise. → THIS FILE runs 1200-sample steps, so THREE steps fit in one 4096
//     block. That single condition is what all three defects needed.
//   • The six goldens' pattern switches all land on 61440-ALIGNED BLOCK HEADS,
//     which take the (correct) pre-flush path. → THIS FILE's #48 scenario resolves
//     to step 61, an ODD step index, and an odd multiple of 1200 is a block head at
//     NO swept block size (proved as a precondition below), so the flush is
//     MID-BLOCK at all ten.
//   • pattern_alloc_guard.cpp hits the shape at ONE block size, so there is nothing
//     to compare against. → THIS FILE compares ten.
//   • sequencer_retrigger.cpp uses a 50% GATE, so only the already-due branch is
//     ever exercised. → THIS FILE's cases B and F run LEN > 100%, which is the only
//     way a same-pitch onset finds the previous note still sounding.
// Each was individually reasonable. The gap was BETWEEN them.
//
// ── THE GOVERNING PROPERTY ──────────────────────────────────────────────────
// For a fixed musical scenario the complete MIDI event stream — POSITIONS and
// BYTES and EVENT COUNT — is identical at every block size, for EVERY path that
// can emit a note-off. All three are asserted, not just the first: #48's
// divergence showed up as 309 events against 308, because a stale table entry
// joined the CC123 sweep. A position-only comparison would have missed it. CC123
// placement and count are additionally compared on their own, because the sweep is
// the part of a flush that a position diff buries in the middle of a long stream.
//
// ── THE SEVEN SCENARIOS AND THE PATHS THEY COVER ────────────────────────────
//   A  natural gate-end (`emitDueNoteOffs`) + same-pitch retrigger, ALREADY-DUE
//   B  same-pitch retrigger, STILL-SOUNDING (LEN 150 %, the #46 shape)
//   C  MID-BLOCK quantized-switch flush over already-ended entries (the #48 shape)
//   D  switch flush landing exactly ON a block head (the PRE-FLUSH sibling path)
//   E  discontinuity flush — stop and locate
//   F  all of the above in ONE render, because #48's spurious CC123 only appeared
//      in combination
//   G  EMISSION ORDER among offs sharing one sample (issue #51, found by this file
//      while it was being written — see that case for the whole story)
//
// A FOURTH DEFECT CAME OUT OF BUILDING THIS. #51 is not a misplaced sample: it is
// two note-offs on ONE sample coming out in a buffer-size-dependent ORDER, because
// the table's storage order depended on removal history. It is fixed (order-
// preserving `removeAt` plus a forward `emitDueNoteOffs` walk — the two are
// coupled), and it is why cases D, E and G all flush SEVERAL notes at once rather
// than routing around the multi-note shape that real tie/legato music produces
// constantly.
//
// ── ANTI-VACUITY, WHICH IS THE POINT ────────────────────────────────────────
// Two empty renders compare equal, and a scenario that never enters the branch it
// names passes forever. So every case carries:
//   • FIRST-PRINCIPLES LITERALS — a note-on count and at least one absolute
//     note-off sample, written as numbers in this source, never re-derived from
//     the node;
//   • A REACHABILITY ASSERTION per path, proving the branch was exercised (see
//     `FlushProbe` and the per-case notes — #48 hid for a whole phase precisely
//     because three tests all THOUGHT they covered it);
//   • A NEGATIVE CONTROL: a one-lane-value perturbation whose stream must NOT
//     match, so a comparator that always returned true fails here.
// No Catch2 macro runs inside a sweep or a per-block loop: everything is
// aggregated into a struct and asserted afterwards.
//
// ── THE CLOCK ───────────────────────────────────────────────────────────────
// 300 BPM (`Transport::maxBpm`) @ 48 kHz on the 1/32 grid (`gridStepPpq` 0.125,
// in spec per §2.1 "1/32..1/4") ⇒ ONE STEP IS EXACTLY 1200 SAMPLES, so three steps
// fit inside a 4096-sample block and every literal below is an exact integer.
// The alignment unit is 61440 = lcm {32, 64, 96, 128, 256, 480, 512, 1024, 2048,
// 4096} — the only samples that are block heads at every swept size, so every
// scheduled command sits on a multiple of it and every span is a multiple of it.
// ─────────────────────────────────────────────────────────────────────────────

#include "support/MidiRenderHarness.h"
#include "support/NoteLifecycleCheck.h"
#include "support/SequencerRenderRig.h"

#include "engine/graph/EngineCommand.h"
#include "engine/graph/Transport.h"
#include "engine/midi/NotePool.h"
#include "engine/sequencer/PatternDocument.h"
#include "engine/sequencer/PatternTypes.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using arpbox::engine::EngineCommandType;
using arpbox::engine::LaneId;
using arpbox::engine::maxSteps;
using arpbox::engine::PatternDocument;
using arpbox::engine::PoolSnapshot;
using arpbox::engine::QuantizeMode;
using arpbox::engine::Transport;
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
// ─────────────────────────────────────────────────────────────────────────────
// The clock, the sweep, and the pool — all as literals
// ─────────────────────────────────────────────────────────────────────────────

constexpr double offSampleRate = 48000.0;
constexpr double offBpm = 300.0;         ///< Transport::maxBpm — asserted, not assumed.
constexpr double offGridStepPpq = 0.125; ///< 1/32 (§2.1 allows 1/32..1/4).

/** One step: 0.125 x (60 / 300) x 48000 = 1200, exactly. THE reachability
    precondition for this whole file — three of these fit in one 4096-sample block,
    which is the single condition all three defects needed. */
constexpr std::int64_t stepSamples = 1200;

/** lcm {32, 64, 96, 128, 256, 480, 512, 1024, 2048, 4096} = 2^12 x 3 x 5. */
constexpr std::int64_t alignmentUnit = 61440;

/** 4096 is the largest realistic device buffer and the size every defect in this
    family needed; 96 and 480 are here because real CoreAudio devices hand out
    non-powers of two. */
constexpr int sweptBlockSizes[] = { 32, 64, 96, 128, 256, 480, 512, 1024, 2048, 4096 };
constexpr int numSweptBlockSizes = static_cast<int> (std::size (sweptBlockSizes));

/** The block size the per-block reachability probes run at. It must be LARGER than
    one step or none of these code paths can be entered at all. */
constexpr int probeBlockSize = 4096;

/** PatternDocument's documented default pool: C major, one octave. Written HERE so
    every expectation below derives from a value in the TEST, not from the engine. */
constexpr int poolPitches[] = { 60, 62, 64, 65, 67, 69, 71, 72 };
constexpr int poolSize = static_cast<int> (std::size (poolPitches));

constexpr int outChannel = 1; ///< PatternSetState::outputChannel default.
constexpr int velocityA = 100;
constexpr int velocityB = 111; ///< THE one byte that says "pattern B played this".

constexpr int patternA = 0;
constexpr int patternB = 1;

// Raw MIDI status bytes, spelled out so the literal assertions read as MIDI.
constexpr int noteOnCh1 = 0x90;
constexpr int noteOffCh1 = 0x80;
constexpr int ccCh1 = 0xB0;
constexpr int allNotesOffCc = 0x7B;

// ─────────────────────────────────────────────────────────────────────────────
// Small helpers (the idiom determinism_goldens.cpp already uses)
// ─────────────────────────────────────────────────────────────────────────────

using ConfigureFn = void (*) (PatternDocument&);

/** Samples per step from the MUSICAL definition, derived in the test rather than
    read off the node — so a node that computed it differently would disagree with
    the 1200 literal instead of agreeing with itself. */
double samplesPerStepAt (double stepPpq, double bpm, double sampleRate) noexcept
{
    return stepPpq * (60.0 / bpm) * sampleRate;
}

/** True when `sample` is a block head at EVERY swept size (i.e. a legal command
    position for a cross-size comparison). */
bool isHeadEverywhere (std::int64_t sample) noexcept
{
    for (const int blockSize : sweptBlockSizes)
        if (sample % static_cast<std::int64_t> (blockSize) != 0)
            return false;

    return true;
}

/** How many swept block sizes have `sample` as a block head. 0 ⇒ the sample is
    strictly mid-block everywhere, which is the precondition case C rests on. */
int headCount (std::int64_t sample) noexcept
{
    int count = 0;
    for (const int blockSize : sweptBlockSizes)
        if (sample % static_cast<std::int64_t> (blockSize) == 0)
            ++count;

    return count;
}

/** `true` if `event` is exactly this absolute sample carrying exactly these three
    MIDI bytes. */
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

/** The raw bytes of every event at exactly `sample`, IN EMISSION ORDER, as one flat
    vector — so a test can pin not just which events share a sample but the order
    they came out in.

    Only meaningful because issue #51 made that order deterministic: storage order in
    `SoundingNoteTable` is now the add-order subsequence of the live set, so a flush
    emits oldest-registered first at every buffer size. Before the fix this vector
    was a coin flip between carvings. */
std::vector<int> byteSequenceAt (const MidiRenderResult& render, std::int64_t sample)
{
    std::vector<int> bytes;
    for (const auto& event : render.events)
        if (event.absoluteSample == sample)
            for (int i = 0; i < event.numBytes (); ++i)
                bytes.push_back (static_cast<int> (event.bytes ()[i]));

    return bytes;
}

std::vector<TimedMidiEvent> noteOnsOf (const MidiRenderResult& render)
{
    return render.select ([] (const TimedMidiEvent& event) { return event.message.isNoteOn (); });
}

std::vector<TimedMidiEvent> noteOffsOf (const MidiRenderResult& render)
{
    return render.select ([] (const TimedMidiEvent& event) { return event.message.isNoteOff (); });
}

/** Absolute positions of every CC123 in the render, in emission order. THE thing a
    position-only stream diff buries: #48 added one of these and nothing else. */
std::vector<std::int64_t> sweepSamplesOf (const MidiRenderResult& render)
{
    std::vector<std::int64_t> samples;
    for (const auto& event : render.events)
        if (event.message.isControllerOfType (123))
            samples.push_back (event.absoluteSample);

    return samples;
}

/** Note-ons carrying `velocity` — "the notes pattern A played" / "…B played". */
std::vector<TimedMidiEvent> notesFromPattern (const MidiRenderResult& render, int velocity)
{
    return render.select ([velocity] (const TimedMidiEvent& event)
                          { return event.message.isNoteOn () && event.message.getVelocity () == velocity; });
}

/** The render's byte stream with events sorted into canonical (position, then raw
    bytes) order — `MidiRenderResult`'s documented "canonical form when a test
    deliberately does not care about within-sample ordering". Identical to
    `toByteStream()` whenever no two events share a sample. */
std::vector<std::uint8_t> canonicalByteStream (const MidiRenderResult& render)
{
    MidiRenderResult sorted = render;
    std::sort (sorted.events.begin (), sorted.events.end ());
    return sorted.toByteStream ();
}

MidiRenderConfig renderConfig (std::int64_t span, int blockSize)
{
    auto config = MidiRenderConfig::samples (span, offSampleRate, blockSize);
    config.numChannels = 1;      // MIDI-only node; the scratch buffer only carries a length
    config.eventReserve = 16384; // > any render here, so the loop never reallocates
    return config;
}

/** One complete render from a FRESH rig — the only way a stream is produced here,
    so no scenario can observe another's leftover state. */
MidiRenderResult
renderAt (ConfigureFn configure, const std::vector<ScheduledCommand>& schedule, std::int64_t span, int blockSize)
{
    SequencerRig rig { offSampleRate, blockSize };
    configure (rig.patternDocument);
    return renderSequencer (rig, renderConfig (span, blockSize), schedule);
}

/** Tempo + play at sample 0, then STOP at `musicEnd` (a §5.5 flush point, so the
    stream ends balanced rather than truncated mid-gate).

    THE TEMPO COMMAND IS NOT OPTIONAL. A fresh `Transport` runs at
    `Transport::defaultBpm` (120), which would silently double every step length;
    the 1200-sample literals are what catches that. */
std::vector<ScheduledCommand> playThenStop (std::int64_t musicEnd)
{
    return { ScheduledCommand { 0, engineCommand (EngineCommandType::setTempoBpm, offBpm) },
             ScheduledCommand { 0, engineCommand (EngineCommandType::transportPlay) },
             ScheduledCommand { musicEnd, engineCommand (EngineCommandType::transportStop) } };
}

// ─────────────────────────────────────────────────────────────────────────────
// THE SWEEP — aggregated, asserted once, afterwards
// ─────────────────────────────────────────────────────────────────────────────

/** What comparing one scenario across all ten block sizes observed. The reference
    is the FIRST swept size (32); every later size is compared against it.

    FOUR INDEPENDENT COMPARISONS, because #48 proved positions alone insufficient:
    the byte stream in EMISSION order, the byte stream in CANONICAL order, the EVENT
    COUNT, and the CC123 placement/count.

    ── WHY EMISSION ORDER AND CANONICAL ORDER ARE TRACKED SEPARATELY ───────────
    They differ by exactly one thing: the order of events that share an absolute
    sample. BOTH ARE NOW REQUIRED, at all ten sizes, in every case — see
    `REQUIRE_SWEEP_CLEAN`. The pair is kept rather than collapsed into the strict
    comparison alone because the DIFFERENCE between them is diagnostic: a failure
    that trips `streamsMatched` while `canonicalMatched` holds is an ORDERING defect
    (issue #51's shape), and one that trips both is a PLACEMENT defect (the
    #36/#46/#48 family). Splitting them is what let #51 be identified as the former
    within one run instead of being mistaken for a fourth instance of the latter. */
struct OffSweep
{
    int sizesChecked = 0;
    int streamsMatched = 0;   ///< Byte-identical in EMISSION order (positions AND bytes).
    int canonicalMatched = 0; ///< Byte-identical once sorted by (position, bytes).
    int countsMatched = 0;    ///< Identical EVENT COUNT — #48 showed up as 309 vs 308.
    int sweepsMatched = 0;    ///< Identical CC123 positions AND count.

    std::int64_t minEvents = 0;
    std::int64_t maxEvents = 0;
    std::size_t minSweeps = 0;
    std::size_t maxSweeps = 0;

    /** Fewest note-ons any swept size produced. GATES the balance claim: an empty
        stream is trivially balanced, so `allBalanced` means nothing until this is
        shown to be positive. */
    std::int64_t minNoteOns = 0;

    bool spansCorrect = true;
    bool allSorted = true;
    bool allBalanced = true;

    juce::String report;
};

OffSweep sweepBlockSizes (ConfigureFn configure, const std::vector<ScheduledCommand>& schedule, std::int64_t span)
{
    OffSweep outcome;
    MidiRenderResult reference;
    std::vector<std::uint8_t> referenceBytes;
    std::vector<std::uint8_t> referenceCanonical;
    std::vector<std::int64_t> referenceSweeps;

    outcome.report << "note-off sweep over " << juce::String (numSweptBlockSizes) << " block sizes:\n";

    for (const int blockSize : sweptBlockSizes)
    {
        const auto render = renderAt (configure, schedule, span, blockSize);
        const auto bytes = render.toByteStream ();
        const auto canonical = canonicalByteStream (render);
        const auto sweeps = sweepSamplesOf (render);
        const auto count = static_cast<std::int64_t> (render.events.size ());

        ++outcome.sizesChecked;

        if (outcome.sizesChecked == 1)
        {
            reference = render;
            referenceBytes = bytes;
            referenceCanonical = canonical;
            referenceSweeps = sweeps;

            outcome.minEvents = outcome.maxEvents = count;
            outcome.minSweeps = outcome.maxSweeps = sweeps.size ();
        }
        else
        {
            outcome.minEvents = std::min (outcome.minEvents, count);
            outcome.maxEvents = std::max (outcome.maxEvents, count);
            outcome.minSweeps = std::min (outcome.minSweeps, sweeps.size ());
            outcome.maxSweeps = std::max (outcome.maxSweeps, sweeps.size ());
        }

        const bool streamOk = (bytes == referenceBytes);
        const bool canonicalOk = (canonical == referenceCanonical);
        const bool countOk = (count == static_cast<std::int64_t> (reference.events.size ()));
        const bool sweepOk = (sweeps == referenceSweeps);

        outcome.streamsMatched += streamOk ? 1 : 0;
        outcome.canonicalMatched += canonicalOk ? 1 : 0;
        outcome.countsMatched += countOk ? 1 : 0;
        outcome.sweepsMatched += sweepOk ? 1 : 0;

        if (render.numSamples != span)
            outcome.spansCorrect = false;

        if (! render.isSampleSorted ())
            outcome.allSorted = false;

        NoteLifecycleTracker tracker;
        tracker.observeAll (render);
        if (! tracker.balanced ())
            outcome.allBalanced = false;

        const auto noteOns = static_cast<std::int64_t> (tracker.noteOnsSeen ());
        outcome.minNoteOns = outcome.sizesChecked == 1 ? noteOns : std::min (outcome.minNoteOns, noteOns);

        outcome.report << "  block " << juce::String (blockSize) << ": " << juce::String (count) << " events, "
                       << juce::String (static_cast<std::int64_t> (sweeps.size ())) << " CC123, "
                       << (streamOk ? "stream OK" : "STREAM MISMATCH") << ", "
                       << (canonicalOk ? "canonical OK" : "CANONICAL MISMATCH") << ", "
                       << (countOk ? "count OK" : "COUNT DIFF") << ", " << (sweepOk ? "sweeps OK" : "SWEEP DIFF")
                       << "\n";

        if (! streamOk)
            outcome.report << reference.describeDifference (render) << "\n";
    }

    return outcome;
}

/** Every case asserts the same five sweep invariants; only the literals differ. */
#define REQUIRE_SWEEP_CLEAN(sweep, expectedEvents, expectedSweeps)                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        INFO ((sweep).report);                                                                                         \
        REQUIRE ((sweep).sizesChecked == numSweptBlockSizes);                                                          \
        REQUIRE ((sweep).spansCorrect);                                                                                \
        REQUIRE ((sweep).allSorted);                                                                                   \
        REQUIRE ((sweep).minNoteOns > 0); /* gates the balance claim — see OffSweep */                                 \
        REQUIRE ((sweep).allBalanced);                                                                                 \
        REQUIRE ((sweep).minEvents == (sweep).maxEvents);                                                              \
        REQUIRE ((sweep).minEvents == (expectedEvents));                                                               \
        REQUIRE ((sweep).minSweeps == (sweep).maxSweeps);                                                              \
        REQUIRE ((sweep).minSweeps == static_cast<std::size_t> (expectedSweeps));                                      \
        REQUIRE ((sweep).canonicalMatched == numSweptBlockSizes);                                                      \
        REQUIRE ((sweep).countsMatched == numSweptBlockSizes);                                                         \
        REQUIRE ((sweep).sweepsMatched == numSweptBlockSizes);                                                         \
        REQUIRE ((sweep).streamsMatched == numSweptBlockSizes);                                                        \
    } while (false)

// ─────────────────────────────────────────────────────────────────────────────
// THE PER-BLOCK REACHABILITY PROBE
// ─────────────────────────────────────────────────────────────────────────────

/** What one block-by-block pass observed. The harness's absolute-position stream
    deliberately cannot see a block boundary, and the boundary is the whole question
    here, so these cases re-drive the rig one block at a time.

    ── HOW `offsBeforeMarkInMarkBlock` PROVES THE #48 BRANCH RAN ───────────────
    `mark` is the switch's absolute adopt sample. This counts note-offs emitted, in
    the SINGLE block that contains `mark`, at absolute samples in
    `[markBlockStart, mark)`.

    Two kinds of off can legitimately appear in that window, and every case using
    this counter names which of its offs is which rather than reading the number
    alone: the CUT-SHORT off the flush places at `mark - 1`, and — the interesting
    one — an off at its OWN due sample somewhere earlier in the window.

    The latter can only have come from `SoundingNoteTable::flush`'s ALREADY-ENDED
    branch. The alternatives are excluded by the engine's own ordering
    (`SequencerProcessor::processBlock` steps 2 → 3 → 4):
      • `emitDueNoteOffs` runs at the END of the block, AFTER the walk that fires
        the flush — and the flush empties the table, so nothing is left for it;
      • `retireNoLaterThan` (the same-pitch retrigger) needs the same pitch to
        recur, which the scenarios that use this counter either exclude entirely
        (case C: a pitch recurs every 8 steps = 9600 samples) or account for
        explicitly (case F, where the retrigger's off is separately identified).
    A pre-flush from the PREVIOUS block cannot contribute either: that path fires
    only when `mark` IS a block head, in which case this window is empty. */
struct FlushProbe
{
    int blocksRendered = 0;
    std::int64_t noteOns = 0;

    std::int64_t markBlockStart = -1;              ///< First sample of the block containing `mark`.
    bool markIsBlockHead = false;                  ///< `mark % blockSize == 0` ⇒ the PRE-FLUSH path.
    int offsBeforeMarkInMarkBlock = 0;             ///< See the note above — the #48 reachability number.
    int sweepsInMarkBlock = 0;                     ///< CC123s emitted in that same block.
    std::vector<std::int64_t> markBlockOffSamples; ///< Their absolute positions, for INFO.

    /** Blocks containing a note-off for a pitch followed, LATER IN THE SAME BLOCK,
        by a note-on for that pitch. Observable ONLY when `emitStep` found an entry
        still in the table — i.e. the #36 / #46 retrigger path. Present whether or
        not the fixes are in (they move WHERE the off lands, not whether the pair
        exists), which is what makes it an honest probe rather than a second copy of
        the fix assertion. */
    int blocksWithRetriggerPair = 0;
    int retriggerPairs = 0;

    juce::String describe () const
    {
        juce::String text;
        text << "probe: " << juce::String (blocksRendered) << " blocks, " << juce::String (noteOns) << " note-ons, "
             << juce::String (retriggerPairs) << " retrigger pairs in " << juce::String (blocksWithRetriggerPair)
             << " blocks; mark block starts at " << juce::String (markBlockStart)
             << (markIsBlockHead ? " (mark IS a block head)" : " (mark is MID-BLOCK)") << ", "
             << juce::String (offsBeforeMarkInMarkBlock) << " off(s) before the mark in it, "
             << juce::String (sweepsInMarkBlock) << " CC123 in it";

        for (const auto sample : markBlockOffSamples)
            text << "\n    off before mark @" << juce::String (sample);

        return text;
    }
};

/** Renders `span` samples block by block at `blockSize`, in the graph's own
    command-drain-then-`beginBlock` order, collecting the counters above. `mark` < 0
    disables the mark half. NO CATCH2 MACRO RUNS IN THIS LOOP. */
FlushProbe probeBlocks (int blockSize,
                        ConfigureFn configure,
                        const std::vector<ScheduledCommand>& schedule,
                        std::int64_t span,
                        std::int64_t mark)
{
    SequencerRig rig { offSampleRate, blockSize };
    configure (rig.patternDocument);

    juce::AudioBuffer<float> audio (1, blockSize);
    juce::MidiBuffer midi;
    midi.ensureSize (16384);

    FlushProbe probe;

    if (mark >= 0)
    {
        probe.markBlockStart = (mark / blockSize) * blockSize;
        probe.markIsBlockHead = (mark % blockSize) == 0;
    }

    const auto blocks = static_cast<int> (span / blockSize);

    for (int block = 0; block < blocks; ++block)
    {
        const std::int64_t base = static_cast<std::int64_t> (block) * blockSize;

        // Command drain, THEN beginBlock — the graph head node's order, and the
        // reason a command lands in the very block it was scheduled for.
        for (const auto& entry : schedule)
            if (entry.atSample >= base && entry.atSample < base + blockSize)
                rig.applyCommand (entry.command);

        midi.clear ();
        rig.renderBlock (audio, midi);
        ++probe.blocksRendered;

        const bool isMarkBlock = (mark >= 0 && base == probe.markBlockStart);

        // Earliest note-off offset seen so far in THIS block, per pitch; -1 = none.
        std::array<int, 128> firstOffOffset {};
        firstOffOffset.fill (-1);

        bool flagged = false;

        // juce::MidiBuffer iterates in non-decreasing sample order, so "later in the
        // block" is simply "seen after".
        for (const auto meta : midi)
        {
            const auto message = meta.getMessage ();
            const std::int64_t absolute = base + static_cast<std::int64_t> (meta.samplePosition);

            if (message.isControllerOfType (123))
            {
                if (isMarkBlock)
                    ++probe.sweepsInMarkBlock;
                continue;
            }

            const int note = message.getNoteNumber ();
            if (note < 0 || note > 127)
                continue;

            const auto slot = static_cast<std::size_t> (note);

            // isNoteOff() FIRST — a velocity-0 note-on is a release (the same
            // ordering, and the same reason, as NoteLifecycleCheck.h).
            if (message.isNoteOff ())
            {
                if (firstOffOffset[slot] < 0)
                    firstOffOffset[slot] = meta.samplePosition;

                if (isMarkBlock && absolute < mark)
                {
                    ++probe.offsBeforeMarkInMarkBlock;
                    probe.markBlockOffSamples.push_back (absolute);
                }
            }
            else if (message.isNoteOn ())
            {
                ++probe.noteOns;

                if (firstOffOffset[slot] >= 0 && meta.samplePosition > firstOffOffset[slot])
                {
                    ++probe.retriggerPairs;
                    flagged = true;
                }
            }
        }

        if (flagged)
            ++probe.blocksWithRetriggerPair;
    }

    return probe;
}

// ─────────────────────────────────────────────────────────────────────────────
// Scenario configurations
// ─────────────────────────────────────────────────────────────────────────────

/** A ONE-NOTE pool, so every gated step retriggers the same pitch — the simplest
    reliable way to put one pitch on adjacent steps. */
void setSinglePitchPool (PatternDocument& document)
{
    PoolSnapshot pool {};
    pool.size = 1;
    pool.sorted[0] = static_cast<std::uint8_t> (poolPitches[0]);
    pool.asPlayed[0] = static_cast<std::uint8_t> (poolPitches[0]);
    document.setPool (pool);
}

/** Turns pattern `index`'s GATE lane fully on over all `maxSteps` storage slots and
    sets its active LENGTH — which is also its `gatePeriodSteps`, i.e. what a
    `patternEnd` switch quantizes to.

    An ALL-ON gate lane makes the gated ordinal equal the global step index at ANY
    length (`loop * length + step % length == step`), so the pool traversal is
    continuous across a pattern switch regardless of the two lanes' lengths. */
void gateOnWithPeriod (PatternDocument& document, int index, int gateLength)
{
    document.setLaneLength (index, LaneId::gate, gateLength);

    for (int step = 0; step < maxSteps; ++step)
        document.setLaneValue (index, LaneId::gate, step, 1);
}

/** Fills one lane of pattern `index` with a single value across all storage slots. */
void fillLane (PatternDocument& document, int index, LaneId lane, int value)
{
    for (int step = 0; step < maxSteps; ++step)
        document.setLaneValue (index, lane, step, value);
}

// ── A: single pitch, LEN 50 % ────────────────────────────────────────────────

void configureRetrigger (PatternDocument& document)
{
    document.beginTransaction ();
    document.setGrid (offGridStepPpq);
    setSinglePitchPool (document);
    document.endTransaction ();
}

void configureRetriggerPerturbed (PatternDocument& document)
{
    configureRetrigger (document);
    document.setLaneValue (patternA, LaneId::vel, 5, 101); // ONE lane value
}

// ── B: single pitch, LEN 150 % (the #46 shape) ───────────────────────────────

constexpr int tiedLenPercent = 150;

/** 150 % of 1200 — the gate `emitStep` resolves from `tiedLenPercent`, and the
    number every "natural end" literal below is written against.

    IT IS ALSO WHY CASES D AND E FLUSH SEVERAL NOTES AT ONCE, which is deliberate.
    An 1800-sample gate spans one and a half steps, so at a flush point two
    consecutive steps' notes are typically both still live and both get cut on the
    SAME sample. Until issue #51 those two offs came out in a buffer-size-dependent
    order and these cases had to be detuned to 110 % to cut exactly one note each;
    order-preserving removal made that workaround unnecessary, and keeping it would
    have meant the contract cases routed around a path real music takes constantly.
    See case G for the guard that keeps the ordering honest. */
constexpr std::int64_t tiedGateSamples = 1800;

void configureTied (PatternDocument& document)
{
    document.beginTransaction ();
    document.setGrid (offGridStepPpq);
    setSinglePitchPool (document);
    fillLane (document, patternA, LaneId::len, tiedLenPercent);
    document.endTransaction ();
}

void configureTiedPerturbed (PatternDocument& document)
{
    configureTied (document);
    document.setLaneValue (patternA, LaneId::vel, 5, 101);
}

/** THE NEGATIVE CONTROL FOR B's REACHABILITY COUNTER: the identical LEN 150 %
    configuration over the DEFAULT 8-note pool, so a pitch recurs only every 8 steps
    (9600 samples) — far beyond the 1800-sample gate. No same-pitch cutoff is
    possible, so the cutoff counter must read 0. */
void configureTiedWideRecurrence (PatternDocument& document)
{
    document.beginTransaction ();
    document.setGrid (offGridStepPpq);
    fillLane (document, patternA, LaneId::len, tiedLenPercent);
    document.endTransaction ();
}

// ── C: the MID-BLOCK switch flush (the #48 shape) ────────────────────────────

/** Pattern A's GATE length, and therefore its `patternEnd` period. 61 is chosen so
    the switch resolves to STEP 61 — see the derivation in the case itself. */
constexpr int switchGatePeriod = 61;

void configureMidBlockSwitch (PatternDocument& document)
{
    document.beginTransaction ();
    document.setGrid (offGridStepPpq);

    gateOnWithPeriod (document, patternA, switchGatePeriod);
    gateOnWithPeriod (document, patternB, switchGatePeriod);
    fillLane (document, patternB, LaneId::vel, velocityB);

    document.endTransaction ();
}

void configureMidBlockSwitchPerturbed (PatternDocument& document)
{
    configureMidBlockSwitch (document);
    document.setLaneValue (patternA, LaneId::vel, 5, 101);
}

// ── D: the switch flush landing exactly ON a block head (pre-flush) ──────────

/** Pattern A's GATE length for case D: 64 makes the `patternEnd` period 8.0 PPQ, so
    a command at 245760 (PPQ 25.6) resolves to PPQ 32.0 = step 256 = sample 307200 —
    the smallest step boundary that is a block head at all ten swept sizes. */
constexpr int headSwitchGatePeriod = 64;

void configureBlockHeadSwitch (PatternDocument& document)
{
    document.beginTransaction ();
    document.setGrid (offGridStepPpq);

    gateOnWithPeriod (document, patternA, headSwitchGatePeriod);
    gateOnWithPeriod (document, patternB, headSwitchGatePeriod);
    fillLane (document, patternA, LaneId::len, tiedLenPercent);
    fillLane (document, patternB, LaneId::len, tiedLenPercent);
    fillLane (document, patternB, LaneId::vel, velocityB);

    document.endTransaction ();
}

void configureBlockHeadSwitchPerturbed (PatternDocument& document)
{
    configureBlockHeadSwitch (document);
    document.setLaneValue (patternA, LaneId::vel, 5, 101);
}

// ── E: discontinuity flushes (stop and locate) ───────────────────────────────

/** The default 8-note pool at LEN 150 %, which leaves TWO notes straddling any flush
    point — the shape case E uses for its stop and locate, and case G for its
    dedicated ordering guard. */
void configureDiscontinuity (PatternDocument& document)
{
    document.beginTransaction ();
    document.setGrid (offGridStepPpq);
    fillLane (document, patternA, LaneId::len, tiedLenPercent);
    document.endTransaction ();
}

void configureDiscontinuityPerturbed (PatternDocument& document)
{
    configureDiscontinuity (document);
    document.setLaneValue (patternA, LaneId::vel, 5, 101);
}

// ── F: everything at once ────────────────────────────────────────────────────

/** PITCH lane of LENGTH 2, `{0, -1}`. With the 8-note pool and `DirectionMode::up`
    the pool DEGREE becomes `n % 8 + (n odd ? -1 : 0)`, which pairs each EVEN step
    with the ODD step after it on the same pitch:

        n:      0  1  2  3  4  5  6  7  8  9 …
        degree: 0  0  2  2  4  4  6  6  0  0 …

    That is what makes a same-pitch retrigger reachable while pitches STILL recur
    only every 8 steps otherwise — so the flush in this case sees both an
    already-ended entry and a still-sounding one. */
constexpr int mixedPitchLane[] = { 0, -1 };

/** LEN lane of LENGTH 2, `{150, 50}`: the EVEN step of each pair is a TIE that the
    odd step cuts short (the #46 shape), the ODD step ends naturally well before its
    pitch returns (the already-ended entry the #48 flush must place correctly). */
constexpr int mixedLenLane[] = { tiedLenPercent, 50 };

void configureMixed (PatternDocument& document)
{
    document.beginTransaction ();
    document.setGrid (offGridStepPpq);

    for (const int index : { patternA, patternB })
    {
        gateOnWithPeriod (document, index, switchGatePeriod);

        document.setLaneLength (index, LaneId::pitch, 2);
        document.setLaneLength (index, LaneId::len, 2);

        for (int step = 0; step < 2; ++step)
        {
            document.setLaneValue (index, LaneId::pitch, step, mixedPitchLane[step]);
            document.setLaneValue (index, LaneId::len, step, mixedLenLane[step]);
        }
    }

    fillLane (document, patternB, LaneId::vel, velocityB);

    document.endTransaction ();
}

void configureMixedPerturbed (PatternDocument& document)
{
    configureMixed (document);
    document.setLaneValue (patternA, LaneId::vel, 5, 101);
}

/** The pool degree case F's pitch lane produces at global step `n` — written in the
    TEST, independently of `PatternSnapshot::poolIndexAt`. */
int mixedDegree (int n) noexcept
{
    return (n % poolSize) + mixedPitchLane[n % 2];
}

/** …and the MIDI note it resolves to. Every degree here is inside `[0, poolSize)`,
    so no octave carry is involved. */
int mixedPitch (int n) noexcept
{
    return poolPitches[mixedDegree (n)];
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 0. The clock, asserted once for the whole file
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/note-off: the 1/32 clock puts three steps inside one 4096-sample block",
           "[unit][midi-conformance][determinism]")
{
    // Phase 5 lesson: a test silently ran at the default 120 BPM and was caught only
    // because absolute positions were asserted as literals. The tempo is pinned to
    // the transport's own maximum and the grid arithmetic to numbers.
    REQUIRE (offBpm == Transport::maxBpm);

    // Approx, not `==`: 60/300 is inexact in binary, so exact equality would assert
    // a rounding accident. The margin is far below one sample.
    REQUIRE (samplesPerStepAt (offGridStepPpq, offBpm, offSampleRate) == Catch::Approx (1200.0).margin (1.0e-9));
    REQUIRE (stepSamples == 1200);

    // THE REACHABILITY PRECONDITION FOR THE WHOLE FILE. At the 1/16 scaffold grid
    // the step would be 2400 samples and none of the six cases below could enter the
    // branch it names — which is exactly why pattern_switch.cpp (6000-sample steps)
    // never did.
    REQUIRE (3 * stepSamples < probeBlockSize);
    REQUIRE (probeBlockSize == 4096);

    // The alignment unit really is the lcm: every swept size divides it, and it is
    // the SMALLEST positive multiple of 32 with that property.
    REQUIRE (isHeadEverywhere (alignmentUnit));
    REQUIRE (headCount (alignmentUnit) == numSweptBlockSizes);
    REQUIRE (numSweptBlockSizes == 10);

    // ── THE PROPERTY CASE C RESTS ON, PROVED HERE ────────────────────────────
    // An ODD multiple of 1200 is a block head at NO swept block size, and an EVEN
    // one is a head at 32 (and sometimes more). That is what lets case C put a
    // pattern-switch flush strictly MID-BLOCK at all ten sizes — the shape the six
    // goldens, whose switches all land on 61440-aligned heads, cannot produce.
    int oddHeads = 0;
    int evenHeadTotal = 0;

    for (int n = 1; n <= 128; ++n)
    {
        const std::int64_t boundary = static_cast<std::int64_t> (n) * stepSamples;

        if (n % 2 == 1)
            oddHeads += headCount (boundary);
        else
            evenHeadTotal += headCount (boundary);
    }

    REQUIRE (oddHeads == 0);            // no odd step boundary is a head anywhere
    REQUIRE (evenHeadTotal > 0);        // …and the even ones are, so the claim is not vacuous
    REQUIRE (headCount (73200) == 0);   // case C's adopt sample: mid-block everywhere
    REQUIRE (headCount (307200) == 10); // case D's adopt sample: a head everywhere
}

// ─────────────────────────────────────────────────────────────────────────────
// A. Natural gate ends + the ALREADY-DUE same-pitch retrigger (issue #36)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/note-off: natural gate ends and already-due retriggers keep their absolute samples",
           "[midi-conformance][determinism]")
{
    // ONE-NOTE POOL, LEN 50 %: every gated step retriggers the same pitch, and every
    // note-off is ALREADY DUE (600 samples after its on) by the time the next step
    // arrives (1200 samples after it). At 4096 the first block alone holds step 0's
    // on (0), its due off (600) and step 1's on (1200) — the exact #36 shape. At 32
    // the off and the re-on are in DIFFERENT blocks, so `emitDueNoteOffs` places the
    // off and `emitStep` never sees it. Those two must produce the same MIDI.
    constexpr std::int64_t music = 2 * alignmentUnit; // 122880
    constexpr std::int64_t span = 3 * alignmentUnit;  // 184320 — one unit past the stop flush

    // ── LITERALS, FROM FIRST PRINCIPLES ──────────────────────────────────────
    // Boundaries sit at n x 1200 and the transport plays [0, 122880): n = 0..102.
    REQUIRE (music / stepSamples == 102);
    REQUIRE (music % stepSamples == 480); // the span does NOT end on a boundary
    constexpr int expectedOns = 103;
    constexpr std::int64_t gateSamples = 600; // LEN 50 % of 1200, exactly
    REQUIRE (gateSamples == stepSamples / 2);

    // 102 gates expire inside the span; the 103rd note (on 122400, off due 123000) is
    // still sounding when the stop flushes it, which is why there is a CC123 at all.
    constexpr std::int64_t lastOn = 102 * stepSamples; // 122400
    REQUIRE (lastOn + gateSamples > music);            // 123000 > 122880

    const auto schedule = playThenStop (music);
    REQUIRE (isHeadEverywhere (music));
    REQUIRE (scheduleIsBlockAligned (schedule, sweptBlockSizes[0]));

    const auto sweep = sweepBlockSizes (&configureRetrigger, schedule, span);
    REQUIRE_SWEEP_CLEAN (sweep, 2 * expectedOns + 1, 1);

    // ── THE STREAM ITSELF, AT THE PROBE SIZE ─────────────────────────────────
    const auto render = renderAt (&configureRetrigger, schedule, span, probeBlockSize);
    const auto ons = noteOnsOf (render);
    const auto offs = noteOffsOf (render);
    INFO (render.describe (10));

    REQUIRE (static_cast<int> (ons.size ()) == expectedOns);
    REQUIRE (static_cast<int> (offs.size ()) == expectedOns);

    // THE FIX, AS A LITERAL: step 0's off belongs at 600 — its TRUE due sample — at
    // every block size. Reverting #36 puts it at 1200 (co-located with step 1's
    // re-on) wherever the buffer is large enough to hold both.
    REQUIRE (containsEvent (render, 600, noteOffCh1, poolPitches[0], 0x00));
    REQUIRE (offs.front ().absoluteSample == gateSamples);

    // …and the same statement for every step, aggregated (no macro per iteration).
    int onsAtGrid = 0;
    int offsAtGateEnd = 0;

    for (std::size_t i = 0; i < ons.size (); ++i)
    {
        const auto expectedOn = static_cast<std::int64_t> (i) * stepSamples;
        onsAtGrid += (ons[i].absoluteSample == expectedOn && ons[i].message.getNoteNumber () == poolPitches[0] &&
                      ons[i].message.getChannel () == outChannel)
                         ? 1
                         : 0;

        // The last off is the stop flush's, at the stop sample, not at a gate end.
        const auto expectedOff = (i + 1 < ons.size ()) ? expectedOn + gateSamples : music;
        offsAtGateEnd += (offs[i].absoluteSample == expectedOff) ? 1 : 0;
    }

    REQUIRE (onsAtGrid == expectedOns);
    REQUIRE (offsAtGateEnd == expectedOns);
    REQUIRE (sweepSamplesOf (render) == std::vector<std::int64_t> { music });

    // ── REACHABILITY: the #36 path really was entered ────────────────────────
    const auto positive = probeBlocks (probeBlockSize, &configureRetrigger, schedule, span, -1);
    INFO (positive.describe ());
    REQUIRE (positive.blocksRendered == span / probeBlockSize);
    REQUIRE (positive.noteOns == expectedOns);
    REQUIRE (positive.blocksWithRetriggerPair > 0);
    REQUIRE (positive.retriggerPairs > 0);

    // ── NEGATIVE CONTROL 1: the counter measures the path, not any render ─────
    // The default 8-note pool pushes a pitch's recurrence out to 8 steps = 9600
    // samples, beyond both the gate and the block, so the counter must read 0.
    const auto control = probeBlocks (probeBlockSize, &configureTiedWideRecurrence, schedule, span, -1);
    INFO (control.describe ());
    REQUIRE (control.noteOns > 0); // the control really played, too
    REQUIRE (control.blocksWithRetriggerPair == 0);
    REQUIRE (control.retriggerPairs == 0);

    // ── NEGATIVE CONTROL 2: one lane value ⇒ a DIFFERENT stream ──────────────
    const auto perturbed = renderAt (&configureRetriggerPerturbed, schedule, span, probeBlockSize);
    REQUIRE (! perturbed.empty ());
    REQUIRE (perturbed.toByteStream () != render.toByteStream ());
}

// ─────────────────────────────────────────────────────────────────────────────
// B. The STILL-SOUNDING same-pitch retrigger — LEN > 100 % (issue #46)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/note-off: a LEN>100% same-pitch cutoff lands one sample before the retrigger, always",
           "[midi-conformance][determinism]")
{
    // sequencer_retrigger.cpp runs a 50 % gate, so its outgoing note-off is ALWAYS
    // already due and only `isDueAtOrBefore` is ever taken. LEN 150 % is what makes
    // the note STILL LIVE when its pitch returns — the branch #46 lived in.
    //
    // `cutoffForSamePitch` therefore schedules each note's off at
    // `next same-pitch on - 1` = 1200n + 1199, an ABSOLUTE sample. When 1200(n+1) is
    // a block head (at 32: every EVEN step boundary) that sample lies in the
    // PREVIOUS block — a placement `jmax (0, offset - 1)` structurally could not
    // express, because it clamped to offset 0 and emitted the off one sample LATE.
    constexpr std::int64_t music = alignmentUnit;    // 61440
    constexpr std::int64_t span = 2 * alignmentUnit; // 122880
    constexpr std::int64_t cutoffOffset = 1199;      // = stepSamples - 1
    REQUIRE (cutoffOffset == stepSamples - 1);

    // ── LITERALS ─────────────────────────────────────────────────────────────
    // Boundaries n x 1200 < 61440 ⇒ n = 0..51.
    REQUIRE (music / stepSamples == 51);
    constexpr int expectedOns = 52;

    // The natural end at LEN 150 % would be 1800 samples after the on; the cutoff is
    // 1199. Asserting they DIFFER is what makes the 1199 literal meaningful.
    constexpr std::int64_t naturalGate = 1800;
    REQUIRE (naturalGate == static_cast<std::int64_t> (1.5 * static_cast<double> (stepSamples)));
    REQUIRE (naturalGate != cutoffOffset);

    const auto schedule = playThenStop (music);
    const auto sweep = sweepBlockSizes (&configureTied, schedule, span);

    // 52 ons + 51 cutoffs + 1 stop-flush off + 1 CC123 = 105.
    REQUIRE_SWEEP_CLEAN (sweep, 2 * expectedOns + 1, 1);

    const auto render = renderAt (&configureTied, schedule, span, probeBlockSize);
    const auto ons = noteOnsOf (render);
    const auto offs = noteOffsOf (render);
    INFO (render.describe (10));

    REQUIRE (static_cast<int> (ons.size ()) == expectedOns);
    REQUIRE (static_cast<int> (offs.size ()) == expectedOns);

    // THE LITERAL THIS CASE EXISTS FOR: step 0's off at 1199, not 1200 and not 1800.
    REQUIRE (containsEvent (render, cutoffOffset, noteOffCh1, poolPitches[0], 0x00));
    REQUIRE (! containsEvent (render, stepSamples, noteOffCh1, poolPitches[0], 0x00));
    REQUIRE (! containsEvent (render, naturalGate, noteOffCh1, poolPitches[0], 0x00));

    int onsAtGrid = 0;
    int offsAtCutoff = 0;
    int cutoffsAtBlockHeadBoundary32 = 0;
    int cutoffsAtBlockHeadBoundary4096 = 0;

    for (std::size_t i = 0; i < ons.size (); ++i)
    {
        const auto expectedOn = static_cast<std::int64_t> (i) * stepSamples;
        onsAtGrid += (ons[i].absoluteSample == expectedOn) ? 1 : 0;

        // The last note is cut by the stop flush, not by a same-pitch onset.
        const bool isLast = (i + 1 == ons.size ());
        const auto expectedOff = isLast ? music : expectedOn + cutoffOffset;
        offsAtCutoff += (offs[i].absoluteSample == expectedOff) ? 1 : 0;

        // ── REACHABILITY, AND WHY IT IS ASYMMETRIC ───────────────────────────
        // The bug bites exactly when the retriggering onset is a BLOCK HEAD, so the
        // cutoff sample `on - 1` belongs to the previous block. Counting those per
        // size shows the two ends of the sweep disagree about which boundaries are
        // heads — which is the mechanism, made visible.
        if (! isLast)
        {
            const std::int64_t nextOn = expectedOn + stepSamples;
            cutoffsAtBlockHeadBoundary32 += (nextOn % 32 == 0) ? 1 : 0;
            cutoffsAtBlockHeadBoundary4096 += (nextOn % 4096 == 0) ? 1 : 0;
        }
    }

    REQUIRE (onsAtGrid == expectedOns);
    REQUIRE (offsAtCutoff == expectedOns);

    // At 32 half the boundaries are heads; at 4096 none is. Same music, and the
    // engine must not care — which is exactly what the sweep above proved.
    REQUIRE (cutoffsAtBlockHeadBoundary32 > 0);
    REQUIRE (cutoffsAtBlockHeadBoundary4096 == 0);

    // ── REACHABILITY: the lookahead really fired ─────────────────────────────
    // A note-off at `on + 1199` can ONLY come from `cutoffForSamePitch`; a natural
    // LEN 150 % end is at `on + 1800`. Counting them separates the two.
    int cutoffOffs = 0;
    int naturalOffs = 0;

    for (const auto& off : offs)
    {
        cutoffOffs += (off.absoluteSample % stepSamples == cutoffOffset) ? 1 : 0;
        naturalOffs += (off.absoluteSample % stepSamples == naturalGate % stepSamples &&
                        off.absoluteSample % stepSamples != cutoffOffset)
                           ? 1
                           : 0;
    }

    INFO ("cutoff-shaped offs: " << cutoffOffs << ", natural-shaped offs: " << naturalOffs);
    REQUIRE (cutoffOffs == expectedOns - 1);
    REQUIRE (naturalOffs == 0);

    // ── NEGATIVE CONTROL: recurrence beyond the gate ⇒ no cutoff at all ──────
    const auto wide = renderAt (&configureTiedWideRecurrence, schedule, span, probeBlockSize);
    int wideCutoffs = 0;
    int wideNaturals = 0;

    for (const auto& off : noteOffsOf (wide))
    {
        wideCutoffs += (off.absoluteSample % stepSamples == cutoffOffset) ? 1 : 0;
        wideNaturals += (off.absoluteSample % stepSamples == naturalGate % stepSamples) ? 1 : 0;
    }

    INFO (wide.describe (10));
    REQUIRE (! noteOnsOf (wide).empty ()); // the control really played
    REQUIRE (wideCutoffs == 0);
    REQUIRE (wideNaturals > 0);

    // ── NEGATIVE CONTROL: one lane value ⇒ a DIFFERENT stream ────────────────
    const auto perturbed = renderAt (&configureTiedPerturbed, schedule, span, probeBlockSize);
    REQUIRE (! perturbed.empty ());
    REQUIRE (perturbed.toByteStream () != render.toByteStream ());
}

// ─────────────────────────────────────────────────────────────────────────────
// C. The MID-BLOCK quantized-switch flush (issue #48)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/note-off: a mid-block switch flush emits already-ended notes at their own samples",
           "[midi-conformance][determinism]")
{
    // ── THE DERIVATION, AND WHY EVERY NUMBER IS WHAT IT IS ───────────────────
    // The command can only be pushed on a multiple of 61440 (anything else is
    // consumed at a different absolute sample per block size). At 61440 the
    // transport is at PPQ 61440 / 9600 = 6.4.
    //
    // Pattern A's GATE length is 61, so its `patternEnd` period is 61 x 0.125 =
    // 7.625 PPQ and the switch resolves to
    //     ceil (6.4 / 7.625) x 7.625 = 7.625 PPQ  ⇒  step 61  ⇒  sample 73200.
    // 61 is ODD, and case 0 above proves no odd multiple of 1200 is a block head at
    // any swept size — so this flush is strictly MID-BLOCK at ALL TEN. That is the
    // configuration the six goldens (switches on 61440-aligned heads) cannot reach.
    //
    // LEN stays at 50 %, so NOTHING is sounding at 73200 and every entry the flush
    // finds has ALREADY ENDED. The correct stream therefore carries NO CC123 at the
    // switch — and that is the count divergence #48 produced: forcing every entry
    // through the cut-short path adds a sweep that must not be there.
    constexpr std::int64_t commandSample = alignmentUnit; // 61440, PPQ 6.4
    constexpr std::int64_t adoptSample = 73200;           // step 61
    constexpr std::int64_t music = 2 * alignmentUnit;     // 122880
    constexpr std::int64_t span = 3 * alignmentUnit;      // 184320

    REQUIRE (switchGatePeriod == 61);
    REQUIRE (adoptSample == 61 * stepSamples);
    REQUIRE (adoptSample > commandSample);
    REQUIRE (headCount (adoptSample) == 0); // MID-BLOCK at every swept size
    REQUIRE (isHeadEverywhere (commandSample));

    constexpr int expectedOns = 103; // n x 1200 < 122880 ⇒ n = 0..102
    constexpr std::int64_t gateSamples = 600;

    auto schedule = playThenStop (music);
    schedule.insert (schedule.begin () + 2,
                     ScheduledCommand { commandSample, patternSwitchCommand (patternB, QuantizeMode::patternEnd) });
    REQUIRE (scheduleIsBlockAligned (schedule, sweptBlockSizes[0]));

    const auto sweep = sweepBlockSizes (&configureMidBlockSwitch, schedule, span);

    // 103 ons + 103 offs + ONE CC123 (the stop's). A second sweep here IS the #48
    // regression, and `REQUIRE_SWEEP_CLEAN` pins both the count and the sweep count.
    REQUIRE_SWEEP_CLEAN (sweep, 2 * expectedOns + 1, 1);

    const auto render = renderAt (&configureMidBlockSwitch, schedule, span, probeBlockSize);
    const auto ons = noteOnsOf (render);
    const auto offs = noteOffsOf (render);
    const auto fromA = notesFromPattern (render, velocityA);
    const auto fromB = notesFromPattern (render, velocityB);
    INFO (render.describe (10));

    // ── ANTI-VACUITY: both patterns really played, and the switch really landed ─
    REQUIRE (static_cast<int> (ons.size ()) == expectedOns);
    REQUIRE (static_cast<int> (offs.size ()) == expectedOns);
    REQUIRE (fromA.size () + fromB.size () == ons.size ());
    REQUIRE (! fromA.empty ());
    REQUIRE (! fromB.empty ());
    REQUIRE (fromB.front ().absoluteSample == adoptSample);
    REQUIRE (fromA.back ().absoluteSample == adoptSample - stepSamples); // 72000, a clean cut
    REQUIRE (commandSample != adoptSample);                              // the command never sits on its own boundary

    // ── THE LITERALS ─────────────────────────────────────────────────────────
    // Step 0: pool[0] = 60 = 0x3C at velocity 100. Step 60: degree 60 % 8 = 4 ⇒
    // pool[4] = 67 = 0x43, still A. Step 61: degree 5 ⇒ pool[5] = 69 = 0x45, now B.
    REQUIRE (eventIs (render.events[0], 0, noteOnCh1, poolPitches[0], velocityA));
    REQUIRE (containsEvent (render, 72000, noteOnCh1, poolPitches[4], velocityA));
    REQUIRE (containsEvent (render, adoptSample, noteOnCh1, poolPitches[5], velocityB));

    // THE #48 ASSERTION, AS ABSOLUTE SAMPLES. These three notes ended on their own
    // schedule BEFORE the switch and inside the same 4096-sample block as it; each
    // must keep its own due sample. Reverting #48 drags all three to 73199 and adds
    // a CC123 there.
    REQUIRE (containsEvent (render, 70200, noteOffCh1, poolPitches[2], 0x00)); // step 58
    REQUIRE (containsEvent (render, 71400, noteOffCh1, poolPitches[3], 0x00)); // step 59
    REQUIRE (containsEvent (render, 72600, noteOffCh1, poolPitches[4], 0x00)); // step 60
    REQUIRE (! containsEvent (render, adoptSample - 1, noteOffCh1, poolPitches[2], 0x00));
    REQUIRE (! containsEvent (render, adoptSample - 1, ccCh1, allNotesOffCc, 0x00));

    // The ONLY sweep in the render is the stop's.
    REQUIRE (sweepSamplesOf (render) == std::vector<std::int64_t> { music });

    int offsAtGateEnd = 0;
    for (std::size_t i = 0; i < offs.size (); ++i)
    {
        const auto expected =
            (i + 1 < offs.size ()) ? static_cast<std::int64_t> (i) * stepSamples + gateSamples : music;
        offsAtGateEnd += (offs[i].absoluteSample == expected) ? 1 : 0;
    }
    REQUIRE (offsAtGateEnd == expectedOns);

    // ── REACHABILITY: the flush ran MID-BLOCK over ALREADY-ENDED ENTRIES ─────
    // See the note on `FlushProbe`: an off in `[markBlockStart, adoptSample)` inside
    // the flush block can only have come from `flush`'s already-ended branch. This
    // configuration has no same-pitch retriggers at all (a pitch recurs every 8
    // steps = 9600 samples, and the 50 % gate ends 600 samples after its on), so the
    // retrigger alternative is excluded outright — and the probe asserts that too.
    const auto probe4096 = probeBlocks (probeBlockSize, &configureMidBlockSwitch, schedule, span, adoptSample);
    INFO (probe4096.describe ());
    REQUIRE (probe4096.noteOns == expectedOns);
    REQUIRE (probe4096.retriggerPairs == 0); // no retrigger could have emitted these
    REQUIRE (! probe4096.markIsBlockHead);
    REQUIRE (probe4096.markBlockStart == 69632); // block 17 of [0, …) at 4096
    REQUIRE (probe4096.offsBeforeMarkInMarkBlock == 3);
    REQUIRE (probe4096.markBlockOffSamples == std::vector<std::int64_t> { 70200, 71400, 72600 });
    REQUIRE (probe4096.sweepsInMarkBlock == 0); // nothing was cut short ⇒ no sweep

    // …and the same probe at 2048, where the narrower block admits exactly ONE
    // already-ended entry. Two different non-zero counts is what says the number
    // tracks the block carving rather than being a constant.
    const auto probe2048 = probeBlocks (2048, &configureMidBlockSwitch, schedule, span, adoptSample);
    INFO (probe2048.describe ());
    REQUIRE (! probe2048.markIsBlockHead);
    REQUIRE (probe2048.markBlockStart == 71680);
    REQUIRE (probe2048.offsBeforeMarkInMarkBlock == 1);
    REQUIRE (probe2048.markBlockOffSamples == std::vector<std::int64_t> { 72600 });

    // ── THE NEGATIVE CONTROL FOR THE PROBE ITSELF ────────────────────────────
    // At 32 the flush block is only 16 samples wide, so the already-ended window is
    // empty and the counter must read 0 — proving it measures the window rather than
    // firing on any render that contains note-offs.
    const auto probe32 = probeBlocks (32, &configureMidBlockSwitch, schedule, span, adoptSample);
    INFO (probe32.describe ());
    REQUIRE (probe32.noteOns == expectedOns); // it really played
    REQUIRE (! probe32.markIsBlockHead);
    REQUIRE (probe32.markBlockStart == 73184);
    REQUIRE (probe32.offsBeforeMarkInMarkBlock == 0);

    // ── NEGATIVE CONTROL: one lane value ⇒ a DIFFERENT stream ────────────────
    const auto perturbed = renderAt (&configureMidBlockSwitchPerturbed, schedule, span, probeBlockSize);
    REQUIRE (! perturbed.empty ());
    REQUIRE (perturbed.toByteStream () != render.toByteStream ());
}

// ─────────────────────────────────────────────────────────────────────────────
// D. The switch flush landing exactly ON a block head — the PRE-FLUSH sibling
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/note-off: a switch flush on a block head emits the identical stream",
           "[midi-conformance][determinism]")
{
    // THE SIBLING PATH OF CASE C, HERE SO A FUTURE CHANGE CANNOT FIX ONE AND BREAK
    // THE OTHER SILENTLY. When `adoptSample - 1` is the LAST sample of a block, the
    // block that fires the switch could never emit it, so `processBlock` step 4
    // PRE-FLUSHES from the preceding block instead. That is the other half of the
    // #46 fix and it has no test of its own anywhere else.
    //
    // Derivation: 307200 = lcm (1200, 61440) is the SMALLEST step boundary that is a
    // block head at every swept size (step 256, PPQ 32.0). Pattern A's GATE length
    // 64 makes the `patternEnd` period 8.0 PPQ, so a command at 245760 (PPQ 25.6)
    // resolves to ceil (25.6 / 8) x 8 = 32.0 PPQ. LEN 150 % guarantees a note IS
    // sounding there, without which the pre-flush would emit nothing and this case
    // would pass whatever the engine did — and at the STOP it leaves TWO, so this
    // case exercises a multi-note flush as well (see `tiedGateSamples`).
    constexpr std::int64_t commandSample = 4 * alignmentUnit; // 245760, PPQ 25.6
    constexpr std::int64_t adoptSample = 307200;              // step 256, PPQ 32.0
    constexpr std::int64_t music = 6 * alignmentUnit;         // 368640
    constexpr std::int64_t span = 7 * alignmentUnit;          // 430080

    REQUIRE (headSwitchGatePeriod == 64);
    REQUIRE (adoptSample == 256 * stepSamples);
    REQUIRE (headCount (adoptSample) == numSweptBlockSizes); // a head EVERYWHERE
    REQUIRE (isHeadEverywhere (commandSample));

    // ── LITERALS ─────────────────────────────────────────────────────────────
    // n x 1200 < 368640 ⇒ n = 0..307.
    REQUIRE (music / stepSamples == 307);
    constexpr int expectedOns = 308;

    // WHICH NOTES EACH FLUSH CUTS, derived rather than observed. An 1800-sample gate
    // straddles one and a half steps, so the pre-flush catches ONE note (step 255)
    // and the stop catches TWO (steps 306 and 307) — the multi-note shape whose
    // emission order issue #51 made deterministic, and which case G guards directly.
    REQUIRE (tiedGateSamples == 1800);
    REQUIRE (255 * stepSamples + tiedGateSamples >= adoptSample); // 307800 >= 307200
    REQUIRE (254 * stepSamples + tiedGateSamples < adoptSample);  // 306600 <  307200
    REQUIRE (307 * stepSamples + tiedGateSamples >= music);       // 370200 >= 368640
    REQUIRE (306 * stepSamples + tiedGateSamples >= music);       // 369000 >= 368640 — the SECOND
    REQUIRE (305 * stepSamples + tiedGateSamples < music);        // 367800 <  368640 — and only two

    auto schedule = playThenStop (music);
    schedule.insert (schedule.begin () + 2,
                     ScheduledCommand { commandSample, patternSwitchCommand (patternB, QuantizeMode::patternEnd) });
    REQUIRE (scheduleIsBlockAligned (schedule, sweptBlockSizes[0]));

    const auto sweep = sweepBlockSizes (&configureBlockHeadSwitch, schedule, span);

    // 308 ons + 308 offs + TWO CC123 (the switch pre-flush at 307199, the stop at
    // 368640) = 618.
    REQUIRE_SWEEP_CLEAN (sweep, 2 * expectedOns + 2, 2);

    const auto render = renderAt (&configureBlockHeadSwitch, schedule, span, probeBlockSize);
    const auto ons = noteOnsOf (render);
    const auto fromA = notesFromPattern (render, velocityA);
    const auto fromB = notesFromPattern (render, velocityB);
    INFO (render.describe (10));

    REQUIRE (static_cast<int> (ons.size ()) == expectedOns);
    REQUIRE (static_cast<int> (noteOffsOf (render).size ()) == expectedOns);
    REQUIRE (fromA.size () + fromB.size () == ons.size ());
    REQUIRE (fromB.front ().absoluteSample == adoptSample);
    REQUIRE (fromA.back ().absoluteSample == adoptSample - stepSamples);

    // ── THE LITERALS THIS CASE EXISTS FOR ────────────────────────────────────
    // Step 0 on at 0, its natural LEN 150 % end at 1800. Step 255 (pool[255 % 8] =
    // pool[7] = 72 = 0x48) is cut short by the pre-flush at 307199 — ONE sample
    // before the adopt point, decided on the ABSOLUTE timeline in the PREVIOUS
    // block. The CC123 rides with it.
    REQUIRE (eventIs (render.events[0], 0, noteOnCh1, poolPitches[0], velocityA));
    REQUIRE (containsEvent (render, tiedGateSamples, noteOffCh1, poolPitches[0], 0x00));
    REQUIRE (containsEvent (render, adoptSample - 1, noteOffCh1, poolPitches[7], 0x00));
    REQUIRE (containsEvent (render, adoptSample - 1, ccCh1, allNotesOffCc, 0x00));
    REQUIRE (containsEvent (render, adoptSample, noteOnCh1, poolPitches[0], velocityB));

    // Exactly two sweeps, at exactly these samples.
    REQUIRE (sweepSamplesOf (render) == std::vector<std::int64_t> { adoptSample - 1, music });

    // ── THE MULTI-NOTE STOP FLUSH, AS AN ORDERED BYTE SEQUENCE (issue #51) ────
    // Steps 306 (pool[306 % 8] = pool[2] = 64 = 0x40) and 307 (pool[3] = 65 = 0x41)
    // are both live at the stop. Registration order is 306 then 307, so that is the
    // order they are emitted in — at every buffer size, which is the property #51
    // established and the `streamsMatched` leg of the sweep above enforces.
    REQUIRE (byteSequenceAt (render, music) == std::vector<int> { noteOffCh1,
                                                                  poolPitches[306 % poolSize],
                                                                  0x00,
                                                                  noteOffCh1,
                                                                  poolPitches[307 % poolSize],
                                                                  0x00,
                                                                  ccCh1,
                                                                  allNotesOffCc,
                                                                  0x00 });

    // ── REACHABILITY: the PRE-FLUSH path, at every swept size ────────────────
    // `adoptSample` is a block head everywhere, so `adoptSample - 1` is always the
    // previous block's last sample and the pre-flush is the path taken at all ten.
    int preFlushSizes = 0;
    for (const int blockSize : sweptBlockSizes)
        preFlushSizes += (adoptSample % blockSize == 0) ? 1 : 0;

    REQUIRE (preFlushSizes == numSweptBlockSizes);

    const auto probe = probeBlocks (probeBlockSize, &configureBlockHeadSwitch, schedule, span, adoptSample);
    INFO (probe.describe ());
    REQUIRE (probe.noteOns == expectedOns);
    REQUIRE (probe.markIsBlockHead);                // the pre-flush precondition
    REQUIRE (probe.markBlockStart == adoptSample);  // …so the window below is empty
    REQUIRE (probe.offsBeforeMarkInMarkBlock == 0); // the flush did NOT run mid-block
    REQUIRE (probe.sweepsInMarkBlock == 0);         // its sweep landed in the PREVIOUS block

    // ── NEGATIVE CONTROL: one lane value ⇒ a DIFFERENT stream ────────────────
    const auto perturbed = renderAt (&configureBlockHeadSwitchPerturbed, schedule, span, probeBlockSize);
    REQUIRE (! perturbed.empty ());
    REQUIRE (perturbed.toByteStream () != render.toByteStream ());
}

// ─────────────────────────────────────────────────────────────────────────────
// E. The discontinuity flush — stop and locate
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/note-off: stop and locate flushes are byte-identical at every block size",
           "[midi-conformance][determinism]")
{
    // The third flush caller (`handleDiscontinuities`). It always releases from the
    // BLOCK HEAD, so the table's already-ended window is empty by construction and
    // every entry is cut short and swept. That is a genuinely different shape from
    // C and D, and it is the one every §5.5 flush point outside a pattern switch
    // takes — so it belongs in a file about the whole class.
    //
    // LEN 150 % (`tiedGateSamples`), so TWO notes are sounding at each of the two
    // flush points and each flush emits two offs on ONE sample plus the sweep. That
    // is the everyday shape — a 1.5-step gate is ordinary tie/legato — and pinning
    // its exact byte order here is only possible because issue #51 made the emission
    // order registration order rather than a function of the block carving.
    //
    // A locate to PPQ 9.0 is exactly step 72 on this grid, so the new leg's first
    // step lands at offset 0 of the locate block and the arithmetic below stays exact.
    constexpr std::int64_t locateAt = alignmentUnit;  // 61440
    constexpr double locateTargetPpq = 9.0;           // = step 72 at 0.125 PPQ/step
    constexpr std::int64_t music = 2 * alignmentUnit; // 122880
    constexpr std::int64_t span = 3 * alignmentUnit;  // 184320

    REQUIRE (locateTargetPpq / offGridStepPpq == Catch::Approx (72.0).margin (1.0e-9));
    REQUIRE (isHeadEverywhere (locateAt));
    REQUIRE (isHeadEverywhere (music));

    // ── LITERALS ─────────────────────────────────────────────────────────────
    // Leg 1: boundaries n x 1200 < 61440 ⇒ n = 0..51 ⇒ 52 ons. At the locate, TWO
    // notes are still sounding — steps 50 (60000 + 1800 = 61800) and 51 (63000) —
    // while step 49's (60600) has already drained. Both are cut short and swept.
    // Leg 2: 52 more ons at 61440 + 1200k, and the stop cuts two the same way.
    constexpr int onsPerLeg = 52;
    constexpr int expectedOns = 2 * onsPerLeg;
    REQUIRE (51 * stepSamples + tiedGateSamples >= locateAt); // 63000 >= 61440 — still sounding
    REQUIRE (50 * stepSamples + tiedGateSamples >= locateAt); // 61800 >= 61440 — and so is this
    REQUIRE (49 * stepSamples + tiedGateSamples < locateAt);  // 60600 <  61440 — and only two

    // Leg 2 restarts the sample arithmetic at `locateAt`, and the stop sits the same
    // 61440 samples later, so the same three inequalities hold for it.
    REQUIRE (locateAt + 51 * stepSamples + tiedGateSamples >= music); // 124440 >= 122880
    REQUIRE (locateAt + 50 * stepSamples + tiedGateSamples >= music); // 123240 >= 122880
    REQUIRE (locateAt + 49 * stepSamples + tiedGateSamples < music);  // 122040 <  122880

    std::vector<ScheduledCommand> schedule = {
        ScheduledCommand { 0, engineCommand (EngineCommandType::setTempoBpm, offBpm) },
        ScheduledCommand { 0, engineCommand (EngineCommandType::transportPlay) },
        ScheduledCommand { locateAt, engineCommand (EngineCommandType::transportLocate, locateTargetPpq) },
        ScheduledCommand { music, engineCommand (EngineCommandType::transportStop) }
    };
    REQUIRE (scheduleIsBlockAligned (schedule, sweptBlockSizes[0]));

    const auto sweep = sweepBlockSizes (&configureDiscontinuity, schedule, span);

    // 104 ons + 104 offs + 2 CC123 = 210.
    REQUIRE_SWEEP_CLEAN (sweep, 2 * expectedOns + 2, 2);

    const auto render = renderAt (&configureDiscontinuity, schedule, span, probeBlockSize);
    const auto ons = noteOnsOf (render);
    INFO (render.describe (10));

    REQUIRE (static_cast<int> (ons.size ()) == expectedOns);
    REQUIRE (static_cast<int> (noteOffsOf (render).size ()) == expectedOns);

    // ── THE LITERALS, INCLUDING EMISSION ORDER (issue #51) ───────────────────
    // Both flushes land ON their block head and each cuts TWO notes. Leg 1 cuts steps
    // 50 (pool[2] = 64 = 0x40) and 51 (pool[3] = 65 = 0x41); leg 2's last two are
    // indices 72 + 50 = 122 (pool[2]) and 72 + 51 = 123 (pool[3]).
    //
    // The whole byte sequence at each flush sample is pinned, ORDER INCLUDED: two
    // per-note offs oldest-registered-first, then the sweep. `handleDiscontinuities`
    // runs before the step walk, so on the locate sample the flush precedes the new
    // leg's first note-on — which is asserted separately below rather than folded in
    // here, because `byteSequenceAt` covers every event on the sample.
    REQUIRE (eventIs (render.events[0], 0, noteOnCh1, poolPitches[0], velocityA));
    REQUIRE (containsEvent (render, tiedGateSamples, noteOffCh1, poolPitches[0], 0x00)); // a natural LEN 150 % end

    REQUIRE (byteSequenceAt (render, locateAt) == std::vector<int> { noteOffCh1,
                                                                     poolPitches[50 % poolSize],
                                                                     0x00, // step 50, registered first
                                                                     noteOffCh1,
                                                                     poolPitches[51 % poolSize],
                                                                     0x00, // step 51
                                                                     ccCh1,
                                                                     allNotesOffCc,
                                                                     0x00, // the sweep
                                                                     noteOnCh1,
                                                                     poolPitches[72 % poolSize],
                                                                     velocityA }); // the new leg, after it

    REQUIRE (byteSequenceAt (render, music) == std::vector<int> { noteOffCh1,
                                                                  poolPitches[122 % poolSize],
                                                                  0x00,
                                                                  noteOffCh1,
                                                                  poolPitches[123 % poolSize],
                                                                  0x00,
                                                                  ccCh1,
                                                                  allNotesOffCc,
                                                                  0x00 });

    // The locate does NOT stop the transport: step 72 starts the new leg on the very
    // same sample, and it is a note-ON, so nothing was left hanging by the flush.
    REQUIRE (containsEvent (render, locateAt, noteOnCh1, poolPitches[72 % poolSize], velocityA));

    REQUIRE (sweepSamplesOf (render) == std::vector<std::int64_t> { locateAt, music });

    int offsAtFlushPoints = 0;
    for (const auto& off : noteOffsOf (render))
        offsAtFlushPoints += (off.absoluteSample == locateAt || off.absoluteSample == music) ? 1 : 0;

    // ── REACHABILITY: both flushes really cut notes short ────────────────────
    // TWO per flush; without this the two CC123s could be a sweep with no offs
    // behind them, which is exactly the failure §5.5 calls out (hosted plugins
    // honour CC123 inconsistently, so the per-note offs are the real mechanism).
    INFO ("offs landing on a flush point: " << offsAtFlushPoints);
    REQUIRE (offsAtFlushPoints == 4);

    // ── NEGATIVE CONTROL: one lane value ⇒ a DIFFERENT stream ────────────────
    const auto perturbed = renderAt (&configureDiscontinuityPerturbed, schedule, span, probeBlockSize);
    REQUIRE (! perturbed.empty ());
    REQUIRE (perturbed.toByteStream () != render.toByteStream ());
}

// ─────────────────────────────────────────────────────────────────────────────
// F. All of it in one render — because #48's spurious CC123 only appeared in
//    combination
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/note-off: retriggers, ties, a mid-block switch and a stop stay byte-identical together",
           "[midi-conformance][determinism]")
{
    // ── THE ONE RENDER THAT CONTAINS EVERY PATH ──────────────────────────────
    // PITCH lane {0, -1} of length 2 pairs each EVEN step with the ODD step after it
    // on one pitch; LEN lane {150, 50} of length 2 makes the even one a TIE the odd
    // one cuts short and the odd one a note that ends WELL BEFORE its pitch returns
    // (8 steps later). So one render carries, simultaneously:
    //
    //   • a same-pitch cutoff at `next on - 1` on every even step   (the #46 shape)
    //   • an ALREADY-DUE retrigger at every odd step                (the #36 shape)
    //   • odd steps' notes ending naturally                         (emitDueNoteOffs)
    //   • a MID-BLOCK switch flush at step 61 that sees BOTH an already-ended entry
    //     (step 59, due 71400) AND a still-sounding one (step 60)   (the #48 shape)
    //   • a terminating stop flush
    //
    // WHICH DEFECTS THIS CASE REDDENS FOR, MEASURED RATHER THAN ASSUMED: #36 (the
    // literal at `evenCutoff` below moves from 1199 to 1200) and #48 (the canonical
    // stream diverges at 4096). It does NOT redden for #46, and the reason is worth
    // stating so nobody assumes otherwise: the #46 collapse only bites when the
    // RETRIGGERING ONSET is a block head, and this pairing puts every retrigger on an
    // ODD step boundary — which case 0 proves is a head at no swept size. #46 is
    // covered squarely by case B, which retriggers on every step and therefore hits
    // the even boundaries too.
    //
    // THE STEP-60 DETAIL IS LOAD-BEARING. Its pair partner is step 61 — the adopt
    // step — and `cutoffForSamePitch` STOPS at a resolved switch, so step 60 keeps
    // its natural 73800 end instead of a 73199 cutoff and is therefore CUT SHORT by
    // the flush. That is what puts a CC123 at 73199 next to an already-ended off at
    // 71400: the exact combination #48 corrupted.
    constexpr std::int64_t commandSample = alignmentUnit; // 61440, PPQ 6.4
    constexpr std::int64_t adoptSample = 73200;           // step 61 — odd, mid-block everywhere
    constexpr std::int64_t music = 2 * alignmentUnit;     // 122880
    constexpr std::int64_t span = 3 * alignmentUnit;      // 184320

    REQUIRE (headCount (adoptSample) == 0);

    // ── THE PITCH PAIRING, DERIVED IN THE TEST ───────────────────────────────
    REQUIRE (mixedDegree (0) == mixedDegree (1));
    REQUIRE (mixedDegree (2) == mixedDegree (3));
    REQUIRE (mixedDegree (0) != mixedDegree (2));
    REQUIRE (mixedDegree (58) == mixedDegree (59)); // pool[2] = 64
    REQUIRE (mixedDegree (60) == mixedDegree (61)); // pool[4] = 67 — the adopt pair
    REQUIRE (mixedPitch (58) == poolPitches[2]);
    REQUIRE (mixedPitch (60) == poolPitches[4]);

    // ── LITERALS ─────────────────────────────────────────────────────────────
    constexpr int expectedOns = 103;             // n x 1200 < 122880 ⇒ n = 0..102
    constexpr std::int64_t evenCutoff = 1199;    // even step: cut at the odd step's on - 1
    constexpr std::int64_t oddGate = 600;        // odd step: LEN 50 % natural end
    constexpr std::int64_t alreadyEnded = 71400; // step 59's own due sample
    constexpr std::int64_t cutShort = 73199;     // step 60, cut by the switch flush

    REQUIRE (alreadyEnded == 59 * stepSamples + oddGate);
    REQUIRE (cutShort == adoptSample - 1);
    REQUIRE (alreadyEnded < adoptSample);

    auto schedule = playThenStop (music);
    schedule.insert (schedule.begin () + 2,
                     ScheduledCommand { commandSample, patternSwitchCommand (patternB, QuantizeMode::patternEnd) });
    REQUIRE (scheduleIsBlockAligned (schedule, sweptBlockSizes[0]));

    const auto sweep = sweepBlockSizes (&configureMixed, schedule, span);

    // 103 ons + 103 offs + TWO CC123 (switch at 73199, stop at 122880) = 208.
    REQUIRE_SWEEP_CLEAN (sweep, 2 * expectedOns + 2, 2);

    const auto render = renderAt (&configureMixed, schedule, span, probeBlockSize);
    const auto ons = noteOnsOf (render);
    const auto offs = noteOffsOf (render);
    const auto fromA = notesFromPattern (render, velocityA);
    const auto fromB = notesFromPattern (render, velocityB);
    INFO (render.describe (12));

    REQUIRE (static_cast<int> (ons.size ()) == expectedOns);
    REQUIRE (static_cast<int> (offs.size ()) == expectedOns);
    REQUIRE (fromA.size () + fromB.size () == ons.size ());
    REQUIRE (fromB.front ().absoluteSample == adoptSample);

    // ── THE LITERALS, EACH NAMING ITS PATH ───────────────────────────────────
    REQUIRE (eventIs (render.events[0], 0, noteOnCh1, poolPitches[0], velocityA));
    REQUIRE (containsEvent (render, evenCutoff, noteOffCh1, mixedPitch (0), 0x00));            // #46 shape
    REQUIRE (containsEvent (render, stepSamples, noteOnCh1, mixedPitch (1), velocityA));       // the retrigger
    REQUIRE (containsEvent (render, stepSamples + oddGate, noteOffCh1, mixedPitch (1), 0x00)); // natural end
    REQUIRE (containsEvent (render, alreadyEnded, noteOffCh1, mixedPitch (59), 0x00)); // #48: kept its own sample
    REQUIRE (containsEvent (render, cutShort, noteOffCh1, mixedPitch (60), 0x00));     // cut by the flush
    REQUIRE (containsEvent (render, cutShort, ccCh1, allNotesOffCc, 0x00));            // …and swept
    REQUIRE (containsEvent (render, adoptSample, noteOnCh1, mixedPitch (61), velocityB));

    // Exactly two sweeps, at exactly these samples. A THIRD (or a moved one) is the
    // #48 signature in this configuration.
    REQUIRE (sweepSamplesOf (render) == std::vector<std::int64_t> { cutShort, music });

    // The whole off stream, aggregated against the closed form written above.
    int offsAsDerived = 0;
    for (int n = 0; n < expectedOns; ++n)
    {
        const auto on = static_cast<std::int64_t> (n) * stepSamples;
        std::int64_t expected = (n % 2 == 0) ? on + evenCutoff : on + oddGate;

        if (n == 60)
            expected = cutShort; // the switch flush, not the (suppressed) lookahead
        else if (expected >= music)
            expected = music; // the stop flush

        offsAsDerived += containsEvent (render, expected, noteOffCh1, mixedPitch (n), 0x00) ? 1 : 0;
    }

    INFO ("offs matching the closed form: " << offsAsDerived << " of " << expectedOns);
    REQUIRE (offsAsDerived == expectedOns);

    // ── REACHABILITY: every path was entered, in this one render ─────────────
    const auto probe = probeBlocks (probeBlockSize, &configureMixed, schedule, span, adoptSample);
    INFO (probe.describe ());
    REQUIRE (probe.noteOns == expectedOns);

    // (a) the retrigger path (#36 / #46).
    REQUIRE (probe.retriggerPairs > 0);
    REQUIRE (probe.blocksWithRetriggerPair > 0);

    // (b) the MID-BLOCK flush over an already-ended entry (#48). THREE offs precede
    //     the mark inside the flush block, and naming each is the point — an
    //     unexplained count would not be evidence of anything:
    //       70799  step 58, emitted by the RETRIGGER at step 59 (accounted for, not
    //              assumed away);
    //       71400  step 59, which NOTHING BUT `flush`'s already-ended branch could
    //              have emitted — its pitch does not recur until step 66 at 79200, so
    //              no retrigger can claim it, and `emitDueNoteOffs` runs only after
    //              the flush has already emptied the table. THIS IS THE #48 PROOF;
    //       73199  step 60, the still-sounding entry the flush CUT SHORT. It sits at
    //              `mark - 1`, hence inside the window too.
    REQUIRE (! probe.markIsBlockHead);
    REQUIRE (probe.markBlockStart == 69632);
    REQUIRE (probe.offsBeforeMarkInMarkBlock == 3);
    REQUIRE (probe.markBlockOffSamples ==
             std::vector<std::int64_t> { 58 * stepSamples + evenCutoff, alreadyEnded, cutShort });

    // (c) the still-sounding entry really was cut short and swept, in that same block.
    REQUIRE (probe.sweepsInMarkBlock == 1);

    // ── THE NEGATIVE CONTROL FOR THE PROBE ───────────────────────────────────
    // At 32 the flush block is 16 samples wide, so the already-ended window holds
    // NOTHING: steps 58 and 59 were both emitted in earlier blocks, and the only off
    // left before the mark is the cut-short one at `mark - 1`. The counter dropping
    // from three to one is what says it measures the window rather than firing on any
    // render that contains note-offs — and the sweep still being there says the
    // still-sounding half of the flush is unaffected by the carving.
    const auto probe32 = probeBlocks (32, &configureMixed, schedule, span, adoptSample);
    INFO (probe32.describe ());
    REQUIRE (probe32.noteOns == expectedOns);
    REQUIRE (probe32.markBlockStart == 73184);
    REQUIRE (probe32.offsBeforeMarkInMarkBlock == 1);
    REQUIRE (probe32.markBlockOffSamples == std::vector<std::int64_t> { cutShort });
    REQUIRE (probe32.sweepsInMarkBlock == 1);

    // ── NEGATIVE CONTROL: one lane value ⇒ a DIFFERENT stream ────────────────
    const auto perturbed = renderAt (&configureMixedPerturbed, schedule, span, probeBlockSize);
    REQUIRE (! perturbed.empty ());
    REQUIRE (perturbed.toByteStream () != render.toByteStream ());
}

// ─────────────────────────────────────────────────────────────────────────────
// G. EMISSION ORDER WITHIN ONE SAMPLE — the dedicated guard for issue #51
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/note-off: a multi-note flush emits its offs in registration order at every block size",
           "[midi-conformance][determinism]")
{
    // ── WHAT THIS CASE GUARDS, AND THE DEFECT THAT PRODUCED IT ───────────────
    // Written while building this file, and RED on its first run at exactly one of
    // the ten swept sizes. It was NOT the #36/#46/#48 family: no note-off moved to a
    // different sample, no event appeared or disappeared, and the CC123 landed
    // identically. What differed was the ORDER of two note-offs sharing ONE absolute
    // sample.
    //
    // THE DEFECT (issue #51). `SoundingNoteTable::flush` emits in table STORAGE
    // order, and `removeAt` used to vacate a slot by swapping the LAST entry into it.
    // Storage order was therefore a function of WHICH ENTRIES HAD BEEN REMOVED
    // EARLIER — i.e. of how the render happened to be carved into blocks. Two
    // survivors reaching one flush could come out in either order, and
    // `juce::MidiBuffer` preserves insertion order among equal timestamps, so the
    // difference reached the emitted stream. Observed here at 300 BPM / 48 kHz on the
    // 1/32 grid, LEN 150 %, stop at 61440: steps 50 and 51 (pool[2] = 64, pool[3] =
    // 65) came out `64, 65` at nine sizes and `65, 64` at 4096.
    //
    // WHY IT MATTERED DESPITE BEING MUSICALLY INERT. Two note-offs on different
    // pitches at one sample are indistinguishable to a synth. But §1.2's contract is
    // BYTE-IDENTICAL MIDI and `tests/golden/` compares byte streams in EMISSION
    // order, so any golden covering a multi-note flush was order-sensitive and passed
    // only by luck of the carving it was baked at. The six goldens survived because
    // their flushes happened to be stable, not because the engine guaranteed it.
    //
    // THE FIX (issue #51), which this case now guards permanently: `removeAt` shifts
    // the tail down instead of swapping, so storage order is always the ADD-ORDER
    // SUBSEQUENCE of the live set — a property of the music alone. `emitDueNoteOffs`
    // was switched from a backward to a forward walk to match; THE TWO HALVES ARE
    // COUPLED (the backward walk was only correct under swap-with-last), so a
    // fails-without check must revert both.
    //
    // WHAT CHANGED IN THIS FILE WHEN THE FIX LANDED. While the defect stood, cases D
    // and E were detuned to LEN 110 % so each flush cut exactly one note and the
    // ambiguity could not arise; this case asserted only the canonical (sorted)
    // stream and surfaced strict order through `INFO`, which meant it OBSERVED the
    // defect without FAILING on it — the same shape that let #48 hide for a phase.
    // Both are gone: D and E are back at LEN 150 % and flush two notes at once, and
    // the strict `streamsMatched` comparison below is a REQUIRE. This case remains as
    // the minimal, directly-named guard, and it additionally pins the byte order
    // itself rather than only the cross-size agreement — so it reddens both if the
    // order becomes carving-dependent again AND if it becomes stably wrong.
    constexpr std::int64_t music = alignmentUnit;    // 61440
    constexpr std::int64_t span = 2 * alignmentUnit; // 122880
    constexpr int expectedOns = 52;                  // n x 1200 < 61440 ⇒ n = 0..51

    // The precondition that makes this a MULTI-note flush: two consecutive steps'
    // LEN-150 % ends both land at or past the stop, and only two do.
    REQUIRE (51 * stepSamples + tiedGateSamples > music); // 63000 > 61440
    REQUIRE (50 * stepSamples + tiedGateSamples > music); // 61800 > 61440
    REQUIRE (49 * stepSamples + tiedGateSamples < music); // 60600 < 61440 — and only two

    const auto schedule = playThenStop (music);
    const auto sweep = sweepBlockSizes (&configureDiscontinuity, schedule, span);

    // 52 ons + 52 offs + 1 CC123 = 105 — and, since #51, byte-identical in EMISSION
    // order at all ten sizes, which `REQUIRE_SWEEP_CLEAN`'s `streamsMatched` leg is
    // what actually enforces.
    REQUIRE_SWEEP_CLEAN (sweep, 2 * expectedOns + 1, 1);

    // ── THE ORDER ITSELF, AS A LITERAL BYTE SEQUENCE ─────────────────────────
    // Asserted at BOTH ends of the sweep. The cross-size comparison above says the
    // ten agree; this says WHAT they agree on, so a change that reordered every size
    // identically — stable, but no longer registration order — still reddens.
    // Step 50 → pool[2] = 64 = 0x40, registered first; step 51 → pool[3] = 65 = 0x41.
    const std::vector<int> expectedFlushBytes { noteOffCh1,
                                                poolPitches[50 % poolSize],
                                                0x00,
                                                noteOffCh1,
                                                poolPitches[51 % poolSize],
                                                0x00,
                                                ccCh1,
                                                allNotesOffCc,
                                                0x00 };

    for (const int blockSize : { 32, probeBlockSize })
    {
        const auto render = renderAt (&configureDiscontinuity, schedule, span, blockSize);
        INFO ("block size " << blockSize << "\n" << render.describe (8));

        REQUIRE (static_cast<int> (noteOnsOf (render).size ()) == expectedOns);
        REQUIRE (byteSequenceAt (render, music) == expectedFlushBytes);
        REQUIRE (sweepSamplesOf (render) == std::vector<std::int64_t> { music });
    }

    // ── NEGATIVE CONTROL: one lane value ⇒ a DIFFERENT stream ────────────────
    const auto reference = renderAt (&configureDiscontinuity, schedule, span, probeBlockSize);
    const auto perturbed = renderAt (&configureDiscontinuityPerturbed, schedule, span, probeBlockSize);
    REQUIRE (! perturbed.empty ());
    REQUIRE (perturbed.toByteStream () != reference.toByteStream ());
}
