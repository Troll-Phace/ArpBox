// ─────────────────────────────────────────────────────────────────────────────
// step_microswing — MICRO-TIMING and SWING, and the ONE clamp they compose
// through (ARCHITECTURE §12.1 MICRO "-50..+50 % step / swing applies on top",
// §8.1 `transport.swingPct`; engine/sequencer/StepLogic.h `swingShiftSteps`,
// `PatternTypes.h`'s swing range, and `StepEmission::shiftSteps` as
// `evaluateStep` composes it in engine/sequencer/SequencerProcessor.cpp).
//
// ── FOUR FAILURE CLASSES, NONE OF WHICH ANY EXISTING TEST CAN SEE ───────────
// Every pre-7.2 golden was baked at swing 50 with MICRO 0, i.e. at a displacement
// of exactly zero, so the whole of this arithmetic is invisible to them.
//
//   1. `stepIndex % 2` INSTEAD OF `stepFloorMod (stepIndex, 2)`. C++ `%` yields
//      NEGATIVE residues for negative operands — `-1 % 2` is `-1`, never `1` — so a
//      `% 2 == 1` pairing test reads step -1 as EVEN and puts the entire negative
//      half of the timeline half a step out of phase with the positive half.
//      Negative indices are not hypothetical: the retrigger lookahead scans one step
//      BACK and the locate paths evaluate below zero. During a straight
//      play-from-zero render the bug is completely invisible, which is why the sweep
//      below is exhaustive from -128 and why index -1 gets an assertion of its own.
//   2. SWING 50 NOT BEING BITWISE ZERO. `swingShiftSteps` returns EXACTLY 0.0 at
//      50 %, and `placedPpq = gridPpq + shiftSteps * stepPpq` is therefore
//      BIT-IDENTICAL to `gridPpq`. That identity is the entire proof that swing's
//      arrival cannot move a single pre-7.2 golden — "approximately zero" would not
//      do, because a 1-ulp PPQ change can cross a snap window and move an emitted
//      sample.
//   3. CLAMPING PER SOURCE INSTEAD OF ONCE ON THE TOTAL. MICRO alone reaches ±0.5
//      and swing alone reaches +0.5, so two per-source clamps admit a COMPOSED ±1.0
//      — and that breaks the derivation the step walk's scan widening rests on
//      (`stepScanBack` / `stepScanForward` are computed from
//      `maxSubStepShiftSteps` and both bounds are ATTAINED, not padded). A
//      displacement past 0.5 places events in blocks the walk never visits, so they
//      VANISH — uniformly at every buffer size, which is the one failure no
//      cross-carving determinism comparison can detect. Hence the exhaustive
//      lane sweep below, and hence its anti-vacuity requirement that ±0.5 really is
//      reached (a bound nothing attains is a bound nothing checks).
//   4. TREATING THE SATURATION AS A BUG. At swing 75 an odd step with MICRO +50 and
//      an odd step with MICRO 0 land on the SAME sample. That is documented
//      behaviour, deliberately baked into the `micro-swing-compose` golden, and the
//      literal table below states it as an equality rather than leaving it to be
//      "fixed" by someone who reads it as a rounding artefact.
//
// ── WHAT IS WRITTEN INDEPENDENTLY HERE, AND WHAT IS NOT ────────────────────
// The PAIRING is: `independentIsOddStep` is `((n % 2) + 2) % 2 == 1`, a different
// expression from `stepFloorMod`'s `r < 0 ? r + b : r`, so the two cannot share a
// mistake — and the pairing is where failure class 1 lives.
//
// The DISPLACEMENT MAGNITUDE deliberately is not: `(swingPct / 100 - 0.5) * 2` is
// re-typed in the same shape the engine uses, because the assertions are BITWISE and
// an algebraically equivalent rearrangement (say `swingPct / 50 - 1`) would differ in
// the last ulp for values like 55 and 66 and would make the test fail for a reason
// that has nothing to do with swing. The outside witnesses on the magnitude are the
// LITERAL tables (75 % ⇒ 0.5, 62.5 % ⇒ 0.25, 50 % ⇒ 0.0), which are exact dyadic
// values and are checked in figures.
// ─────────────────────────────────────────────────────────────────────────────

#include "support/MidiRenderHarness.h"
#include "support/SequencerRenderRig.h"

#include "engine/graph/EngineCommand.h"
#include "engine/sequencer/PatternDocument.h"
#include "engine/sequencer/PatternSnapshot.h"
#include "engine/sequencer/PatternTypes.h"
#include "engine/sequencer/SequencerProcessor.h"
#include "engine/sequencer/StepLogic.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using arpbox::engine::buildPatternSnapshot;
using arpbox::engine::defaultSwingPct;
using arpbox::engine::DirectionMode;
using arpbox::engine::EngineCommandType;
using arpbox::engine::evaluateStep;
using arpbox::engine::LaneId;
using arpbox::engine::laneDefault;
using arpbox::engine::laneOf;
using arpbox::engine::laneRange;
using arpbox::engine::LaneState;
using arpbox::engine::maxSteps;
using arpbox::engine::maxSubStepShiftSteps;
using arpbox::engine::maxSwingPct;
using arpbox::engine::minSwingPct;
using arpbox::engine::numLanes;
using arpbox::engine::PatternDocument;
using arpbox::engine::PatternSetState;
using arpbox::engine::PatternSnapshot;
using arpbox::engine::PoolSnapshot;
using arpbox::engine::StepEmission;
using arpbox::engine::StepRuntime;
using arpbox::engine::swingShiftSteps;
using arpbox::testing::engineCommand;
using arpbox::testing::MidiRenderConfig;
using arpbox::testing::MidiRenderResult;
using arpbox::testing::renderSequencer;
using arpbox::testing::ScheduledCommand;
using arpbox::testing::SequencerRig;

namespace
{
// ─────────────────────────────────────────────────────────────────────────────
// A. The independently written half
// ─────────────────────────────────────────────────────────────────────────────

/** Whether global step `stepIndex` is an UPBEAT — written as `((n % 2) + 2) % 2 == 1`,
    DELIBERATELY a different expression from `stepFloorMod`'s `r < 0 ? r + b : r`, so
    a mistake in one cannot be mirrored by the other.

    This is the function the whole file is really about: `stepIndex % 2 == 1` looks
    identical and is wrong for every negative index. */
bool independentIsOddStep (std::int64_t stepIndex) noexcept
{
    return (((stepIndex % 2) + 2) % 2) == 1;
}

/** `swingShiftSteps`' contract: odd global steps are delayed by
    `(swingPct / 100 - 0.5) * 2` steps, even steps by EXACTLY 0.0.

    The magnitude expression is re-typed in the engine's own shape on purpose — see
    the header note on why an algebraically equivalent rearrangement would break the
    bitwise comparisons for no useful reason. The LITERAL tables further down are the
    outside witness on the magnitude; this function is the outside witness on the
    PAIRING. */
double expectedSwingShift (double swingPct, std::int64_t stepIndex) noexcept
{
    if (! independentIsOddStep (stepIndex))
        return 0.0;

    return (swingPct / 100.0 - 0.5) * 2.0;
}

/** The composed displacement §12.1 describes — MICRO (a percentage of the step) plus
    swing "on top", clamped ONCE on the SUM into ±`maxSubStepShiftSteps`. Written as
    an explicit two-armed clamp rather than through `juce::jlimit`, so the "one clamp,
    on the total" reading is spelled out here independently of the engine's. */
double expectedShiftSteps (double swingPct, int microPercent, std::int64_t stepIndex) noexcept
{
    const double composed = static_cast<double> (microPercent) / 100.0 + expectedSwingShift (swingPct, stepIndex);

    if (composed < -maxSubStepShiftSteps)
        return -maxSubStepShiftSteps;

    if (composed > maxSubStepShiftSteps)
        return maxSubStepShiftSteps;

    return composed;
}

// ─────────────────────────────────────────────────────────────────────────────
// B. Snapshot fixture (direct `evaluateStep` calls)
// ─────────────────────────────────────────────────────────────────────────────

/** Pattern 0 with every lane at its §12.1 default, GATE all-on across a 16-step cycle
    (so every swept index — negative ones included — actually emits a displacement
    rather than the empty emission an ungated step returns), and a one-note pool. */
PatternSetState microFixtureState ()
{
    PatternSetState state {};
    state.gridStepPpq = 0.25;

    PoolSnapshot pool {};
    pool.size = 1;
    pool.sorted[0] = 60;
    pool.asPlayed[0] = 60;
    state.pool = pool;

    auto& pattern = state.patterns[0];
    pattern.direction = DirectionMode::up;

    for (int lane = 0; lane < numLanes; ++lane)
    {
        LaneState& target = pattern.lanes[static_cast<std::size_t> (lane)];
        target.length = 16;
        target.division = 1;

        for (int step = 0; step < maxSteps; ++step)
            target.values[static_cast<std::size_t> (step)] = laneDefault (static_cast<LaneId> (lane));
    }

    LaneState& gate = laneOf (pattern, LaneId::gate);

    for (int step = 0; step < maxSteps; ++step)
        gate.values[static_cast<std::size_t> (step)] = 1;

    return state;
}

/** A MUTABLE copy of the built fixture snapshot, so the exhaustive MICRO x swing
    sweep can poke one value per iteration instead of paying for ~125 KB of snapshot
    build 800 times.

    LEGITIMATE HERE for the same reason `step_probability.cpp`'s `poked` copies are:
    every value written in is INSIDE its §12.1 range, so the builder's clamp has
    nothing to do, and the cases that exercise the DOCUMENT clamp go through
    `PatternDocument` + `buildPatternSnapshot` explicitly. */
std::unique_ptr<PatternSnapshot> mutableFixtureSnapshot ()
{
    const auto built = buildPatternSnapshot (microFixtureState (), 1);

    if (built == nullptr)
        return nullptr;

    return std::make_unique<PatternSnapshot> (*built);
}

/** Pins pattern 0's MICRO lane to `microPercent` at every step (length 1, so the
    value holds for every global index including the negative ones). */
void pokeMicro (PatternSnapshot& snapshot, int microPercent)
{
    LaneState& micro = snapshot.patterns[0].lanes[static_cast<std::size_t> (LaneId::micro)];
    micro.length = 1;
    micro.division = 1;
    micro.values[0] = static_cast<std::int16_t> (microPercent);
}

/** The composed displacement `evaluateStep` reports for global step `stepIndex`. */
double shiftStepsAt (const PatternSnapshot& snapshot, std::int64_t stepIndex)
{
    constexpr StepRuntime runtime {};
    const StepEmission emission = evaluateStep (snapshot, 0, stepIndex, runtime);

    return emission.shiftSteps;
}

// ─────────────────────────────────────────────────────────────────────────────
// C. Render fixture (the "swing 50 changes nothing" black-box half)
// ─────────────────────────────────────────────────────────────────────────────

// 120 BPM @ 48 kHz on the default 1/16 grid ⇒ one step is exactly 6000 samples, the
// same clock step_probability.cpp uses, so a finding here is directly comparable
// against that file with no alignment argument.
constexpr double probeSampleRate = 48000.0;
constexpr double probeBpm = 120.0;
constexpr std::int64_t probeSamplesPerStep = 6000;
constexpr std::int64_t probeSpanSamples = 6000 * 16; // one full 16-step loop
constexpr int probeBlockSize = 128;

/** Renders the DEFAULT document (GATE all-on across 16 steps, stub pool, `up`
    traversal, RATCHET 1, MICRO 0) — optionally after explicitly writing the STRAIGHT
    swing value and the FLAT ratchet ramp, which must be indistinguishable from never
    having touched either. */
MidiRenderResult renderStraight (bool writeFeelControlsExplicitly)
{
    SequencerRig rig { probeSampleRate, probeBlockSize };

    if (writeFeelControlsExplicitly)
    {
        rig.patternDocument.beginTransaction ();
        rig.patternDocument.setSwing (minSwingPct);
        rig.patternDocument.setRatchetVelocityRamp (0.0);
        rig.patternDocument.endTransaction ();
    }

    const std::vector<ScheduledCommand> schedule {
        ScheduledCommand { 0, engineCommand (EngineCommandType::setTempoBpm, probeBpm) },
        ScheduledCommand { 0, engineCommand (EngineCommandType::transportPlay) }
    };

    auto config = MidiRenderConfig::samples (probeSpanSamples, probeSampleRate, probeBlockSize);
    config.numChannels = 1;
    config.eventReserve = 4096;

    return renderSequencer (rig, config, schedule);
}

/** The swing values swept everywhere in this file. 50 is the straight default, 62.5
    and 75 are exact dyadic values (so their displacements — 0.25 and 0.5 — can be
    pinned as literals), and 55 / 66 are deliberately NON-dyadic so the sweep also
    covers values whose displacement has no tidy decimal form. */
constexpr double sweptSwingPcts[] = { 50.0, 55.0, 62.5, 66.0, 75.0 };

/** Index range for the pairing sweep: EXHAUSTIVE across zero, 128 either side, so
    both halves of the timeline are covered and the odd/even counts are exactly
    balanced (128 each). */
constexpr std::int64_t firstIndex = -128;
constexpr std::int64_t lastIndex = 127;
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1. THE PAIRING IS A FLOOR-MODULUS, AND NEGATIVE INDICES ARE THE POINT
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/micro-swing: swing pairs on the FLOOR-modulus of the global step index", "[unit]")
{
    // ── THE C++ FACT THIS WHOLE CASE EXISTS FOR ──────────────────────────────
    // `%` truncates toward zero, so its residue carries the sign of the dividend.
    // State it in figures before relying on it: a `stepIndex % 2 == 1` pairing test
    // reads step -1 as NOT odd, and step -1 is then paired as a DOWNBEAT — the whole
    // negative timeline shuffled half a step out of phase with the positive one.
    REQUIRE (-1 % 2 == -1);
    REQUIRE (-2 % 2 == 0);
    REQUIRE (! ((-1 % 2) == 1));
    REQUIRE (independentIsOddStep (-1));
    REQUIRE (! independentIsOddStep (-2));

    // The two assertions the header's failure class 1 comes down to, in figures, at
    // the maximum swing where the displacement is exactly half a step.
    REQUIRE (swingShiftSteps (75.0, -1) == 0.5);
    REQUIRE (swingShiftSteps (75.0, -2) == 0.0);
    REQUIRE (swingShiftSteps (75.0, -3) == 0.5);
    REQUIRE (swingShiftSteps (75.0, 1) == 0.5);
    REQUIRE (swingShiftSteps (75.0, 2) == 0.0);
    REQUIRE (swingShiftSteps (75.0, 0) == 0.0);

    // …and the exact dyadic middle, so the magnitude is pinned at more than one point.
    REQUIRE (swingShiftSteps (62.5, 1) == 0.25);
    REQUIRE (swingShiftSteps (62.5, -1) == 0.25);
    REQUIRE (swingShiftSteps (62.5, 2) == 0.0);

    // THE EXHAUSTIVE SWEEP. Bitwise equality against the independently written
    // pairing, over both halves of the timeline.
    int checks = 0;
    int mismatches = 0;
    int nonZeroShifts = 0;
    int oddIndices = 0;
    std::string firstMismatch;

    for (const double swingPct : sweptSwingPcts)
        for (std::int64_t index = firstIndex; index <= lastIndex; ++index)
        {
            const double actual = swingShiftSteps (swingPct, index);
            const double expected = expectedSwingShift (swingPct, index);
            ++checks;

            if (actual != 0.0)
                ++nonZeroShifts;

            if (actual != expected)
            {
                ++mismatches;

                if (firstMismatch.empty ())
                    firstMismatch = "swing " + std::to_string (swingPct) + " index " + std::to_string (index) +
                                    " expected " + std::to_string (expected) + " got " + std::to_string (actual);
            }
        }

    for (std::int64_t index = firstIndex; index <= lastIndex; ++index)
        if (independentIsOddStep (index))
            ++oddIndices;

    INFO ("checks " << checks << ", non-zero " << nonZeroShifts << ", odd indices " << oddIndices
                    << ", first mismatch: " << firstMismatch);
    REQUIRE (checks == 5 * static_cast<int> (lastIndex - firstIndex + 1));
    REQUIRE (mismatches == 0);

    // ANTI-VACUITY, in two parts. The sweep spans 256 indices of which exactly 128
    // are odd; four of the five swing values displace those and 50 % displaces none,
    // so a `swingShiftSteps` that returned 0.0 unconditionally — or that paired on the
    // wrong parity — cannot produce this count.
    REQUIRE (oddIndices == 128);
    REQUIRE (nonZeroShifts == 4 * 128);
}

TEST_CASE ("sequencer/micro-swing: the document clamps swing into 50..75 and it reaches the snapshot", "[unit]")
{
    // PROJECT-LEVEL, mirroring the grid: no pattern index, because a per-pattern
    // swing would let a quantized pattern switch change the FEEL mid-flight. CLAMPED
    // rather than rejected — unlike a grid, every finite swing value has a sane
    // nearest legal neighbour, and §5.3's macros/mod matrix will eventually drive this
    // continuously and must not fail on overshoot. (A non-finite value IS rejected;
    // that path is not exercised here because it fires a `jassertfalse`.)
    REQUIRE (minSwingPct == 50.0);
    REQUIRE (maxSwingPct == 75.0);
    REQUIRE (defaultSwingPct == 50.0);

    struct Row
    {
        double requested;
        double stored;
    };

    const Row rows[] = { { 50.0, 50.0 }, { 55.0, 55.0 },   { 62.5, 62.5 }, { 75.0, 75.0 },  { 49.9, 50.0 },
                         { 0.0, 50.0 },  { -100.0, 50.0 }, { 75.1, 75.0 }, { 100.0, 75.0 }, { 1.0e9, 75.0 } };

    int checks = 0;
    int wrong = 0;
    std::string firstWrong;

    for (const auto& row : rows)
    {
        PatternDocument document;
        REQUIRE (document.state ().swingPct == defaultSwingPct);

        document.setSwing (row.requested);
        const auto snapshot = buildPatternSnapshot (document.state (), 1);
        ++checks;

        if (snapshot == nullptr || snapshot->swingPct != row.stored)
        {
            ++wrong;

            if (firstWrong.empty ())
                firstWrong = "requested " + std::to_string (row.requested) + " expected " +
                             std::to_string (row.stored) + " got " +
                             (snapshot != nullptr ? std::to_string (snapshot->swingPct) : std::string ("<null>"));
        }
    }

    INFO ("checks " << checks << ", first wrong: " << firstWrong);
    REQUIRE (checks == 10);
    REQUIRE (wrong == 0);

    // The clamp runs BEFORE the no-op comparison, so 80 -> 75 twice is ONE edit and
    // not a second undo entry plus a second ~125 KB snapshot build.
    PatternDocument document;
    REQUIRE (! document.setSwing (50.0));
    REQUIRE (document.setSwing (62.5));
    REQUIRE (! document.setSwing (62.5));
    REQUIRE (document.setSwing (80.0));
    REQUIRE (document.state ().swingPct == 75.0);
    REQUIRE (! document.setSwing (90.0));
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. SWING 50 IS A BITWISE NO-OP
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/micro-swing: swingPct 50 displaces nothing, bitwise", "[unit]")
{
    // "APPROXIMATELY ZERO" WOULD NOT DO. The walk computes
    // `placedPpq = gridPpq + shiftSteps * stepPpq`; at bit-zero that expression is
    // BIT-IDENTICAL to `gridPpq`, so a straight pattern's placed PPQ is the same
    // double it was before Phase 7.2 existed and no pre-7.2 golden can move. At
    // 1e-18 instead of 0 the product is denormal-small but the SUM can still cross a
    // snap window at some tempo/rate combination, and the failure would surface as a
    // one-sample golden diff on some machines and not others.
    int checks = 0;
    int nonZero = 0;

    for (std::int64_t index = firstIndex - 4096; index <= lastIndex + 4096; ++index)
    {
        ++checks;

        if (swingShiftSteps (minSwingPct, index) != 0.0)
            ++nonZero;
    }

    INFO ("checks " << checks);
    REQUIRE (checks == static_cast<int> (lastIndex - firstIndex + 1) + 2 * 4096);
    REQUIRE (nonZero == 0);

    // …and the same statement one level up, through the pure emission core: a
    // MICRO-0 pattern at swing 50 reports a displacement of exactly 0.0, so
    // `StepEmission::shiftSteps` is bit-zero and not merely small.
    const auto snapshot = mutableFixtureSnapshot ();
    REQUIRE (snapshot != nullptr);
    REQUIRE (snapshot->swingPct == defaultSwingPct);
    REQUIRE (laneDefault (LaneId::micro) == 0);

    int emissions = 0;
    int displaced = 0;

    for (std::int64_t index = -64; index <= 64; ++index)
    {
        const StepEmission emission = evaluateStep (*snapshot, 0, index, StepRuntime {});
        ++emissions;

        if (! emission.gate || emission.shiftSteps != 0.0)
            ++displaced;
    }

    INFO ("emissions " << emissions);
    REQUIRE (emissions == 129);
    REQUIRE (displaced == 0);

    // THE BLACK-BOX HALF: explicitly writing the straight swing value and the flat
    // ratchet ramp is indistinguishable from never having touched either — which is
    // what makes the pre-7.2 goldens' continued green a statement about the DEFAULTS
    // rather than about two features happening to cancel.
    const auto untouched = renderStraight (false);
    const auto written = renderStraight (true);

    INFO (untouched.describeDifference (written));
    REQUIRE (untouched.events.size () == written.events.size ());
    REQUIRE (untouched.toByteStream () == written.toByteStream ());

    // ANTI-VACUITY: the render has to contain the whole loop, or "identical" is true
    // of two empty streams. 16 gated steps ⇒ 16 note-ons at 6000-sample spacing.
    int noteOns = 0;

    for (const auto& event : untouched.events)
        if (event.message.isNoteOn ())
            ++noteOns;

    INFO (untouched.describe (8));
    REQUIRE (noteOns == 16);
    REQUIRE (untouched.events.front ().absoluteSample == 0);

    // And the two writes are not even EDITS: the defaults already are straight and
    // flat, so both setters report "nothing changed".
    PatternDocument document;
    REQUIRE (! document.setSwing (minSwingPct));
    REQUIRE (! document.setRatchetVelocityRamp (0.0));
    REQUIRE (document.getUndoDepth () == 0);
    REQUIRE (probeSamplesPerStep == 6000);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. THE COMPOSITION, AND THE SINGLE CLAMP ON THE TOTAL
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/micro-swing: MICRO and swing compose additively and are clamped ONCE on the total", "[unit]")
{
    // §12.1 stores MICRO as a percentage of the step, -50..+50, and says "swing
    // applies on top" — so the composition is ADDITIVE with MICRO first, and the
    // clamp is applied to the SUM at exactly one place inside the pure core.
    //
    // EVERY ROW BELOW IS AN EXACT DYADIC VALUE, so it can be written in figures: MICRO
    // ±50/±25 are ±0.5/±0.25 exactly, swing 75 % is +0.5 exactly and swing 62.5 % is
    // +0.25 exactly. Index 1 and -1 are ODD (upbeats), 2 and -2 are EVEN.
    REQUIRE (maxSubStepShiftSteps == 0.5);
    REQUIRE (laneRange (LaneId::micro).lo == -50);
    REQUIRE (laneRange (LaneId::micro).hi == 50);

    struct Row
    {
        double swingPct;
        int microPercent;
        std::int64_t stepIndex;
        double expected;
    };

    const Row rows[] = {
        // swing 75 % on an ODD step: the swing term alone is already at the ceiling,
        // so every non-negative MICRO SATURATES to the same displacement. That is
        // failure class 4 — documented behaviour, not a rounding artefact.
        { 75.0, 50, 1, 0.5 },
        { 75.0, 25, 1, 0.5 },
        { 75.0, 0, 1, 0.5 },
        { 75.0, -25, 1, 0.25 },
        { 75.0, -50, 1, 0.0 },
        { 75.0, 50, -1, 0.5 },
        { 75.0, 0, -1, 0.5 },
        { 75.0, -50, -1, 0.0 },

        // swing 75 % on an EVEN step: no swing term at all, so MICRO is the whole
        // displacement and reaches both ends of its own range.
        { 75.0, 50, 2, 0.5 },
        { 75.0, 0, 2, 0.0 },
        { 75.0, -50, 2, -0.5 },
        { 75.0, -50, -2, -0.5 },

        // swing 62.5 % ⇒ +0.25 on odd steps: composes without saturating except at
        // MICRO +50, where 0.25 + 0.5 = 0.75 is clamped back to 0.5.
        { 62.5, 50, 1, 0.5 },
        { 62.5, 25, 1, 0.5 },
        { 62.5, 0, 1, 0.25 },
        { 62.5, -25, 1, 0.0 },
        { 62.5, -50, 1, -0.25 },
        { 62.5, -50, 2, -0.5 },

        // swing 50 % (straight): MICRO is the entire displacement on every step,
        // odd and even alike.
        { 50.0, 50, 1, 0.5 },
        { 50.0, 0, 1, 0.0 },
        { 50.0, -50, 1, -0.5 },
        { 50.0, 25, 2, 0.25 },
        { 50.0, -25, -3, -0.25 },
    };

    const auto snapshot = mutableFixtureSnapshot ();
    REQUIRE (snapshot != nullptr);

    int checks = 0;
    int wrong = 0;
    std::string firstWrong;

    for (const auto& row : rows)
    {
        snapshot->swingPct = row.swingPct;
        pokeMicro (*snapshot, row.microPercent);
        const double actual = shiftStepsAt (*snapshot, row.stepIndex);
        ++checks;

        if (actual != row.expected)
        {
            ++wrong;

            if (firstWrong.empty ())
                firstWrong = "swing " + std::to_string (row.swingPct) + " micro " + std::to_string (row.microPercent) +
                             " index " + std::to_string (row.stepIndex) + " expected " + std::to_string (row.expected) +
                             " got " + std::to_string (actual);
        }
    }

    INFO ("checks " << checks << ", first wrong: " << firstWrong);
    REQUIRE (checks == 23);
    REQUIRE (wrong == 0);

    // THE SATURATION, STATED AS AN EQUALITY. At swing 75 an odd step with MICRO +50
    // and an odd step with MICRO 0 land on the SAME displacement — so they land on the
    // same sample. Written out because the natural reading of a difference of 0 here
    // is "MICRO is broken", and the fix that reading suggests (clamp per source) is
    // failure class 3.
    snapshot->swingPct = 75.0;
    pokeMicro (*snapshot, 50);
    const double saturated = shiftStepsAt (*snapshot, 3);
    pokeMicro (*snapshot, 0);
    const double unmoved = shiftStepsAt (*snapshot, 3);

    REQUIRE (saturated == unmoved);
    REQUIRE (saturated == maxSubStepShiftSteps);

    // …and the same pair on an EVEN step is NOT equal, which is what says the
    // equality above is saturation rather than MICRO being ignored outright.
    pokeMicro (*snapshot, 50);
    const double evenWithMicro = shiftStepsAt (*snapshot, 4);
    pokeMicro (*snapshot, 0);
    const double evenWithout = shiftStepsAt (*snapshot, 4);

    REQUIRE (evenWithMicro != evenWithout);
    REQUIRE (evenWithMicro == 0.5);
    REQUIRE (evenWithout == 0.0);
}

TEST_CASE ("sequencer/micro-swing: the composed displacement never leaves ±0.5 and ATTAINS both bounds", "[unit]")
{
    // ── THE FAILS-WITHOUT IS CLAMPING PER SOURCE ─────────────────────────────
    // MICRO alone reaches ±0.5 (§12.1: -50..+50 % of a step) and swing alone reaches
    // +0.5 (`swingPct` 75), so TWO per-source clamps would admit a composed ±1.0 and
    // still look correct at every individual source. A composed displacement past 0.5
    // breaks the derivation `stepScanBack` / `stepScanForward` rest on — both bounds
    // there are ATTAINED, not padded — so the walk would never visit the block a
    // displaced event landed in and the event would simply vanish, at every buffer
    // size equally. No cross-carving determinism comparison can see that, which is
    // why this bound is checked directly and exhaustively over the lane's range.
    //
    // AND THE BOUND MUST BE ATTAINED, or it is a bound nothing checks: a clamp
    // implemented as `jlimit (-0.4, 0.4, …)` would satisfy "never leaves ±0.5"
    // perfectly while silently narrowing the feature.
    REQUIRE (maxSubStepShiftSteps == 0.5);

    const double swingGrid[] = { 50.0, 51.0, 55.0, 60.0, 62.5, 66.0, 70.0, 75.0 };

    const auto snapshot = mutableFixtureSnapshot ();
    REQUIRE (snapshot != nullptr);

    int checks = 0;
    int outOfRange = 0;
    int mismatches = 0;
    int atUpperBound = 0;
    int atLowerBound = 0;
    double maxSeen = 0.0;
    double minSeen = 0.0;
    std::string firstBad;

    for (const double swingPct : swingGrid)
    {
        snapshot->swingPct = swingPct;

        // EXHAUSTIVE over §12.1's MICRO range, every value, not a sample of it.
        for (int microPercent = laneRange (LaneId::micro).lo; microPercent <= laneRange (LaneId::micro).hi;
             ++microPercent)
        {
            pokeMicro (*snapshot, microPercent);

            // One ODD and one EVEN index, plus their negative counterparts, so the
            // swing term is exercised present and absent on both halves of the
            // timeline.
            for (const std::int64_t index :
                 { std::int64_t { 1 }, std::int64_t { 2 }, std::int64_t { -1 }, std::int64_t { -2 } })
            {
                const double actual = shiftStepsAt (*snapshot, index);
                const double expected = expectedShiftSteps (swingPct, microPercent, index);
                ++checks;

                if (actual > maxSeen)
                    maxSeen = actual;

                if (actual < minSeen)
                    minSeen = actual;

                if (actual == maxSubStepShiftSteps)
                    ++atUpperBound;

                if (actual == -maxSubStepShiftSteps)
                    ++atLowerBound;

                if (actual > maxSubStepShiftSteps || actual < -maxSubStepShiftSteps)
                {
                    ++outOfRange;

                    if (firstBad.empty ())
                        firstBad = "OUT OF RANGE swing " + std::to_string (swingPct) + " micro " +
                                   std::to_string (microPercent) + " index " + std::to_string (index) + " got " +
                                   std::to_string (actual);
                }

                if (actual != expected)
                {
                    ++mismatches;

                    if (firstBad.empty ())
                        firstBad = "swing " + std::to_string (swingPct) + " micro " + std::to_string (microPercent) +
                                   " index " + std::to_string (index) + " expected " + std::to_string (expected) +
                                   " got " + std::to_string (actual);
                }
            }
        }
    }

    INFO ("checks " << checks << ", max " << maxSeen << ", min " << minSeen << ", at +0.5 " << atUpperBound
                    << ", at -0.5 " << atLowerBound << ", first bad: " << firstBad);

    // 8 swing values x 101 MICRO values x 4 indices.
    REQUIRE (checks == 8 * 101 * 4);
    REQUIRE (outOfRange == 0);
    REQUIRE (mismatches == 0);

    // BOTH BOUNDS ARE REACHED, EXACTLY. +0.5 from MICRO +50 (on every index) and from
    // swing 75 alone on the odd ones; -0.5 from MICRO -50 on the even ones.
    REQUIRE (maxSeen == maxSubStepShiftSteps);
    REQUIRE (minSeen == -maxSubStepShiftSteps);
    REQUIRE (atUpperBound > 0);
    REQUIRE (atLowerBound > 0);
}
