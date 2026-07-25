// ─────────────────────────────────────────────────────────────────────────────
// sequencer_retrigger — the two Phase-5-gate determinism findings that Phase 6
// made reachable: issue #36 (the PRE-GOLDEN BLOCKER) and issue #37's bound.
//
// ── WHY THIS FILE EXISTS BEFORE THE FIRST GOLDEN IS BAKED ───────────────────
// #36: `SequencerProcessor::emitStep` runs BEFORE `SoundingNoteTable::
// emitDueNoteOffs`, so `find()` can return an entry whose note-off was already due
// EARLIER IN THIS BLOCK. Retiring it at `offset - 1` (the correct placement for a
// note that is genuinely still sounding) puts it wherever the re-on happens to
// fall — while a SMALLER buffer, one that separates the off and the re-on into
// different blocks, emits it at its exact due sample. Same musical input, different
// MIDI, decided by the device buffer size: a direct §1.2 violation.
//
// A golden baked while that was live would freeze buffer-size-dependent output as
// the permanent reference, and `.claude/rules/git-conventions.md` makes regenerating
// a golden an explicitly-justified act. So this test is the gate that had to exist
// first, and it is verified FAILS-WITHOUT / PASSES-WITH (see the report accompanying
// the commit that introduced it).
//
// ── THE CONFIGURATION IS CHOSEN SO THE BUG IS REACHABLE, NOT FOR CONVENIENCE ─
// At the 1/16 scaffold grid, 300 BPM / 48 kHz gives a 2400-sample step, so two
// consecutive steps do not fit inside even a 4096-sample block and the retrigger
// path is NEVER ENTERED — a test written there would be green for the wrong reason,
// forever. This file therefore runs the 1/32 grid (in spec: §2.1 "1/32..1/4"):
//
//     300 BPM (Transport::maxBpm), 48 kHz, gridStepPpq 0.125
//       ⇒ step = 1200 samples EXACTLY, LEN 50 % ⇒ gate = 600 samples EXACTLY
//
// and a SINGLE-NOTE POOL, so every gated step retriggers the same pitch and the
// retrigger path is entered at every step boundary that shares a block with the
// previous step's due note-off. At block 4096 the first block alone holds step 0's
// on (0), its due off (600), step 1's on (1200), … — the exact shape #36 describes.
//
// THE ANTI-VACUITY MACHINERY IS THE POINT (Phase 5 lesson: a sweep that only
// compares renders to each other passes trivially on empty renders, and a test that
// never enters the bug path passes trivially too):
//   • absolute positions asserted as LITERALS (1200 / 600), never re-derived from
//     the node — a test that silently ran at the default 120 BPM would pass a
//     formula-vs-formula check;
//   • a REACHABILITY COUNTER that counts, block by block, note-off-then-later-
//     note-on-of-the-same-pitch pairs inside ONE block, REQUIREd > 0;
//   • a NEGATIVE CONTROL — the identical shape with the pitch recurrence pushed
//     out to 8 steps — REQUIREing that same counter reads 0, which is what proves
//     the counter measures the bug path rather than always firing.
//
// #37 is a different animal: the one-sample snap-boundary window. The user chose
// option (b) — document the window rather than close it — so the test here turns the
// header's prose bound into a guard that goes RED if the supported grid coarsens.
// ─────────────────────────────────────────────────────────────────────────────

#include "support/MidiRenderHarness.h"
#include "support/NoteLifecycleCheck.h"
#include "support/SequencerRenderRig.h"

#include "engine/graph/EngineCommand.h"
#include "engine/graph/EngineGraph.h"
#include "engine/graph/Transport.h"
#include "engine/midi/NotePool.h"
#include "engine/sequencer/PatternTypes.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using arpbox::engine::EngineCommandType;
using arpbox::engine::LaneId;
using arpbox::engine::maxSteps;
using arpbox::engine::PoolSnapshot;
using arpbox::engine::Transport;
using arpbox::testing::engineCommand;
using arpbox::testing::MidiRenderConfig;
using arpbox::testing::MidiRenderResult;
using arpbox::testing::NoteLifecycleTracker;
using arpbox::testing::renderSequencer;
using arpbox::testing::ScheduledCommand;
using arpbox::testing::scheduleIsBlockAligned;
using arpbox::testing::SequencerRig;
using arpbox::testing::TimedMidiEvent;

namespace
{
// ── The musical configuration (see the header note on why each value) ────────

constexpr double retrigSampleRate = 48000.0;
constexpr double retrigBpm = 300.0;         ///< Transport::maxBpm — asserted below, not assumed.
constexpr double retrigGridStepPpq = 0.125; ///< 1/32 note (§2.1 allows 1/32..1/4).

/** One step at the configuration above: 0.125 x (60/300) x 48000 = 1200, exactly. */
constexpr std::int64_t retrigStepSamples = 1200;

/** The gate at the default LEN of 50 %: 1200 / 2 = 600, exactly. */
constexpr std::int64_t retrigGateSamples = 600;

// ── THE SECOND BRANCH: LEN > 100 % (issue #65, refs #46) ─────────────────────
// Everything above configures the ALREADY-DUE branch. A 50 % gate ends 600 samples
// into a 1200-sample step, so by the time the pitch returns the outgoing note has
// ALWAYS expired — `cutoffForSamePitch` breaks out immediately on
// `boundary > naturalDueSample` and the STILL-SOUNDING branch is never entered.
// These constants configure the twin case, where the note is still live when its
// own pitch comes round again.

/** LEN as a percentage of the step (§12.1 allows 1..400). 150 % ⇒ tie/legato. */
constexpr int retrigTiedLenPercent = 150;

/** The NATURAL end of a 150 % gate: llround (1.5 x 1200) = 1800, exactly. Beyond the
    1200-sample step, which is what makes the note still sounding at the retrigger. */
constexpr std::int64_t retrigTiedNaturalSamples = 1800;

/** Where `cutoffForSamePitch` actually places that note's off: §5.5's 1-sample gap
    before the next same-pitch onset, on the ABSOLUTE timeline — `1200 - 1`. */
constexpr std::int64_t retrigCutoffOffset = 1199;

/** lcm (32, 64, 96, 128, 256, 480, 512, 1024, 2048, 4096) = 2^12 x 3 x 5. Every
    swept block size divides it, so a command scheduled on a multiple of it lands on
    a block HEAD everywhere and every render covers the identical sample span. */
constexpr std::int64_t blockAlignmentUnit = 61440;

/** Samples the transport plays for. 2 x the alignment unit. */
constexpr std::int64_t playSpanSamples = 122880;

/** Total render, 3 x the alignment unit: the play span plus one more unit AFTER the
    terminating stop, so the flush and the silence behind it are both in the stream. */
constexpr std::int64_t renderSpanSamples = 184320;

/** 4096 is required (the largest realistic device buffer, and the size #36 names);
    96 and 480 are there because real CoreAudio devices hand out non-powers of two. */
constexpr int sweptBlockSizes[] = { 32, 64, 96, 128, 256, 480, 512, 1024, 2048, 4096 };

/** Step boundaries in [0, 122880): n x 1200 < 122880 ⇒ n = 0..102. */
constexpr int expectedNoteOns = 103;

constexpr int retrigChannel = 1; ///< PatternSetState::outputChannel default.
constexpr int retrigPitch = 60;  ///< The single-note pool's only note (middle C).

/** The block size the reachability counter and the negative control run at. It must
    be LARGER than one step (1200) or the bug path cannot be entered at all. */
constexpr int reachabilityBlockSize = 4096;

/** Samples per step from the MUSICAL definition, derived here rather than read off
    the node — so a node that computed it differently would disagree with the
    literals above instead of agreeing with itself. */
double samplesPerStepAt (double stepPpq, double bpm, double sampleRate) noexcept
{
    return stepPpq * (60.0 / bpm) * sampleRate;
}

/** THE POSITIVE CONFIGURATION: 1/32 grid over a ONE-NOTE pool, so the traversal has
    a single degree and every gated step emits the same pitch. That is the simplest
    reliable way to put the same pitch on adjacent steps; a PITCH lane repeating a
    value on neighbouring steps would do the same job with more moving parts. */
void configureSamePitchOnEveryStep (SequencerRig& rig)
{
    rig.patternDocument.setGrid (retrigGridStepPpq);

    PoolSnapshot pool {};
    pool.size = 1;
    pool.sorted[0] = static_cast<std::uint8_t> (retrigPitch);
    pool.asPlayed[0] = static_cast<std::uint8_t> (retrigPitch);
    rig.patternDocument.setPool (pool);
}

/** THE NEGATIVE CONTROL: identical in every respect except the pool, which stays the
    default 8-note stub. `DirectionMode::up` over 8 notes makes a pitch recur every 8
    steps = 9600 samples — far beyond both the 600-sample gate and the 4096-sample
    block — so the retrigger path is unreachable and the counter must read 0. */
void configureRecurrenceBeyondTheGate (SequencerRig& rig)
{
    rig.patternDocument.setGrid (retrigGridStepPpq);
}

/** Fills pattern 0's `lane` with one value across every storage slot, so no step of
    the sweep can be reading a default the case did not choose. */
void fillLane (SequencerRig& rig, LaneId lane, int value)
{
    for (int step = 0; step < maxSteps; ++step)
        rig.patternDocument.setLaneValue (0, lane, step, value);
}

/** THE STILL-SOUNDING POSITIVE (issue #65): the same single-note pool and 1/32 grid
    as `configureSamePitchOnEveryStep`, with LEN raised to 150 %. The natural end at
    1800 samples now falls PAST the next same-pitch onset at 1200, so
    `cutoffForSamePitch` has to shorten the note — the branch #46 lived in. */
void configureStillSoundingSamePitch (SequencerRig& rig)
{
    rig.patternDocument.beginTransaction ();
    configureSamePitchOnEveryStep (rig);
    fillLane (rig, LaneId::len, retrigTiedLenPercent);
    rig.patternDocument.endTransaction ();
}

/** THE NEGATIVE CONTROL FOR THE STILL-SOUNDING BRANCH: LEN 150 % over the DEFAULT
    8-note pool, so a pitch recurs only every 8 steps = 9600 samples — far beyond the
    1800-sample gate. Every note reaches its natural end, no lookahead can fire, and
    the cutoff counter must read 0. This is what proves the counter measures the
    branch rather than firing on any tied render. */
void configureStillSoundingWideRecurrence (SequencerRig& rig)
{
    rig.patternDocument.beginTransaction ();
    rig.patternDocument.setGrid (retrigGridStepPpq);
    fillLane (rig, LaneId::len, retrigTiedLenPercent);
    rig.patternDocument.endTransaction ();
}

/** Tempo + play at sample 0, stop at the end of the play span. Every sample here is
    a multiple of `blockAlignmentUnit`, hence a block head at every swept size.

    THE TEMPO COMMAND IS NOT OPTIONAL: a fresh `Transport` runs at
    `Transport::defaultBpm` (120), which would silently halve the step grid. The
    1200-sample literals are what catches that.

    THE STOP IS NOT DECORATION EITHER: it is the flush point that closes the note
    balance. Without it the render ends mid-gate with one note outstanding, and
    `NoteLifecycleTracker::balanced()` would have nothing to say. */
std::vector<ScheduledCommand> retriggerSchedule ()
{
    return { ScheduledCommand { 0, engineCommand (EngineCommandType::setTempoBpm, retrigBpm) },
             ScheduledCommand { 0, engineCommand (EngineCommandType::transportPlay) },
             ScheduledCommand { playSpanSamples, engineCommand (EngineCommandType::transportStop) } };
}

/** One full render at `blockSize`, fresh rig, collecting ABSOLUTE sample positions. */
MidiRenderResult renderRetrigger (int blockSize, void (*configure) (SequencerRig&))
{
    SequencerRig rig { retrigSampleRate, blockSize };
    configure (rig);

    auto config = MidiRenderConfig::samples (renderSpanSamples, retrigSampleRate, blockSize);
    config.numChannels = 1;      // MIDI-only node; the scratch buffer only carries a length
    config.eventReserve = 16384; // > any render here, so the loop never reallocates

    return renderSequencer (rig, config, retriggerSchedule ());
}

std::vector<TimedMidiEvent> noteOnsOf (const MidiRenderResult& render)
{
    return render.select ([] (const TimedMidiEvent& event) { return event.message.isNoteOn (); });
}

std::vector<TimedMidiEvent> noteOffsOf (const MidiRenderResult& render)
{
    return render.select ([] (const TimedMidiEvent& event) { return event.message.isNoteOff (); });
}

// ── THE BRANCH CLASSIFIER (issue #65) ────────────────────────────────────────

/** Which same-pitch retrigger branch produced each note-off, read off the emitted
    stream rather than out of the engine — so it stays an independent observation and
    not a second copy of the fix.

    THE TWO BRANCHES HAVE DISJOINT ABSOLUTE-SAMPLE SIGNATURES, because
    `cutoffForSamePitch` places a still-sounding cutoff at `next same-pitch on - 1`
    while a note that simply expired sits at its natural gate end:

        LEN  50 %   natural end = on +  600   ⇒  sample % 1200 ==  600
        LEN 150 %   natural end = on + 1800   ⇒  sample % 1200 ==  600   (1800 - 1200)
        either      still-sounding cutoff     ⇒  sample % 1200 == 1199

    A note-off at residue 1199 is therefore producible by NOTHING ELSE in these
    configurations, which is what makes `cutoff` an honest reachability counter for
    the still-sounding branch — and what lets the ALREADY-DUE case assert it reads
    ZERO, stating that case's scope limit executably instead of in prose.

    The stop flush lands on neither residue (122880 % 1200 == 480), so it is counted
    on its own and can never inflate either arm. Anything else is `other`, which must
    stay 0 or the classification itself has stopped describing the stream. */
struct RetriggerBranchTally
{
    int cutoff = 0;    ///< Offs only `cutoffForSamePitch`'s still-sounding branch emits.
    int natural = 0;   ///< Offs at the note's own natural gate end.
    int stopFlush = 0; ///< Offs from the terminating stop's flush.
    int other = 0;     ///< Anything unaccounted for — must be 0.

    [[nodiscard]] std::string describe () const
    {
        return "cutoff-shaped " + std::to_string (cutoff) + ", natural-shaped " + std::to_string (natural) +
               ", stop-flush " + std::to_string (stopFlush) + ", unclassified " + std::to_string (other);
    }
};

RetriggerBranchTally classifyNoteOffs (const MidiRenderResult& render)
{
    RetriggerBranchTally tally;

    // No Catch2 macros in here (house rule): count, return, assert at the call site.
    for (const auto& off : noteOffsOf (render))
    {
        const std::int64_t residue = off.absoluteSample % retrigStepSamples;

        if (off.absoluteSample == playSpanSamples)
            ++tally.stopFlush;
        else if (residue == retrigCutoffOffset)
            ++tally.cutoff;
        else if (residue == retrigGateSamples)
            ++tally.natural;
        else
            ++tally.other;
    }

    return tally;
}

// ── THE REACHABILITY COUNTER ─────────────────────────────────────────────────

/** What one block-by-block pass observed. `blocksWithRetrigger` is the anti-vacuity
    number: how many single blocks contained a note-off for a pitch followed, LATER
    IN THE SAME BLOCK, by a note-on for that same pitch. That interleaving is
    observable ONLY when `emitStep` found an already-due entry — i.e. it is exactly
    the #36 code path, and it is present whether or not the fix is in (the fix moves
    WHERE the off lands, not whether the pair exists), which is what makes it an
    honest reachability probe rather than a second copy of the fix assertion. */
struct RetriggerReachability
{
    int blocksWithRetrigger = 0; ///< Blocks containing at least one such pair.
    int retriggerPairs = 0;      ///< Total pairs across the render.
    std::int64_t noteOns = 0;    ///< Note-ons seen (non-vacuity for the counter itself).
    int blocksRendered = 0;      ///< Blocks actually pushed through the node.
};

/** Renders block by block (the harness's absolute-position stream cannot see a block
    boundary, and the boundary is the whole question here) and counts the pairs. */
RetriggerReachability measureReachability (int blockSize, void (*configure) (SequencerRig&))
{
    SequencerRig rig { retrigSampleRate, blockSize };
    configure (rig);

    juce::AudioBuffer<float> audio (1, blockSize);
    juce::MidiBuffer midi;
    midi.ensureSize (8192);

    const auto schedule = retriggerSchedule ();
    const int blocks = static_cast<int> (renderSpanSamples / blockSize);

    RetriggerReachability result;

    // No Catch2 macros inside this loop (house rule): aggregate, then assert.
    for (int block = 0; block < blocks; ++block)
    {
        const std::int64_t base = static_cast<std::int64_t> (block) * blockSize;

        // Command drain, THEN beginBlock (which `renderBlock` does) — the graph head
        // node's order, and the reason a stop lands in the block it was scheduled for.
        for (const auto& entry : schedule)
            if (entry.atSample >= base && entry.atSample < base + blockSize)
                rig.transport.applyCommand (entry.command);

        midi.clear ();
        rig.renderBlock (audio, midi);
        ++result.blocksRendered;

        // Earliest note-off offset seen so far in THIS block, per pitch; -1 = none.
        std::array<int, 128> firstOffOffset {};
        firstOffOffset.fill (-1);

        bool flagged = false;

        // juce::MidiBuffer iterates in non-decreasing sample order, so "later in the
        // block" is simply "seen after".
        for (const auto meta : midi)
        {
            const auto message = meta.getMessage ();
            const int note = message.getNoteNumber ();

            if (note < 0 || note > 127)
                continue;

            const auto slot = static_cast<std::size_t> (note);

            // isNoteOff() FIRST — a velocity-0 note-on is a release (see
            // NoteLifecycleCheck.h for the same ordering and the same reason).
            if (message.isNoteOff ())
            {
                if (firstOffOffset[slot] < 0)
                    firstOffOffset[slot] = meta.samplePosition;
            }
            else if (message.isNoteOn ())
            {
                ++result.noteOns;

                if (firstOffOffset[slot] >= 0 && meta.samplePosition > firstOffOffset[slot])
                {
                    ++result.retriggerPairs;
                    flagged = true;
                }
            }
        }

        if (flagged)
            ++result.blocksWithRetrigger;
    }

    return result;
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Issue #36 — THE PRE-GOLDEN BLOCKER
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/retrigger: an already-due note-off keeps its true sample position at every block size",
           "[midi-conformance][determinism]")
{
    // ── 1. THE CONFIGURATION, AS LITERALS ────────────────────────────────────
    // Phase 5 lesson: a test silently ran at the default 120 BPM and was caught only
    // because absolute positions were asserted as literals. So the grid arithmetic is
    // pinned to numbers, and the tempo is pinned to the transport's own maximum.
    REQUIRE (retrigBpm == Transport::maxBpm);
    // Approx, not `==`: 60/300 is inexact in binary, so exact equality here would be
    // asserting a rounding accident. The margin is far below one sample, so a grid or
    // tempo that actually differed could not slip through.
    REQUIRE (samplesPerStepAt (retrigGridStepPpq, retrigBpm, retrigSampleRate) ==
             Catch::Approx (1200.0).margin (1.0e-9));
    REQUIRE (retrigStepSamples == 1200);
    REQUIRE (retrigGateSamples == 600);

    // The gate really is the node's own LEN resolution: 50 % of the step, rounded,
    // minimum one sample (SequencerProcessor::emitStep).
    REQUIRE (static_cast<std::int64_t> (std::llround (0.5 * static_cast<double> (retrigStepSamples))) ==
             retrigGateSamples);

    // THE REACHABILITY PRECONDITION, stated out loud: two consecutive steps must fit
    // inside the largest swept block, or the #36 path is never entered. At the 1/16
    // scaffold grid the step would be 2400 samples and this would be false.
    REQUIRE (2 * retrigStepSamples < reachabilityBlockSize);
    REQUIRE (retrigGateSamples < retrigStepSamples); // gate ends before the next step

    // ── 2. NOTE-ON COUNT FROM FIRST PRINCIPLES ───────────────────────────────
    // Boundaries sit at n x 1200 and the transport plays [0, 122880): n = 0..102.
    REQUIRE (playSpanSamples / retrigStepSamples == 102);
    REQUIRE (playSpanSamples % retrigStepSamples == 480); // the span does NOT end on a boundary
    REQUIRE (expectedNoteOns == 103);

    // Every note-on is released: 102 gates expire inside the span, and the 103rd note
    // (on at 122400, off due at 123000) is still sounding when the stop flushes it.
    constexpr int expectedNoteOffs = expectedNoteOns;
    constexpr std::int64_t lastOnSample = 102 * retrigStepSamples; // 122400
    REQUIRE (lastOnSample + retrigGateSamples > playSpanSamples);  // 123000 > 122880

    MidiRenderResult reference;
    std::vector<std::uint8_t> referenceBytes;

    for (const int blockSize : sweptBlockSizes)
    {
        INFO ("block size " << blockSize);

        // Preconditions, asserted rather than assumed (house rule): the span is
        // covered by whole blocks, and every command lands on a block head.
        REQUIRE (renderSpanSamples % blockSize == 0);
        REQUIRE (blockAlignmentUnit % blockSize == 0);
        REQUIRE (scheduleIsBlockAligned (retriggerSchedule (), blockSize));

        const auto render = renderRetrigger (blockSize, configureSamePitchOnEveryStep);
        REQUIRE (render.numSamples == renderSpanSamples);
        REQUIRE (render.isSampleSorted ());

        const auto ons = noteOnsOf (render);
        const auto offs = noteOffsOf (render);
        INFO (render.describe (10));

        // NON-VACUITY: the render really played, and it played the shape asserted
        // below. A silent render cannot reach the comparison at the bottom.
        REQUIRE (static_cast<int> (ons.size ()) == expectedNoteOns);
        REQUIRE (static_cast<int> (offs.size ()) == expectedNoteOffs);

        // Exactly one CC123, from the single stop flush; nothing else is emitted.
        const auto sweeps =
            render.select ([] (const TimedMidiEvent& event) { return event.message.isControllerOfType (123); });
        REQUIRE (sweeps.size () == 1u);
        REQUIRE (render.size () == ons.size () + offs.size () + sweeps.size ());

        // ── 4. THE FIX, STATED DIRECTLY ──────────────────────────────────────
        // Step 0's note-off belongs at 600 — its TRUE due sample — at EVERY block
        // size. Without the fix it lands at 1199 (one before step 1's re-on) whenever
        // the block is large enough to hold both, i.e. at 2048 and 4096.
        REQUIRE (offs.front ().absoluteSample == retrigGateSamples);
        REQUIRE (offs.front ().absoluteSample == 600);

        // ...and the same statement for every step, plus the on grid it hangs off.
        for (std::size_t i = 0; i < ons.size (); ++i)
        {
            const auto expectedOn = static_cast<std::int64_t> (i) * retrigStepSamples;
            INFO ("step " << i << ": " << ons[i].describe ());
            REQUIRE (ons[i].absoluteSample == expectedOn);
            REQUIRE (ons[i].message.getNoteNumber () == retrigPitch);
            REQUIRE (ons[i].message.getChannel () == retrigChannel);

            // The last off is the stop flush's, at the stop sample, not at a gate end.
            const auto expectedOff =
                (i + 1 < ons.size ()) ? expectedOn + retrigGateSamples : static_cast<std::int64_t> (playSpanSamples);
            INFO ("off " << i << ": " << offs[i].describe ());
            REQUIRE (offs[i].absoluteSample == expectedOff);
        }

        // ── 6. NOTE LIFECYCLE ────────────────────────────────────────────────
        NoteLifecycleTracker tracker;
        tracker.observeAll (render);
        INFO (tracker.describe ());
        REQUIRE (tracker.noteOnsSeen () == expectedNoteOns); // gate before trusting balanced()
        REQUIRE (tracker.orphanNoteOffs () == 0);
        REQUIRE (tracker.balanced ()); // the terminating stop closed every note

        // ── 5. THE DETERMINISM COMPARISON (§1.2) ─────────────────────────────
        // Byte-for-byte against the 32-sample render, and against its canonical
        // serialization — the Phase-6 golden-file comparison target.
        if (reference.empty ())
        {
            reference = render;
            referenceBytes = render.toByteStream ();
            REQUIRE (! referenceBytes.empty ());
        }
        else
        {
            INFO (reference.describeDifference (render));
            REQUIRE (render == reference);
            REQUIRE (render.toByteStream () == referenceBytes);
        }
    }

    // ── 3. THE ANTI-VACUITY ASSERTION ────────────────────────────────────────
    // Everything above could be green because the #36 path was never entered. This
    // says it WAS: at 4096 there are blocks holding a note-off and a LATER note-on of
    // the same pitch, which is only producible by `emitStep` finding an already-due
    // entry.
    const auto positive = measureReachability (reachabilityBlockSize, configureSamePitchOnEveryStep);
    INFO ("positive: " << positive.blocksWithRetrigger << " blocks with a retrigger pair, " << positive.retriggerPairs
                       << " pairs, " << positive.noteOns << " note-ons over " << positive.blocksRendered << " blocks");
    REQUIRE (positive.blocksRendered == renderSpanSamples / reachabilityBlockSize);
    REQUIRE (positive.noteOns == expectedNoteOns);
    REQUIRE (positive.blocksWithRetrigger > 0);
    REQUIRE (positive.retriggerPairs > 0);

    // ── 7. THE NEGATIVE CONTROL ──────────────────────────────────────────────
    // The identical shape with the pitch recurrence pushed out to 8 steps (9600
    // samples) — beyond the gate AND beyond the block. The counter must read 0 here,
    // which is what proves it measures the bug path rather than firing on any render
    // that contains note-offs and note-ons.
    const auto control = measureReachability (reachabilityBlockSize, configureRecurrenceBeyondTheGate);
    INFO ("control: " << control.blocksWithRetrigger << " blocks with a retrigger pair, " << control.retriggerPairs
                      << " pairs, " << control.noteOns << " note-ons over " << control.blocksRendered << " blocks");
    REQUIRE (control.blocksRendered == positive.blocksRendered);
    REQUIRE (control.noteOns == expectedNoteOns); // the control really played, too
    REQUIRE (control.blocksWithRetrigger == 0);
    REQUIRE (control.retriggerPairs == 0);

    // ── 8. THE SCOPE LIMIT, STATED EXECUTABLY (issue #65) ────────────────────
    // THIS CASE COVERS ONE OF THE TWO SAME-PITCH RETRIGGER BRANCHES. A 50 % gate ends
    // 600 samples into a 1200-sample step, so the outgoing note has ALWAYS expired by
    // the time its pitch returns and `cutoffForSamePitch` breaks out immediately on
    // `boundary > naturalDueSample`. The STILL-SOUNDING branch is unreachable from
    // here — structurally, not by accident.
    //
    // THAT IS WHY #46 SURVIVED #36's FIX: #46 is this defect's twin on the branch
    // this configuration cannot enter, and the guard written for #36 was incapable of
    // catching it. A reader who took "sequencer_retrigger.cpp is green" to mean the
    // retrigger policy was guarded would have been wrong in exactly that way.
    //
    // So the limit is asserted rather than described: every note-off here is a
    // natural gate end or the stop flush, and NONE carries the cutoff signature. The
    // case below covers the other branch.
    const auto tally = classifyNoteOffs (renderRetrigger (reachabilityBlockSize, configureSamePitchOnEveryStep));
    INFO ("already-due branch: " << tally.describe ());
    REQUIRE (tally.natural == expectedNoteOns - 1); // 102 gate ends...
    REQUIRE (tally.stopFlush == 1);                 // ...plus the terminating flush
    REQUIRE (tally.other == 0);                     // the classification really describes the stream
    REQUIRE (tally.cutoff == 0);                    // THE SCOPE LIMIT
}

// ─────────────────────────────────────────────────────────────────────────────
// Issue #65 / #46 — THE OTHER BRANCH: the note is STILL SOUNDING
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/retrigger: a still-sounding same-pitch note is cut one sample before the retrigger at every "
           "block size",
           "[midi-conformance][determinism]")
{
    // ── WHY THIS CASE EXISTS ALONGSIDE ITS INTEGRATION TWIN ──────────────────
    // `sequencer_offdeterminism.cpp` case B already covers this branch, and covers it
    // well — block-head asymmetry, byte-identical sweep, the 1199 literal. This case
    // is not that check repeated. It is the branch-REACHABILITY half, living in the
    // file that owns the retrigger policy, so the two same-pitch branches are
    // guarded side by side under one classifier and neither can be mistaken for the
    // other's coverage. The case above asserts that classifier reads 0; this one
    // asserts it fires.
    //
    // ── THE CONFIGURATION, AND WHY IT REACHES THE BRANCH ─────────────────────
    // Identical to the case above except LEN, which goes from 50 % to 150 %:
    //
    //     step          1200 samples   (1/32 @ 300 BPM / 48 kHz)
    //     natural end   1800 samples   (llround (1.5 x 1200)) — PAST the next step
    //     cutoff        1199 samples   (next same-pitch on - 1, §5.5's 1-sample gap)
    //
    // 1800 > 1200 is the whole point: the note is still live when its own pitch comes
    // round, so `cutoffForSamePitch`'s lookahead must shorten it. At 50 % it could not.
    REQUIRE (retrigTiedNaturalSamples == static_cast<std::int64_t> (std::llround (
                                             0.01 * retrigTiedLenPercent * static_cast<double> (retrigStepSamples))));
    REQUIRE (retrigTiedNaturalSamples > retrigStepSamples); // STILL SOUNDING at the retrigger
    REQUIRE (retrigCutoffOffset == retrigStepSamples - 1);  // §5.5's 1-sample gap
    REQUIRE (retrigCutoffOffset != retrigTiedNaturalSamples);
    REQUIRE (retrigCutoffOffset != retrigGateSamples); // and distinct from the 50 % end

    // Same grid, same span, so the note-on count is the one already derived above.
    REQUIRE (playSpanSamples / retrigStepSamples == 102);
    REQUIRE (expectedNoteOns == 103);

    // ── THE SWEEP ────────────────────────────────────────────────────────────
    // A single block size cannot verify this. At 4096 no step boundary is ever a
    // block head, so `on - 1` always sits in the same block as the retrigger and the
    // pre-#46 placement would look correct. At 32 every EVEN boundary is a head, and
    // the cutoff sample belongs to the PREVIOUS block — the placement `jmax (0,
    // offset - 1)` structurally could not express. The disagreement between those two
    // ends IS the defect, so both must be rendered and compared.
    //
    // No Catch2 macros inside the loop (house rule): aggregate, then assert.
    MidiRenderResult reference;
    std::vector<std::uint8_t> referenceBytes;

    int sizesSwept = 0;
    int sizesMatchingReference = 0;
    int sizesWithFullCutoffTally = 0;
    int sizesWithAnyNaturalOff = 0;
    int sizesWithCorrectOnGrid = 0;
    int sizesWithCorrectOffGrid = 0;
    int sizesBalanced = 0;
    int boundariesThatAreBlockHeads32 = 0;
    int boundariesThatAreBlockHeads4096 = 0;
    std::string firstOffender;

    for (const int blockSize : sweptBlockSizes)
    {
        ++sizesSwept;

        const auto render = renderRetrigger (blockSize, configureStillSoundingSamePitch);
        const auto ons = noteOnsOf (render);
        const auto offs = noteOffsOf (render);
        const auto tally = classifyNoteOffs (render);

        // 102 cutoffs (steps 0..101) + 1 stop-flush off for the last note.
        if (tally.cutoff == expectedNoteOns - 1 && tally.stopFlush == 1 && tally.other == 0)
            ++sizesWithFullCutoffTally;
        else if (firstOffender.empty ())
            firstOffender = "block " + std::to_string (blockSize) + ": " + tally.describe ();

        // The natural 1800-sample end must NEVER be reached: every note but the last
        // is cut short. A non-zero count here means the lookahead did not fire.
        if (tally.natural > 0)
            ++sizesWithAnyNaturalOff;

        // ABSOLUTE LITERALS, not a formula re-derived from the node.
        int onsAtGrid = 0;
        int offsAtCutoff = 0;

        for (std::size_t i = 0; i < ons.size (); ++i)
        {
            const auto expectedOn = static_cast<std::int64_t> (i) * retrigStepSamples;
            onsAtGrid += (ons[i].absoluteSample == expectedOn && ons[i].message.getNoteNumber () == retrigPitch &&
                          ons[i].message.getChannel () == retrigChannel)
                             ? 1
                             : 0;

            const bool isLast = (i + 1 == ons.size ());
            const auto expectedOff = isLast ? playSpanSamples : expectedOn + retrigCutoffOffset;

            if (i < offs.size ())
                offsAtCutoff += (offs[i].absoluteSample == expectedOff) ? 1 : 0;
        }

        if (static_cast<int> (ons.size ()) == expectedNoteOns && onsAtGrid == expectedNoteOns)
            ++sizesWithCorrectOnGrid;

        if (static_cast<int> (offs.size ()) == expectedNoteOns && offsAtCutoff == expectedNoteOns)
            ++sizesWithCorrectOffGrid;

        NoteLifecycleTracker tracker;
        tracker.observeAll (render);

        if (tracker.noteOnsSeen () == expectedNoteOns && tracker.orphanNoteOffs () == 0 && tracker.balanced () &&
            render.isSampleSorted () && render.numSamples == renderSpanSamples)
            ++sizesBalanced;

        if (reference.empty ())
        {
            reference = render;
            referenceBytes = render.toByteStream ();
        }
        else if (render == reference && render.toByteStream () == referenceBytes)
        {
            ++sizesMatchingReference;
        }
        else if (firstOffender.empty ())
        {
            firstOffender = "block " + std::to_string (blockSize) + " differs from the 32-sample reference";
        }
    }

    // How asymmetric the sweep really is — the mechanism, made visible. Counted from
    // arithmetic alone, so it holds whatever the engine did.
    for (int step = 0; step + 1 < expectedNoteOns; ++step)
    {
        const std::int64_t nextOn = static_cast<std::int64_t> (step + 1) * retrigStepSamples;
        boundariesThatAreBlockHeads32 += (nextOn % 32 == 0) ? 1 : 0;
        boundariesThatAreBlockHeads4096 += (nextOn % 4096 == 0) ? 1 : 0;
    }

    INFO ("first offender: " << (firstOffender.empty () ? "none" : firstOffender));
    INFO ("swept " << sizesSwept << " sizes; retrigger boundaries that are block heads — at 32: "
                   << boundariesThatAreBlockHeads32 << ", at 4096: " << boundariesThatAreBlockHeads4096);

    REQUIRE (sizesSwept == 10);
    REQUIRE (sizesWithCorrectOnGrid == 10);
    REQUIRE (sizesWithCorrectOffGrid == 10);
    REQUIRE (sizesBalanced == 10);

    // ── THE REACHABILITY ASSERTION (issue #65) ───────────────────────────────
    // The counter the case above requires to read 0 must read 102 here, at EVERY
    // block size. This is the still-sounding branch, entered.
    REQUIRE (sizesWithFullCutoffTally == 10);
    REQUIRE (sizesWithAnyNaturalOff == 0);

    // ── THE DETERMINISM COMPARISON (§1.2) ────────────────────────────────────
    REQUIRE (! referenceBytes.empty ());
    REQUIRE (sizesMatchingReference == 9); // all nine against the 32-sample reference

    // Non-vacuity for the sweep's asymmetry: the two ends genuinely disagree about
    // which retrigger boundaries are block heads, so the equality above is a claim.
    REQUIRE (boundariesThatAreBlockHeads32 > 0);
    REQUIRE (boundariesThatAreBlockHeads4096 == 0);

    // ── THE NEGATIVE CONTROL ─────────────────────────────────────────────────
    // LEN 150 % over the default 8-note pool: a pitch recurs every 8 steps (9600
    // samples), far beyond the 1800-sample gate, so no lookahead can fire. The cutoff
    // counter must collapse to 0 and the natural end must be what is heard — which is
    // what proves the counter measures the branch and not merely "a tied render".
    const auto control =
        classifyNoteOffs (renderRetrigger (reachabilityBlockSize, configureStillSoundingWideRecurrence));
    INFO ("wide-recurrence control: " << control.describe ());
    REQUIRE (control.natural > 0); // the control really played, and played to its natural end
    REQUIRE (control.other == 0);
    REQUIRE (control.cutoff == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Issue #37 — the snap-boundary window, bounded
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/snap-window: the step-index snap stays under one sample at every supported grid",
           "[unit][midi-conformance]")
{
    // ── WHAT THIS GUARDS AND WHY A COMMENT COULD NOT ─────────────────────────
    // #37's resolution was option (b): DOCUMENT the one-sample window rather than
    // close it (see "THE SNAP-BOUNDARY WINDOW" in SequencerProcessor.h). Documentation
    // decays silently. This case turns the header's table into an executable bound, so
    // it goes RED — rather than merely becoming wrong — if a future phase:
    //
    //   • adds a grid COARSER than a dotted quarter (the window scales with step
    //     length, so a whole-bar grid would blow straight through one sample), or
    //   • applies a lane clock DIVISION to the walk itself rather than to lane
    //     indexing (which would multiply the effective step length the snap sees), or
    //   • raises `engine::kMaxSupportedSampleRate`, the enforced ceiling this case
    //     reads directly (issue #56 — see the note on that below).
    //
    // The window is NOT a fixed number of samples. It is `stepIndexSnapSteps` STEPS,
    // so its width in samples is:
    //
    //     window = stepIndexSnapSteps x stepPpq x (60 / bpm) x sampleRate
    //
    // and it is widest at the COARSEST grid, the SLOWEST tempo and the HIGHEST sample
    // rate. Below one sample, the effect is a note landing at most one sample late.
    // Above one sample it stops being a blemish and starts moving events arbitrarily.

    // Mirrors the anonymous-namespace constant in SequencerProcessor.cpp. It is
    // file-local there (correctly — it is an implementation detail of the walk), so
    // this is a deliberate copy. A change to THAT value is not caught here; the
    // re-derivation obligation for it is stated in the header's window note.
    constexpr double stepIndexSnapSteps = 1.0e-6;

    // THE SUPPORTED GRID SET (§2.1 "1/32..1/4, triplet/dotted"), in quarter notes per
    // step. Straight, triplet (x2/3) and dotted (x3/2) of 1/32, 1/16, 1/8, 1/4.
    // Coarsest = dotted quarter = 1.5. ADDING A COARSER GRID MEANS EDITING THIS LIST,
    // which is precisely the edit that must re-derive the bound.
    constexpr double supportedGridStepPpq[] = {
        0.125, 0.125 * 2.0 / 3.0, 0.125 * 1.5, // 1/32 straight / triplet / dotted
        0.25,  0.25 * 2.0 / 3.0,  0.25 * 1.5,  // 1/16
        0.5,   0.5 * 2.0 / 3.0,   0.5 * 1.5,   // 1/8
        1.0,   1.0 * 2.0 / 3.0,   1.0 * 1.5    // 1/4  (dotted quarter = 1.5, the coarsest)
    };

    // ── THE SAMPLE-RATE CEILING IS NOW A CODE FACT (issue #56) ───────────────
    // This used to read "an assumption, not a code fact": nothing capped the rate, so
    // the bound below rested on 192 kHz being the highest rate mainstream interfaces
    // offer, and at 384 kHz — not hypothetical, DXD interfaces exist and CoreAudio
    // will negotiate them — the worst case would have been 1.728 samples and this
    // bound would have BROKEN. That exposure is what #56 was filed for, and #56
    // closed it.
    //
    // The ceiling is now `engine::kMaxSupportedSampleRate`, enforced by
    // `app::AudioEngine::enforceSampleRateCeiling()` on all three device-open paths:
    // a device negotiating above it is down-negotiated to the highest available rate
    // at or below it, and closed outright if it offers none. So the 0.864-sample
    // worst case asserted below is a REAL BOUND on what the engine can be handed,
    // not a hope about what hardware people own.
    //
    // KEYED OFF THE CONSTANT, NOT A COPY OF IT. The constant lives in
    // engine/graph/EngineGraph.h — deliberately engine-side rather than in app/, so
    // tests can reach it without depending on the app target. Raising it therefore
    // recomputes `worstWindow` here and REDDENS this case, instead of leaving the
    // test quietly checking a number the product no longer honours. That is the whole
    // point of #56: the bound was only ever as good as an assumption nobody enforced,
    // and now the test and the enforcement point at the same symbol.
    constexpr double assumedMaxSampleRate = arpbox::engine::kMaxSupportedSampleRate;

    // The literal the rows below are quoted at, asserted against the constant rather
    // than assumed equal to it — this is what turns a raised ceiling into a red test
    // here rather than into silently stale reference rows.
    REQUIRE (assumedMaxSampleRate == 192000.0);

    // The window formula, once. Comparisons use `Catch::Approx` with an absolute
    // margin rather than `==`: these are derived doubles (1e-6 and 60/20 are both
    // inexact in binary), so exact equality would be asserting a rounding accident,
    // not the bound. The margin is 12 orders of magnitude below the quantity of
    // interest, so it cannot mask a real change to any of the inputs.
    const auto windowAt = [stepIndexSnapSteps] (double stepPpq, double bpm, double sampleRate)
    { return stepIndexSnapSteps * stepPpq * (60.0 / bpm) * sampleRate; };

    const auto sample = [] (double expected) { return Catch::Approx (expected).margin (1.0e-12); };

    // ── The three reference rows from the header's table, pinned ─────────────
    REQUIRE (windowAt (0.25, 120.0, 48000.0) == sample (0.006));             // scaffold: 1/16 @ 120 / 48 kHz
    REQUIRE (windowAt (0.25, 20.0, 192000.0) == sample (0.144));             // 1/16 @ minBpm / 192 kHz
    REQUIRE (windowAt (1.5, 20.0, 192000.0) == sample (0.864));              // 1/4 dotted — the worst supported case
    REQUIRE (windowAt (1.5, Transport::minBpm, 192000.0) == sample (0.864)); // Transport::minBpm really is 20

    // ── The sweep: every supported grid at the worst tempo and rate ──────────
    double worstWindow = 0.0;
    double worstGrid = 0.0;
    int gridsSwept = 0;

    for (const double stepPpq : supportedGridStepPpq)
    {
        ++gridsSwept;
        const double w = windowAt (stepPpq, Transport::minBpm, assumedMaxSampleRate);

        if (w > worstWindow)
        {
            worstWindow = w;
            worstGrid = stepPpq;
        }
    }

    REQUIRE (gridsSwept == 12);
    REQUIRE (worstGrid == sample (1.5)); // the coarsest supported grid is a dotted quarter

    INFO ("worst supported window: " << worstWindow << " samples at stepPpq " << worstGrid << ", " << Transport::minBpm
                                     << " BPM, " << assumedMaxSampleRate << " Hz — headroom " << (1.0 - worstWindow)
                                     << " samples (" << (100.0 * (1.0 - worstWindow)) << " % below one sample)");

    // ── THE BOUND ────────────────────────────────────────────────────────────
    // Under one sample ⇒ the #37 effect is capped at a single sample of lateness and
    // cannot compound. Note how thin this is: ~13.6 % of a sample, against the
    // scaffold row's ~167x margin.
    REQUIRE (worstWindow < 1.0);
    REQUIRE (worstWindow > 0.86); // non-vacuous: the sweep really evaluated the coarse end
    REQUIRE (1.0 - worstWindow > 0.13);

    // And the window is genuinely grid-scaled, not a constant someone could "simplify"
    // into a fixed number of samples: the coarsest grid is 12x the finest.
    REQUIRE (windowAt (1.5, Transport::minBpm, assumedMaxSampleRate) ==
             sample (12.0 * windowAt (0.125, Transport::minBpm, assumedMaxSampleRate)));
}
