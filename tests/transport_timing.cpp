// ─────────────────────────────────────────────────────────────────────────────
// transport_timing — Phase 5.3c: THE buffer-size-independence sweep.
//
// This file carries the Phase 5 headline success criterion — "test pattern plays in
// perfect time at any buffer size" — and is the regression guard for the two
// determinism snaps inside `SequencerProcessor` (see the extended note in
// SequencerProcessor.h):
//
//   1. `stepIndexSnapSteps` — the SAME snapped ceiling applied to both ends of the
//      half-open block interval, so block k's exclusive end index is bitwise equal to
//      block k+1's first index. Without it the two expressions disagree by an ulp at
//      ~24% of block edges, and a step boundary landing inside that disagreement is
//      emitted twice or not at all.
//   2. `sampleOffsetSnapSamples` — snapping up before flooring the PPQ→offset
//      conversion, so a boundary whose true offset is an exact integer does not land
//      one sample early. Which steps move depends on the buffer size, which is why
//      only a cross-block-size comparison catches it.
//
// FOR THE SWEEP TO BE A GUARD IT MUST DISCRIMINATE, so the configurations are chosen,
// not convenient:
//   • Config A — 60 BPM @ 48 kHz. A 16th note is EXACTLY 12000 samples, so every step
//     position is an exact integer and can be asserted literally. This is the family
//     the offset snap exists for: `floor(12000 - 1e-9)` is 11999.
//   • Config B — 137 BPM @ 44.1 kHz. A 16th note is 4828.467… samples: no step lands
//     on an integer, no block edge coincides with a step, and the PPQ arithmetic never
//     hits a clean value. The complementary family.
//   • Block sizes 32…2048 INCLUDING 96 and 480, because real devices run non-power-of-
//     two buffers and a bug that only survives powers of two would ship.
//   • Every span is a multiple of 30720 = lcm(2048, 96, 480), so every block size
//     divides it exactly. Renders therefore cover the IDENTICAL sample span rather
//     than rounding up to different lengths — otherwise a length difference at the
//     tail would masquerade as a timing bug (or, worse, mask one).
//
// AND IT MUST NOT BE VACUOUS: a sweep that only compares renders to each other passes
// trivially when every render is empty. So each render's step COUNT is asserted from
// first principles (bars x steps-per-bar for A, the half-open boundary count for B),
// and config A's positions are asserted as exact absolute samples.
//
// THE PREMISE HAS ONE DOCUMENTED EXCEPTION, and a reader arriving from the test side
// should know it before concluding the sweep proves an absolute guarantee. A step
// boundary whose true position lies within `stepIndexSnapSteps` BELOW a block edge is
// claimed by the later block, and the clamp in `SequencerProcessor::processBlock`
// emits it at offset 0 — up to ONE SAMPLE later than a carving that puts the same
// boundary mid-block. The step INDEX SET is unaffected (never duplicated, never
// skipped); only placement moves, and only by a sample. The window is
// `1e-6 x stepPpq x 60 x sampleRate / bpm` samples, so it widens with the grid — see
// "THE SNAP-BOUNDARY WINDOW" in SequencerProcessor.h for the bound and issue #37 for
// the decision to document rather than close it. No configuration in this file lands
// inside the window, and none realistically can; a failure here means a real bug, not
// that exception.
//
// Existing coverage NOT duplicated here: transport_clock.cpp already pins PPQ against
// its closed form, transport-level block-size equivalence, the tempo clamp, locate,
// the stop edges and head-node render ordering. This file is about what the SEQUENCER
// emits.
// ─────────────────────────────────────────────────────────────────────────────

#include "support/MidiRenderHarness.h"
#include "support/NoteLifecycleCheck.h"
#include "support/SequencerRenderRig.h"

#include "engine/graph/EngineCommand.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using arpbox::engine::EngineCommandType;
using arpbox::testing::engineCommand;
using arpbox::testing::expectedScaffoldSteps;
using arpbox::testing::MidiRenderConfig;
using arpbox::testing::MidiRenderResult;
using arpbox::testing::NoteLifecycleTracker;
using arpbox::testing::renderSequencer;
using arpbox::testing::samplesPerScaffoldStep;
using arpbox::testing::scaffoldGateSamples;
using arpbox::testing::ScheduledCommand;
using arpbox::testing::scheduleIsBlockAligned;
using arpbox::testing::SequencerRig;
using arpbox::testing::TimedMidiEvent;

namespace
{
/** The swept block sizes: the seven required powers of two plus 96 and 480, the two
    non-power-of-two sizes real CoreAudio devices actually hand us. All nine divide
    30720 exactly. */
constexpr int sweptBlockSizes[] = { 32, 64, 96, 128, 256, 480, 512, 1024, 2048 };

/** lcm(2048, 96, 480) — every swept block size divides it, so any render span that is
    a multiple of it is covered by a WHOLE number of blocks at every block size, and a
    command scheduled at a multiple of it lands on a block head everywhere. */
constexpr std::int64_t blockAlignmentUnit = 30720;

/** One musical configuration to sweep. */
struct SweepConfig
{
    double sampleRate = 48000.0;
    double bpm = 120.0;
    std::int64_t spanSamples = 0; ///< Must be a multiple of `blockAlignmentUnit`.
};

// Config A: an EXACT-INTEGER step grid. 60 BPM @ 48 kHz ⇒ quarter = 48000 samples,
// 16th = 12000 samples. Span = 768000 samples = 16 s = 4 bars of 4/4 = 64 sixteenths,
// and 768000 = 25 x 30720.
constexpr SweepConfig configExactGrid { 48000.0, 60.0, 768000 };
constexpr double exactStepSamples = 12000.0;
constexpr int exactGridBars = 4;
constexpr int stepsPerBar = 16; ///< 4/4 at a 16th-note grid (scaffoldStepPpq = 0.25).

// Config B: a NON-INTEGER step grid. 137 BPM @ 44.1 kHz ⇒ 16th = 661500/137 =
// 4828.467… samples. Span = 614400 samples = 20 x 30720.
constexpr SweepConfig configFractionalGrid { 44100.0, 137.0, 614400 };

/** Renders one block size of a config, with an optional command schedule. The rig is
    constructed fresh per call, so each render starts from a pristine transport at
    PPQ 0 with an empty sounding-note table. */
MidiRenderResult renderSweep (const SweepConfig& sweep, int blockSize, const std::vector<ScheduledCommand>& schedule)
{
    SequencerRig rig { sweep.sampleRate, blockSize };

    auto config = MidiRenderConfig::samples (sweep.spanSamples, sweep.sampleRate, blockSize);
    config.numChannels = 1;      // MIDI-only node; the scratch buffer only carries a length
    config.eventReserve = 16384; // > any swept render, so the loop never reallocates

    return renderSequencer (rig, config, schedule);
}

/** Sets the config's tempo and starts the transport, both at absolute sample 0 (a
    block head at every swept size).

    The tempo command is NOT optional: a fresh `Transport` runs at
    `Transport::defaultBpm` (120), so a sweep that only pushed `transportPlay` would
    silently render a different musical configuration than the one it asserts about.
    The exact-grid case's absolute position assertions (12000-sample steps) are the
    guard: at the default tempo they are 6000 apart and the test fails. */
std::vector<ScheduledCommand> startPlaying (const SweepConfig& sweep)
{
    return { ScheduledCommand { 0, engineCommand (EngineCommandType::setTempoBpm, sweep.bpm) },
             ScheduledCommand { 0, engineCommand (EngineCommandType::transportPlay) } };
}

/** Note-ons in the render, in order. */
std::vector<TimedMidiEvent> noteOnsOf (const MidiRenderResult& render)
{
    return render.select ([] (const TimedMidiEvent& event) { return event.message.isNoteOn (); });
}

/** Note-offs in the render, in order (velocity-0 note-ons included, per JUCE). */
std::vector<TimedMidiEvent> noteOffsOf (const MidiRenderResult& render)
{
    return render.select ([] (const TimedMidiEvent& event) { return event.message.isNoteOff (); });
}

/** Number of events whose absolute position falls outside the rendered span — a
    render must never place an event before its start or past its end. */
int outsideSpanCount (const MidiRenderResult& render)
{
    int count = 0;
    for (const auto& event : render)
        if (event.absoluteSample < 0 || event.absoluteSample >= render.numSamples)
            ++count;
    return count;
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 5.3c — the sweep: an exact-integer step grid
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/timing: an exact-integer step grid renders byte-identically at every block size",
           "[midi-conformance]")
{
    // 4 bars x 16 steps: computed from first principles AND stated as a literal, so a
    // formula that mirrored a bug in the node could not agree with both.
    const int expectedSteps =
        expectedScaffoldSteps (configExactGrid.spanSamples, configExactGrid.bpm, configExactGrid.sampleRate);
    REQUIRE (expectedSteps == exactGridBars * stepsPerBar);
    REQUIRE (expectedSteps == 64);

    // The grid really is an exact integer number of samples — the property that makes
    // this config the discriminating one for the sample-offset snap.
    REQUIRE (samplesPerScaffoldStep (configExactGrid.bpm, configExactGrid.sampleRate) == exactStepSamples);
    const std::int64_t gateSamples = scaffoldGateSamples (configExactGrid.bpm, configExactGrid.sampleRate);
    REQUIRE (gateSamples == 6000); // 50% of 12000

    MidiRenderResult reference;
    std::vector<std::uint8_t> referenceBytes;
    bool sawStepsOnBlockEdges = false;
    bool sawStepsMidBlock = false;

    for (const int blockSize : sweptBlockSizes)
    {
        INFO ("block size " << blockSize);

        // Precondition, asserted rather than assumed: the span is covered by whole
        // blocks at this size, so every render spans the identical samples.
        REQUIRE (configExactGrid.spanSamples % blockSize == 0);
        REQUIRE (blockAlignmentUnit % blockSize == 0);
        REQUIRE (scheduleIsBlockAligned (startPlaying (configExactGrid), blockSize));

        const auto render = renderSweep (configExactGrid, blockSize, startPlaying (configExactGrid));
        REQUIRE (render.numSamples == configExactGrid.spanSamples);

        // §5.5: within a block, events are strictly sample-sorted — checked across the
        // whole render, and no event may escape the rendered span.
        REQUIRE (render.isSampleSorted ());
        REQUIRE (outsideSpanCount (render) == 0);

        // NON-VACUITY: the expected number of steps actually fired, each with its
        // note-off. A silently empty render cannot reach the comparison below.
        const auto ons = noteOnsOf (render);
        const auto offs = noteOffsOf (render);
        INFO (render.describe (8));
        REQUIRE (static_cast<int> (ons.size ()) == expectedSteps);
        REQUIRE (static_cast<int> (offs.size ()) == expectedSteps); // last gate ends at 759000 < span
        REQUIRE (render.size () == ons.size () + offs.size ());     // nothing else emitted

        // THE offset-snap assertion, stated absolutely: step n is at exactly
        // 12000*n and its off at 12000*n + 6000. Without the snap, some of these land
        // one sample early — and WHICH ones depends on the block size.
        for (std::size_t i = 0; i < ons.size (); ++i)
        {
            const auto expectedOn = static_cast<std::int64_t> (i) * 12000;
            INFO ("step " << i << ": " << ons[i].describe ());
            REQUIRE (ons[i].absoluteSample == expectedOn);
            REQUIRE (offs[i].absoluteSample == expectedOn + gateSamples);
        }

        // Both boundary families must be exercised by the sweep: 12000 is a whole
        // number of blocks at 32/96/480 (every step lands on a block EDGE) and is not
        // at 64/128/256/512/1024/2048 (every step lands MID-BLOCK). Recorded here and
        // asserted after the loop, so a future edit that dropped one family is caught.
        const bool gridDividesIntoBlocks = (12000 % blockSize) == 0;
        int onEdge = 0;
        for (const auto& on : ons)
            if (on.absoluteSample % blockSize == 0)
                ++onEdge;

        if (gridDividesIntoBlocks)
        {
            REQUIRE (onEdge == static_cast<int> (ons.size ())); // all on block edges
            sawStepsOnBlockEdges = true;
        }
        else
        {
            REQUIRE (onEdge < static_cast<int> (ons.size ())); // at least one mid-block
            sawStepsMidBlock = true;
        }

        // Every note-on is released: the balance closes for this config because the
        // final gate (759000) ends inside the span.
        NoteLifecycleTracker tracker;
        tracker.observeAll (render);
        INFO (tracker.describe ());
        REQUIRE (tracker.noteOnsSeen () == expectedSteps);
        REQUIRE (tracker.balanced ());

        // THE determinism comparison (§1.2), byte-for-byte against the 32-sample
        // render — and against its canonical serialization, which is the Phase-6
        // golden-file target.
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

    REQUIRE (sawStepsOnBlockEdges);
    REQUIRE (sawStepsMidBlock);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5.3c — the sweep: a fractional step grid
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/timing: a fractional step grid renders byte-identically at every block size",
           "[midi-conformance]")
{
    // 137 BPM @ 44.1 kHz: nothing lines up. 614400 samples spans 31.811… quarters =
    // 127.245… steps, so steps 0..127 fire (the half-open interval excludes nothing
    // here — the 128th boundary is simply inside).
    const int expectedSteps = expectedScaffoldSteps (configFractionalGrid.spanSamples,
                                                     configFractionalGrid.bpm,
                                                     configFractionalGrid.sampleRate);
    REQUIRE (expectedSteps == 128);

    const double stepSamples = samplesPerScaffoldStep (configFractionalGrid.bpm, configFractionalGrid.sampleRate);
    REQUIRE (stepSamples > 4828.0);
    REQUIRE (stepSamples < 4829.0); // deliberately NOT an integer

    MidiRenderResult reference;
    std::vector<std::uint8_t> referenceBytes;

    for (const int blockSize : sweptBlockSizes)
    {
        INFO ("block size " << blockSize);
        REQUIRE (configFractionalGrid.spanSamples % blockSize == 0);

        const auto render = renderSweep (configFractionalGrid, blockSize, startPlaying (configFractionalGrid));
        REQUIRE (render.numSamples == configFractionalGrid.spanSamples);
        REQUIRE (render.isSampleSorted ());
        REQUIRE (outsideSpanCount (render) == 0);

        const auto ons = noteOnsOf (render);
        const auto offs = noteOffsOf (render);
        INFO (render.describe (8));
        REQUIRE (static_cast<int> (ons.size ()) == expectedSteps);

        // The final step (n = 127, at sample 613215) has a 2414-sample gate, so its
        // note-off is due at 615629 — PAST the rendered span. It is therefore still
        // pending when the render ends, which is not a leak: the note-off ownership
        // invariant (§5.5) is asserted at FLUSH POINTS (see sequencer_node.cpp), not
        // at an arbitrary mid-gate cut. Exactly one note is outstanding.
        REQUIRE (static_cast<int> (offs.size ()) == expectedSteps - 1);
        REQUIRE (render.size () == ons.size () + offs.size ());

        NoteLifecycleTracker tracker;
        tracker.observeAll (render);
        INFO (tracker.describe ());
        REQUIRE (tracker.orphanNoteOffs () == 0); // no off ever preceded its on
        REQUIRE (tracker.outstanding () == 1);    // the note the render cut in half

        // The grid is regular even though it is fractional: consecutive steps are one
        // step apart, up to the single-sample floor of the offset conversion.
        for (std::size_t i = 1; i < ons.size (); ++i)
        {
            const auto delta = ons[i].absoluteSample - ons[i - 1].absoluteSample;
            INFO ("step " << i << " delta " << delta);
            REQUIRE (static_cast<double> (delta) >= stepSamples - 1.0);
            REQUIRE (static_cast<double> (delta) <= stepSamples + 1.0);
        }

        if (reference.empty ())
        {
            reference = render;
            referenceBytes = render.toByteStream ();
        }
        else
        {
            INFO (reference.describeDifference (render));
            REQUIRE (render == reference);
            REQUIRE (render.toByteStream () == referenceBytes);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 5.3c — the sweep, through a tempo change
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/timing: a mid-render tempo change stays buffer-size independent", "[midi-conformance]")
{
    // A tempo change makes the transport RE-ANCHOR its tempo segment (Transport.h), so
    // the step grid after the change is derived from a fresh (anchorSample, anchorPpq)
    // pair. This pins that the re-anchoring is buffer-size independent too — otherwise
    // every golden that contains a tempo change would be block-size specific.
    //
    // The change lands at sample 30720 = lcm of the swept block sizes, so it is a block
    // HEAD everywhere (commands are consumed at block heads — see SequencerRenderRig.h).
    // At 60 BPM that sample is PPQ 0.64, deliberately NOT on a step boundary: the
    // change has to be handled mid-step, not at a convenient grid point.
    auto schedule = startPlaying (configExactGrid); // 60 BPM from sample 0
    schedule.push_back (ScheduledCommand { blockAlignmentUnit, engineCommand (EngineCommandType::setTempoBpm, 175.0) });

    MidiRenderResult reference;
    std::vector<std::uint8_t> referenceBytes;

    for (const int blockSize : sweptBlockSizes)
    {
        INFO ("block size " << blockSize);
        REQUIRE (scheduleIsBlockAligned (schedule, blockSize));

        const auto render = renderSweep (configExactGrid, blockSize, schedule);
        REQUIRE (render.numSamples == configExactGrid.spanSamples);
        REQUIRE (render.isSampleSorted ());
        REQUIRE (outsideSpanCount (render) == 0);

        // Non-vacuity: 60 → 175 BPM means MORE steps than the constant-tempo render's
        // 64, and the first steps (before the change) are unaffected.
        const auto ons = noteOnsOf (render);
        INFO (render.describe (8));
        REQUIRE (ons.size () > 64u);
        REQUIRE (ons.front ().absoluteSample == 0);
        REQUIRE (ons[1].absoluteSample == 12000); // still the 60 BPM grid at PPQ 0.25

        NoteLifecycleTracker tracker;
        tracker.observeAll (render);
        INFO (tracker.describe ());
        REQUIRE (tracker.orphanNoteOffs () == 0);

        if (reference.empty ())
        {
            reference = render;
            referenceBytes = render.toByteStream ();
        }
        else
        {
            INFO (reference.describeDifference (render));
            REQUIRE (render == reference);
            REQUIRE (render.toByteStream () == referenceBytes);
        }
    }

    // ...and the tempo change is not a no-op: the same span at a constant 60 BPM is a
    // DIFFERENT performance. Without this, a `setTempoBpm` that was silently dropped
    // would make the sweep above pass for the wrong reason.
    const auto constantTempo = renderSweep (configExactGrid, 128, startPlaying (configExactGrid));
    REQUIRE (constantTempo != reference);
    REQUIRE (constantTempo.toByteStream () != referenceBytes);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5.3c — no step is duplicated or skipped at a block edge (the step-index snap)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/timing: no step is duplicated or skipped at a block edge", "[midi-conformance]")
{
    // THE GUARD FOR `stepIndexSnapSteps` specifically, and it needs a different shape
    // from the sweeps above. Snap #1 protects the seam between consecutive blocks:
    // block k's `blockEndPpq()` and block k+1's `blockStartPpq()` are equal only in
    // exact arithmetic and disagree by an ulp at ~24% of block edges. A step boundary
    // landing inside that disagreement is emitted TWICE or NOT AT ALL. But the
    // disagreements that actually straddle a boundary are rare (SequencerProcessor.h
    // measured 27123 out of 1.08e8 edges, ~1 in 4000), so a sweep over two or three
    // tempi can miss every one of them — verified: removing snap #1 leaves the
    // exact-grid, fractional-grid and tempo-change sweeps GREEN.
    //
    // So this case trades depth for breadth: many (tempo, sample rate, block size)
    // combinations over a short span, checking a property that needs no reference
    // render and no closed-form count.
    //
    // THE PROPERTY: consecutive steps of the scaffold pattern never repeat a pitch, and
    // the 16-step pitch cycle is fixed. So walking the emitted note-ons against that
    // cycle detects a duplicated step (the same pitch twice) and a skipped step (the
    // cycle jumps) with no dependence on floating-point endpoint rounding — which a
    // total-count comparison would have, since `ceil()` of a value an ulp below an
    // integer is a legitimate off-by-one for a CORRECT engine.
    constexpr int cyclePitches[16] = { 60, 62, 64, 65, 67, 69, 71, 72, 60, 62, 64, 65, 67, 69, 71, 72 };
    constexpr std::int64_t spanSamples = 61440; // 2 x the alignment unit

    int sequenceBreaks = 0;
    int coincidentSteps = 0;
    std::int64_t totalSteps = 0;
    int combinations = 0;
    double firstBadBpm = 0.0;
    double firstBadSampleRate = 0.0;
    int firstBadBlockSize = 0;

    for (const double sampleRate : { 44100.0, 48000.0 })
    {
        for (const int blockSize : { 128, 512 })
        {
            // 20 → 300 BPM in 2 BPM steps: 141 tempi, each a different (and mostly
            // irrational-in-samples) relationship between the step grid and the block
            // grid, which is what makes the seam disagreement reachable.
            for (double bpm = 20.0; bpm <= 300.0; bpm += 2.0)
            {
                ++combinations;

                SequencerRig rig { sampleRate, blockSize };
                juce::AudioBuffer<float> audio (1, blockSize);
                juce::MidiBuffer midi;
                midi.ensureSize (8192);

                rig.transport.applyCommand (engineCommand (EngineCommandType::setTempoBpm, bpm));
                rig.transport.applyCommand (engineCommand (EngineCommandType::transportPlay));

                const int blocks = static_cast<int> (spanSamples / blockSize);
                std::int64_t stepsSeen = 0;
                std::int64_t previousOnSample = -1;
                bool flagged = false;

                // No Catch2 macros in this loop: ~170k blocks total, aggregated after.
                for (int block = 0; block < blocks; ++block)
                {
                    midi.clear ();
                    rig.renderBlock (audio, midi);

                    const std::int64_t base = static_cast<std::int64_t> (block) * blockSize;

                    for (const auto meta : midi)
                    {
                        const auto message = meta.getMessage ();
                        if (! message.isNoteOn ())
                            continue;

                        const int expectedPitch = cyclePitches[stepsSeen % 16];
                        const std::int64_t onSample = base + meta.samplePosition;

                        if (message.getNoteNumber () != expectedPitch)
                        {
                            ++sequenceBreaks;
                            flagged = true;
                        }

                        // Two steps on the same sample is the other face of a duplicate.
                        if (onSample == previousOnSample)
                        {
                            ++coincidentSteps;
                            flagged = true;
                        }

                        previousOnSample = onSample;
                        ++stepsSeen;
                    }
                }

                totalSteps += stepsSeen;

                if (flagged && firstBadBlockSize == 0)
                {
                    firstBadBpm = bpm;
                    firstBadSampleRate = sampleRate;
                    firstBadBlockSize = blockSize;
                }
            }
        }
    }

    INFO ("swept " << combinations << " (tempo, sample rate, block size) combinations, " << totalSteps << " steps");
    INFO ("first offending combination: " << firstBadBpm << " BPM @ " << firstBadSampleRate << " Hz, block "
                                          << firstBadBlockSize);

    REQUIRE (sequenceBreaks == 0);
    REQUIRE (coincidentSteps == 0);
    REQUIRE (combinations == 564);
    REQUIRE (totalSteps > 5000); // non-vacuous: the matrix really played (≈8300 steps)
}

// ─────────────────────────────────────────────────────────────────────────────
// 5.3c — no event may leave its own block
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/timing: every emitted event lands inside its own block", "[midi-conformance]")
{
    // `juce::MidiBuffer::addEvent` does NOT validate the sample offset, and a step
    // boundary landing on a block edge is exactly where an off-by-one would put an
    // event at `numSamples` (i.e. in the next block, at the wrong absolute sample and
    // out of order relative to that block's own events). The absolute-position stream
    // the harness returns cannot see a raw offset, so this case renders block by block
    // and inspects `samplePosition` directly — it is the guard for the explicit clamp
    // in `SequencerProcessor::processBlock`.
    //
    // A stop is included mid-render because a flush emits at offset 0 with a
    // full-table burst behind it — the other place an offset could go out of range.
    for (const int blockSize : sweptBlockSizes)
    {
        INFO ("block size " << blockSize);

        SequencerRig rig { 48000.0, blockSize };
        juce::AudioBuffer<float> audio (1, blockSize);
        juce::MidiBuffer midi;
        midi.ensureSize (8192);

        const int blocks = static_cast<int> (blockAlignmentUnit * 2 / blockSize);
        REQUIRE (blocks > 1);

        rig.transport.applyCommand (engineCommand (EngineCommandType::transportPlay));

        int outOfRange = 0;
        int outOfOrder = 0;
        int events = 0;
        std::int64_t previousAbsolute = -1;

        for (int block = 0; block < blocks; ++block)
        {
            if (block == blocks / 2)
                rig.transport.applyCommand (engineCommand (EngineCommandType::transportStop));
            else if (block == blocks / 2 + 1)
                rig.transport.applyCommand (engineCommand (EngineCommandType::transportPlay));

            midi.clear ();
            rig.renderBlock (audio, midi);

            const std::int64_t base = static_cast<std::int64_t> (block) * blockSize;

            for (const auto meta : midi)
            {
                ++events;

                if (meta.samplePosition < 0 || meta.samplePosition >= blockSize)
                    ++outOfRange;

                const std::int64_t absolute = base + meta.samplePosition;
                if (absolute < previousAbsolute)
                    ++outOfOrder;
                previousAbsolute = absolute;
            }
        }

        REQUIRE (events > 0); // non-vacuous: something really was emitted
        REQUIRE (outOfRange == 0);
        REQUIRE (outOfOrder == 0);
    }
}
