// ─────────────────────────────────────────────────────────────────────────────
// substep_ownership — THE BACKSTOP FOR PHASE 7.2's NEW GEOMETRY.
//
// ── WHAT CHANGED, AND WHY IT NEEDS ITS OWN FILE ─────────────────────────────
// Until Phase 7.2 the step walk asked ONE question — "is step INDEX n inside this
// block's index range?" — and that question was both "which indices do I look at"
// and "which indices are mine". MICRO, swing and ratchets broke the second meaning:
// an event's PLACED position can sit in a different block from its grid position, and
// a step now places up to eight events at distinct sub-step offsets. So the walk
// grew a widened scan (`stepScanBack == 2`, `stepScanForward == 1`) and ownership
// moved to a per-event predicate (`ownsPpq`) against the placed PPQ.
//
// That replaces a total function ("every index in [first, end) is emitted, exactly
// once, by exactly this block") with a tiling argument. A tiling can fail in two
// directions and BOTH are invisible to every determinism test in the suite:
//
//   * A GAP — an event owned by NO block. It is DROPPED. Because the geometry is
//     identical at every buffer size, it is dropped identically at every buffer
//     size, so a cross-block-size comparison sees two matching streams and passes.
//   * AN OVERLAP — an event owned by TWO blocks. It is DUPLICATED, again uniformly.
//
// ── SO THE LOAD-BEARING ASSERTION HERE IS A LITERAL COUNT, NOT A SWEEP ──────
// This is the one file in the determinism suite whose primary guard is a number
// written in the source. `expectedPairCount` (768) is derived below from the lane
// tables written in THIS file, and every swept block size must produce exactly that
// many note-ons. Remove `stepScanBack` from the walk and notes vanish — 692 of the
// 768 at block size 32, because a positively displaced event's grid index sits in an
// earlier block than the block that contains it — and they vanish at all ten sizes
// alike. `REQUIRE_SWEEP_CLEAN`-style cross-size comparison cannot see that. The
// count can, and does.
//
// ── AND THE WIDENING'S BOUNDS ARE PROVED *ATTAINED*, NOT ASSUMED ────────────
// `stepScanBack`/`stepScanForward` are documented as TIGHT rather than padded. An
// unattained bound is untested padding: nobody would ever learn that
// `stepScanBack == 1` also passes, so nobody would learn that raising
// `maxSubStepShiftSteps` had quietly started dropping notes. This file therefore
// classifies EVERY emitted note by where its index sits relative to the emitting
// block's own index range, and requires:
//
//     min (index - firstIndex) == -2   ⇒ the scan-BACK bound is reached exactly
//     max (index - endIndex)   ==  0   ⇒ the scan-FORWARD bound is reached exactly
//
// Both are observed from the RENDERED STREAM, not from the model: the fixture is
// collision-free (asserted), so a note's absolute sample identifies its
// `(index, child)` pair uniquely and the classification is a fact about what the
// engine emitted.
//
// ── THE CLOCK, AND WHY EVERY NUMBER HERE IS AN EXACT INTEGER ────────────────
// 300 BPM (`Transport::maxBpm`) @ 48 kHz on the 1/32 grid ⇒ ONE STEP IS EXACTLY
// 1200 SAMPLES. MICRO is stored as a percentage of the step, so a MICRO value of m
// displaces by exactly `12 * m` samples; swing at 75 % displaces an odd step by
// exactly 600; and ratchet counts are restricted to {1, 2, 4, 8}, whose slots are
// 1200 / 600 / 300 / 150 samples. Every onset in this file is therefore an exact
// integer and the prediction below needs no tolerance at all — which is what lets a
// MULTISET EQUALITY (predicted onsets vs emitted onsets, per block) stand in for
// "each (index, child) is emitted exactly once, by the right block".
//
// RATCHET 7 is deliberately ABSENT: its slot is 1200/7 = 171.43 samples and the
// prediction would need to reproduce the engine's snap-then-floor. That rounding is
// pinned in tests/step_ratchet.cpp, where it is the subject rather than a nuisance.
//
// ── WHY THE MICRO VALUES LOOK ARBITRARY ─────────────────────────────────────
// {0, +37, -41, +23, -17} — odd, non-round percentages on purpose. The obvious
// choice (multiples of 25) puts every displacement on the same 150-sample grid the
// ratchet slots use, and since 1200 is a multiple of 150 that makes ADJACENT STEPS
// COLLIDE: measured, 83 samples carried two events each. Collisions do not break a
// multiset comparison, but they do break the sample → `(index, child)` map this file
// needs in order to classify a note by its index. These five values are
// collision-free over the whole span (asserted), so the map is a bijection.
//
// Two of them SATURATE the composed clamp (steps 1 and 3: +0.37 and +0.23 on top of
// swing's +0.5, both pinned at +0.5), so the widened scan is exercised at its bound
// rather than near it.
// ─────────────────────────────────────────────────────────────────────────────

#include "support/MidiRenderHarness.h"
#include "support/SequencerRenderRig.h"

#include "engine/graph/EngineCommand.h"
#include "engine/graph/Transport.h"
#include "engine/sequencer/PatternDocument.h"
#include "engine/sequencer/PatternTypes.h"
#include "engine/sequencer/SequencerProcessor.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

using arpbox::engine::EngineCommandType;
using arpbox::engine::LaneId;
using arpbox::engine::maxSteps;
using arpbox::engine::maxSubStepShiftSteps;
using arpbox::engine::PatternDocument;
using arpbox::engine::Transport;
using arpbox::testing::engineCommand;
using arpbox::testing::MidiRenderConfig;
using arpbox::testing::MidiRenderResult;
using arpbox::testing::renderSequencer;
using arpbox::testing::ScheduledCommand;
using arpbox::testing::SequencerRig;

namespace
{
// ─────────────────────────────────────────────────────────────────────────────
// The clock and the sweep — all literals, all exact
// ─────────────────────────────────────────────────────────────────────────────

constexpr double ownSampleRate = 48000.0;
constexpr double ownBpm = 300.0;         ///< Transport::maxBpm — asserted, not assumed.
constexpr double ownGridStepPpq = 0.125; ///< 1/32 (§2.1 allows 1/32..1/4).

/** One step: 0.125 x (60 / 300) x 48000 = 1200, EXACTLY. Every displacement and
    every child offset below is an exact integer number of samples because of it. */
constexpr std::int64_t stepSamples = 1200;

/** lcm {32, 64, 96, 128, 256, 480, 512, 1024, 2048, 4096} = 2^12 x 3 x 5. */
constexpr std::int64_t alignmentUnit = 61440;

/** The rendered span: 4 x 61440 = 245760 samples = 204.8 steps. A whole number of
    blocks at every swept size, and deliberately NOT a whole number of steps, so the
    render's own end is mid-step and the last block's forward scan is meaningful. */
constexpr std::int64_t ownSpan = 4 * alignmentUnit;

constexpr int sweptBlockSizes[] = { 32, 64, 96, 128, 256, 480, 512, 1024, 2048, 4096 };
constexpr int numSweptBlockSizes = static_cast<int> (std::size (sweptBlockSizes));

/** Project swing: the §12.1 maximum, so an odd step is displaced by exactly
    +0.5 step = +600 samples. */
constexpr double ownSwingPct = 75.0;

/** MICRO lane, length 5 — see "WHY THE MICRO VALUES LOOK ARBITRARY" above. */
constexpr int microLane[] = { 0, 37, -41, 23, -17 };
constexpr int microLaneLength = static_cast<int> (std::size (microLane));

/** RATCHET lane, length 4. {1, 2, 4, 8} only: their slots (1200, 600, 300, 150) are
    exact, which the prediction below depends on. */
constexpr int ratchetLane[] = { 1, 2, 4, 8 };
constexpr int ratchetLaneLength = static_cast<int> (std::size (ratchetLane));

/** LEN, short on purpose: this file only ever looks at note-ONS, and a short gate
    keeps the sounding-note table shallow so nothing can be suppressed by it. */
constexpr int ownLenPercent = 25;

/** Every `(index, child)` pair whose onset lands inside `[0, ownSpan)`. Derived
    below from the lane tables above; pinned as a literal so a uniform drop reddens.

    766 of them come from steps 0..204. THE OTHER TWO COME FROM STEP -1, and they are
    not a modelling accident — see `firstModelledIndex`. */
constexpr int expectedPairCount = 768;

// ─────────────────────────────────────────────────────────────────────────────
// The model — written HERE, from §12.1's semantics, never read off the engine
// ─────────────────────────────────────────────────────────────────────────────

/** Floor-modulus, matching `arpbox::engine::stepFloorMod`'s contract for the lane
    index (C++ `%` would give a negative residue and pair the negative timeline half
    a step out of phase). Only non-negative indices are used here, but the model
    should not be the place that assumes it. */
std::int64_t floorMod (std::int64_t value, std::int64_t modulus) noexcept
{
    const std::int64_t remainder = value % modulus;

    return remainder < 0 ? remainder + modulus : remainder;
}

/** The composed MICRO + swing displacement of step `index`, in SAMPLES.

    MICRO first, then swing added on top (§12.1 "swing applies on top"), then ONE
    clamp on the TOTAL to ±`maxSubStepShiftSteps`. Clamping per source would admit
    ±1.0 and is the fails-without for tests/step_microswing.cpp; here the composed
    clamp is what keeps the predicted onsets inside the widened scan's reach. */
std::int64_t shiftSamplesOf (std::int64_t index) noexcept
{
    const double micro = static_cast<double> (microLane[floorMod (index, microLaneLength)]) / 100.0;
    const double swing = floorMod (index, 2) == 1 ? (ownSwingPct / 100.0 - 0.5) * 2.0 : 0.0;
    const double composed = std::max (-maxSubStepShiftSteps, std::min (maxSubStepShiftSteps, micro + swing));

    // Exact by construction: `composed` is either a clamp bound (±0.5) or `12 * m`
    // hundredths of a 1200-sample step, so the product is an integer.
    return static_cast<std::int64_t> (std::llround (composed * static_cast<double> (stepSamples)));
}

/** How many notes step `index` fires (§12.1 RATCHET). PROB stays at its default 100
    throughout this file, so no child is thinned and this IS the emitted count. */
int childCountOf (std::int64_t index) noexcept
{
    return ratchetLane[floorMod (index, ratchetLaneLength)];
}

/** One predicted event. */
struct SubStepPair
{
    std::int64_t index = 0;  ///< Global step index.
    int child = 0;           ///< Ratchet child, `[0, childCountOf (index))`.
    std::int64_t sample = 0; ///< Absolute onset.
};

/** The lowest step index the model has to consider, and A FINDING IN ITS OWN RIGHT.

    ── A STEP BEFORE THE TIMELINE STARTS CAN STILL BE HEARD ────────────────────
    The first version of this model started at index 0 and came up TWO NOTES SHORT of
    what the engine emitted. The two are step **-1**'s children 6 and 7.

    Step -1's grid position is PPQ -0.0625. It is displaced LATE (MICRO -17 % on an odd
    index composes with swing's +0.5 to +0.33 of a step), and its ratchet children run
    a further 7/8 of a step ahead, so children 6 and 7 land at PPQ +0.008 and +0.0256 —
    i.e. at samples 96 and 246, INSIDE the played timeline. The walk reaches index -1
    from block 0 through `stepScanBack`, `ownsPpq` accepts both positions, and the
    engine emits them.

    THAT IS CORRECT, and it is worth stating out loud because it looks wrong: the
    events are at positive PPQ, so they are inside the performance, and dropping them
    would mean the walk's ownership test disagreed with its own scan. It is audible as
    the tail of a ratchet that began just before the loop point — the same thing a
    negatively displaced step at the END of a render does at the other edge.

    Only index -1 contributes: -2's four children reach at most -1224 samples, and
    -3's two reach -2892. Both are checked below, so a future MICRO/RATCHET edit that
    pulled a third index into range would redden the count rather than silently pass. */
constexpr std::int64_t firstModelledIndex = -4;

/** Every pair whose onset lands inside `[0, ownSpan)`.

    THE RANGE IS WIDER THAN THE SPAN IN BOTH DIRECTIONS, deliberately, and BOTH edges
    turned out to matter: a NEGATIVELY displaced step past the span's end can still
    place an event inside it (step 205's grid position is 246000, past the end, but a
    -492-sample MICRO puts it at 245508), and a POSITIVELY displaced step before 0 can
    too (see `firstModelledIndex`). Getting either wrong shows up immediately as a
    predicted-vs-emitted mismatch rather than as a silent miscount — which is exactly
    how the second one was found. */
std::vector<SubStepPair> expectedPairs ()
{
    std::vector<SubStepPair> pairs;
    pairs.reserve (static_cast<std::size_t> (expectedPairCount));

    const std::int64_t lastIndex = ownSpan / stepSamples + 4;

    for (std::int64_t index = firstModelledIndex; index <= lastIndex; ++index)
    {
        const int count = childCountOf (index);
        const std::int64_t base = index * stepSamples + shiftSamplesOf (index);

        for (int child = 0; child < count; ++child)
        {
            const std::int64_t sample = base + static_cast<std::int64_t> (child) * stepSamples / count;

            if (sample >= 0 && sample < ownSpan)
                pairs.push_back (SubStepPair { index, child, sample });
        }
    }

    return pairs;
}

/** `snappedStepCeiling` in exact integer arithmetic. For a non-negative integer
    sample count the walk's `ceil (ppq / stepPpq - 1e-6)` is exactly this: the
    tolerance only ever matters when the quotient is an integer, where both forms
    give the quotient itself. */
std::int64_t stepCeiling (std::int64_t sample) noexcept
{
    return (sample + stepSamples - 1) / stepSamples;
}

// ─────────────────────────────────────────────────────────────────────────────
// The configuration
// ─────────────────────────────────────────────────────────────────────────────

void configureSubStep (PatternDocument& document)
{
    document.beginTransaction ();
    document.setGrid (ownGridStepPpq);
    document.setSwing (ownSwingPct);

    document.setLaneLength (0, LaneId::micro, microLaneLength);
    document.setLaneLength (0, LaneId::ratchet, ratchetLaneLength);

    for (int step = 0; step < microLaneLength; ++step)
        document.setLaneValue (0, LaneId::micro, step, microLane[step]);

    for (int step = 0; step < ratchetLaneLength; ++step)
        document.setLaneValue (0, LaneId::ratchet, step, ratchetLane[step]);

    for (int step = 0; step < maxSteps; ++step)
    {
        document.setLaneValue (0, LaneId::gate, step, 1);
        document.setLaneValue (0, LaneId::len, step, ownLenPercent);
    }

    document.endTransaction ();
}

/** THE NEGATIVE CONTROL: one MICRO value moved by one percent. Everything about the
    shape stays identical — same pair count, same child counts — but 12 samples of
    displacement move a whole family of onsets, so the stream must NOT match. */
void configureSubStepPerturbed (PatternDocument& document)
{
    configureSubStep (document);
    document.setLaneValue (0, LaneId::micro, 1, microLane[1] + 1);
}

std::vector<ScheduledCommand> playSchedule ()
{
    return { ScheduledCommand { 0, engineCommand (EngineCommandType::setTempoBpm, ownBpm) },
             ScheduledCommand { 0, engineCommand (EngineCommandType::transportPlay) } };
}

MidiRenderResult renderAt (void (*configure) (PatternDocument&), int blockSize)
{
    SequencerRig rig { ownSampleRate, blockSize };
    configure (rig.patternDocument);

    auto config = MidiRenderConfig::samples (ownSpan, ownSampleRate, blockSize);
    config.numChannels = 1;
    config.eventReserve = 16384;

    return renderSequencer (rig, config, playSchedule ());
}

std::vector<std::int64_t> noteOnSamplesOf (const MidiRenderResult& render)
{
    std::vector<std::int64_t> samples;

    for (const auto& event : render.events)
        if (event.message.isNoteOn ())
            samples.push_back (event.absoluteSample);

    return samples;
}

// ─────────────────────────────────────────────────────────────────────────────
// What one swept block size observed. House rule: no Catch2 macro in a loop.
// ─────────────────────────────────────────────────────────────────────────────

struct OwnershipSweep
{
    int sizesChecked = 0;
    int sizesWithExactCount = 0;    ///< Sizes emitting exactly `expectedPairCount` note-ons.
    int sizesMatchingModel = 0;     ///< Sizes whose note-on onsets equal the predicted set exactly.
    int sizesMatchingReference = 0; ///< Sizes whose whole event stream matches block size 32's.

    std::int64_t minNoteOns = std::numeric_limits<std::int64_t>::max ();
    std::int64_t maxNoteOns = 0;

    /** THE WIDENING, MEASURED FROM THE EMITTED STREAM. `relativeToFirst` is
        `index - firstIndex` of the emitting block; a negative value means the note
        was only reachable because of `stepScanBack`. `relativeToEnd` is
        `index - endIndex`; a non-negative value means `stepScanForward`. */
    std::int64_t minRelativeToFirst = std::numeric_limits<std::int64_t>::max ();
    std::int64_t maxRelativeToEnd = std::numeric_limits<std::int64_t>::min ();

    std::int64_t reachedByScanBack = 0;    ///< Notes whose index is BELOW the emitting block's range.
    std::int64_t reachedByScanForward = 0; ///< Notes whose index is AT OR ABOVE it.

    int sizesWithScanBack = 0;    ///< Sizes where the backward band produced at least one note.
    int sizesWithScanForward = 0; ///< Sizes where the forward band did.

    /** Notes whose onset matched no predicted pair. MUST be 0 — it would mean the
        engine placed an event the model cannot account for. */
    std::int64_t unexplainedNotes = 0;

    juce::String report;
};
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 0. The fixture's geometry, asserted once for the whole file
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/substep-ownership: the fixture's onsets are exact, collision-free and saturating",
           "[unit][determinism]")
{
    // THE CLOCK. Pinned against the transport's own maximum and against arithmetic,
    // because a test that silently ran at the default 120 BPM would have 2400-sample
    // steps and every literal below would be wrong by a factor of two.
    REQUIRE (ownBpm == Transport::maxBpm);
    REQUIRE (ownGridStepPpq * (60.0 / ownBpm) * ownSampleRate == Catch::Approx (1200.0).margin (1.0e-9));
    REQUIRE (stepSamples == 1200);

    // Every swept size divides the span, so all ten renders cover the identical
    // musical time. Without this a cross-size comparison fails for a reason that has
    // nothing to do with ownership.
    for (const int blockSize : sweptBlockSizes)
    {
        INFO ("block size " << blockSize);
        REQUIRE (ownSpan % blockSize == 0);
    }

    REQUIRE (numSweptBlockSizes == 10);
    REQUIRE (ownSpan == 245760);
    REQUIRE (ownSpan % stepSamples == 960); // NOT a whole number of steps, on purpose

    // ── THE DISPLACEMENTS ARE EXACT, AND TWO OF THEM SATURATE ────────────────
    // `shiftSamplesOf` rounds, so a non-integer displacement would be silently
    // absorbed. Check the raw product instead, and check the saturation the widened
    // scan is meant to be exercised at.
    int saturating = 0;
    int negative = 0;
    int exact = 0;

    for (std::int64_t index = 0; index < 40; ++index)
    {
        const double micro =
            static_cast<double> (microLane[static_cast<std::size_t> (index % microLaneLength)]) / 100.0;
        const double swing = (index % 2) == 1 ? 0.5 : 0.0;

        if (std::abs (micro + swing) > maxSubStepShiftSteps)
            ++saturating;

        if (shiftSamplesOf (index) < 0)
            ++negative;

        // Exactness: the rounded displacement is within a whisker of the raw one, so
        // no onset below is a rounding artefact.
        const double composed = std::max (-maxSubStepShiftSteps, std::min (maxSubStepShiftSteps, micro + swing));
        if (std::abs (composed * static_cast<double> (stepSamples) - static_cast<double> (shiftSamplesOf (index))) <
            1.0e-9)
            ++exact;
    }

    // The MICRO lane's period is 5 and the swing parity's is 2, so the composed
    // displacement repeats every 10 steps: two of every ten saturate (indices 1 and 3
    // mod 10) and two of every ten are negative (2 and 4 mod 10). Over 40 indices that
    // is 8 of each — the floors below are non-vacuity thresholds, not a distribution.
    INFO ("saturating " << saturating << ", negative " << negative << ", exact " << exact);
    REQUIRE (exact == 40);     // every displacement is an exact integer of samples
    REQUIRE (saturating == 8); // the composed clamp really bites, on a fixed schedule
    REQUIRE (negative == 8);   // …and displacements go BOTH ways, or scan-forward is unreachable

    REQUIRE (shiftSamplesOf (0) == 0);    // step 0 is undisplaced: nothing falls off the timeline start
    REQUIRE (shiftSamplesOf (1) == 600);  // +0.37 + 0.5 = 0.87, CLAMPED to +0.5
    REQUIRE (shiftSamplesOf (2) == -492); // -0.41, even step: no swing
    REQUIRE (shiftSamplesOf (3) == 600);  // +0.23 + 0.5 = 0.73, CLAMPED
    REQUIRE (shiftSamplesOf (4) == -204); // -0.17

    // ── THE PAIR SET: COUNTED, AND COLLISION-FREE ────────────────────────────
    const auto pairs = expectedPairs ();

    std::map<std::int64_t, int> perSample;
    for (const auto& pair : pairs)
        ++perSample[pair.sample];

    int collidingSamples = 0;
    for (const auto& entry : perSample)
        if (entry.second > 1)
            ++collidingSamples;

    INFO ("pairs " << pairs.size () << ", distinct onsets " << perSample.size () << ", colliding " << collidingSamples);

    REQUIRE (static_cast<int> (pairs.size ()) == expectedPairCount);
    REQUIRE (expectedPairCount == 768);

    // ── EXACTLY TWO OF THEM COME FROM BEFORE THE TIMELINE STARTS ─────────────
    // See `firstModelledIndex`. Pinned as a count AND as the two samples, so a MICRO
    // or RATCHET edit that pulled a third index into range fails here rather than
    // silently changing what "every pair" means.
    int fromNegativeIndices = 0;
    for (const auto& pair : pairs)
        if (pair.index < 0)
            ++fromNegativeIndices;

    REQUIRE (fromNegativeIndices == 2);
    REQUIRE (shiftSamplesOf (-1) == 396); // -17 % MICRO on an ODD index, plus swing's +600
    REQUIRE (childCountOf (-1) == 8);
    REQUIRE (-1 * stepSamples + 396 + 6 * (stepSamples / 8) == 96);
    REQUIRE (-1 * stepSamples + 396 + 7 * (stepSamples / 8) == 246);
    REQUIRE (perSample.count (96) == 1u);
    REQUIRE (perSample.count (246) == 1u);

    // …and index -2 and -3 really are out of reach, so "exactly two" is a fact about
    // the geometry rather than about where the loop happened to start.
    REQUIRE (-2 * stepSamples + shiftSamplesOf (-2) + (childCountOf (-2) - 1) * (stepSamples / childCountOf (-2)) < 0);
    REQUIRE (-3 * stepSamples + shiftSamplesOf (-3) + (childCountOf (-3) - 1) * (stepSamples / childCountOf (-3)) < 0);

    // COLLISION-FREEDOM IS A PRECONDITION, NOT A NICETY. The case below classifies a
    // note by looking its onset up in this map; two pairs on one sample would make
    // that lookup ambiguous and the widening counters unsound. See the note at the
    // top on why the obvious multiple-of-25 MICRO values are not used.
    REQUIRE (collidingSamples == 0);
    REQUIRE (static_cast<int> (perSample.size ()) == expectedPairCount);

    // The onsets of the first few POSITIVE-index pairs, spelled out so a reader can
    // check the model by hand: step 0 undisplaced with one child; step 1 clamped to
    // +600 with two children 600 apart; step 2 pulled back 492 with four children
    // 300 apart.
    REQUIRE (perSample.count (0) == 1u);    // step 0, child 0
    REQUIRE (perSample.count (1800) == 1u); // step 1, child 0  (1200 + 600)
    REQUIRE (perSample.count (2400) == 1u); // step 1, child 1
    REQUIRE (perSample.count (1908) == 1u); // step 2, child 0  (2400 - 492)
    REQUIRE (perSample.count (2208) == 1u); // step 2, child 1

    // …and the onsets are NOT monotonic in (index, child) order, which is the whole
    // reason ownership had to stop being an index test: step 2's child 0 (1908) sounds
    // BEFORE step 1's child 1 (2400).
    REQUIRE (1908 < 2400);

    int inverted = 0;
    for (std::size_t i = 1; i < pairs.size (); ++i)
        if (pairs[i].sample < pairs[i - 1].sample)
            ++inverted;

    INFO ("(index, child) pairs whose onset precedes their predecessor's: " << inverted);
    REQUIRE (inverted > 50); // index order is emphatically NOT sample order (measured: 73)

    // Every ratchet count occurs, so the file is about ratchets and not about one of
    // them; and 7 is deliberately absent (see the header).
    std::map<int, int> counts;
    for (std::int64_t index = 0; index < 40; ++index)
        ++counts[childCountOf (index)];

    REQUIRE (counts.size () == 4u);
    REQUIRE (counts.count (1) == 1u);
    REQUIRE (counts.count (2) == 1u);
    REQUIRE (counts.count (4) == 1u);
    REQUIRE (counts.count (8) == 1u);
    REQUIRE (counts.count (7) == 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. Every (index, child) is emitted exactly once, by the block that contains it
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/substep-ownership: every (index, child) is emitted exactly once at every block size",
           "[determinism]")
{
    const auto pairs = expectedPairs ();

    // Onset -> pair. Injective (asserted in the case above), so a note's absolute
    // sample identifies which step and which child produced it.
    std::map<std::int64_t, SubStepPair> byOnset;
    for (const auto& pair : pairs)
        byOnset.emplace (pair.sample, pair);

    std::vector<std::int64_t> predicted;
    predicted.reserve (pairs.size ());
    for (const auto& pair : pairs)
        predicted.push_back (pair.sample);
    std::sort (predicted.begin (), predicted.end ());

    OwnershipSweep sweep;
    std::vector<std::uint8_t> referenceBytes;

    sweep.report << "sub-step ownership across " << juce::String (numSweptBlockSizes) << " block sizes ("
                 << juce::String (expectedPairCount) << " expected note-ons each):\n";

    for (const int blockSize : sweptBlockSizes)
    {
        const auto render = renderAt (&configureSubStep, blockSize);
        auto onsets = noteOnSamplesOf (render);
        const auto count = static_cast<std::int64_t> (onsets.size ());

        ++sweep.sizesChecked;

        if (count == expectedPairCount)
            ++sweep.sizesWithExactCount;

        sweep.minNoteOns = std::min (sweep.minNoteOns, count);
        sweep.maxNoteOns = std::max (sweep.maxNoteOns, count);

        auto sorted = onsets;
        std::sort (sorted.begin (), sorted.end ());

        if (sorted == predicted)
            ++sweep.sizesMatchingModel;

        const auto bytes = render.toByteStream ();
        if (sweep.sizesChecked == 1)
            referenceBytes = bytes;

        if (bytes == referenceBytes)
            ++sweep.sizesMatchingReference;

        // ── THE WIDENING, CLASSIFIED FROM WHAT THE ENGINE EMITTED ────────────
        // The emitting block is the block that CONTAINS the sample: `addEvent`'s
        // offset must be inside `[0, numSamples)`, so no other block could have
        // placed it. `firstIndex` / `endIndex` are this file's own integer
        // re-derivation of the walk's snapped ceiling.
        std::int64_t back = 0;
        std::int64_t forward = 0;

        for (const auto onset : onsets)
        {
            const auto found = byOnset.find (onset);

            if (found == byOnset.end ())
            {
                ++sweep.unexplainedNotes;
                continue;
            }

            const std::int64_t base = (onset / blockSize) * blockSize;
            const std::int64_t firstIndex = stepCeiling (base);
            const std::int64_t endIndex = stepCeiling (base + blockSize);
            const std::int64_t index = found->second.index;

            sweep.minRelativeToFirst = std::min (sweep.minRelativeToFirst, index - firstIndex);
            sweep.maxRelativeToEnd = std::max (sweep.maxRelativeToEnd, index - endIndex);

            if (index < firstIndex)
                ++back;
            else if (index >= endIndex)
                ++forward;
        }

        sweep.reachedByScanBack += back;
        sweep.reachedByScanForward += forward;
        sweep.sizesWithScanBack += back > 0 ? 1 : 0;
        sweep.sizesWithScanForward += forward > 0 ? 1 : 0;

        sweep.report << "  block " << juce::String (blockSize) << ": " << juce::String (count) << " note-ons, "
                     << (sorted == predicted ? "model OK" : "MODEL MISMATCH") << ", scan-back " << juce::String (back)
                     << ", scan-forward " << juce::String (forward) << "\n";
    }

    INFO (sweep.report);
    INFO ("min rel-to-first " << sweep.minRelativeToFirst << ", max rel-to-end " << sweep.maxRelativeToEnd
                              << ", unexplained " << sweep.unexplainedNotes);

    REQUIRE (sweep.sizesChecked == numSweptBlockSizes);

    // ── THE LOAD-BEARING LITERAL ─────────────────────────────────────────────
    // A dropped or duplicated event is dropped/duplicated IDENTICALLY at every
    // buffer size, so the two comparisons below it cannot see one. This can.
    REQUIRE (sweep.minNoteOns == expectedPairCount);
    REQUIRE (sweep.maxNoteOns == expectedPairCount);
    REQUIRE (sweep.sizesWithExactCount == numSweptBlockSizes);

    // …and each note is at the sample the geometry says, not merely present.
    REQUIRE (sweep.unexplainedNotes == 0);
    REQUIRE (sweep.sizesMatchingModel == numSweptBlockSizes);

    // The ordinary cross-size determinism claim, for completeness. It is the WEAKER
    // of the two and is here so a failure can be told apart from the count failure.
    REQUIRE (sweep.sizesMatchingReference == numSweptBlockSizes);

    // ── THE WIDENING IS ATTAINED ON BOTH SIDES, AT ITS EXACT BOUNDS ──────────
    // `stepScanBack == 2` and `stepScanForward == 1` are documented as tight. If
    // either bound were never reached it would be untested padding, and nobody would
    // discover that raising `maxSubStepShiftSteps` had started dropping notes at
    // block edges — uniformly, at every buffer size.
    REQUIRE (sweep.reachedByScanBack > 0);
    REQUIRE (sweep.reachedByScanForward > 0);
    REQUIRE (sweep.sizesWithScanBack == numSweptBlockSizes);
    REQUIRE (sweep.sizesWithScanForward == numSweptBlockSizes);
    REQUIRE (sweep.minRelativeToFirst == -2); // the SECOND backward index really is needed
    REQUIRE (sweep.maxRelativeToEnd == 0);    // and the first forward one

    // ── THE NEGATIVE CONTROL ─────────────────────────────────────────────────
    // One MICRO percent. The shape is unchanged (same pair count) and the stream is
    // different — so a comparison that always succeeded would fail here.
    const auto reference = renderAt (&configureSubStep, 128);
    const auto perturbed = renderAt (&configureSubStepPerturbed, 128);

    REQUIRE (noteOnSamplesOf (perturbed).size () == noteOnSamplesOf (reference).size ());
    REQUIRE (perturbed.toByteStream () != reference.toByteStream ());
}
