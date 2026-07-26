// ─────────────────────────────────────────────────────────────────────────────
// step_ratchet — §5.1 L2's RATCHETS (ARCHITECTURE §5.1 L2 "Ratchets (1–8) with
// per-ratchet velocity ramp and ratchet probability", §12.1 RATCHET;
// engine/sequencer/StepLogic.{h,cpp} `ratchetChildCount` / `ratchetChildPasses` /
// `ratchetVelocity`, and the child-placement block of `evaluateStep` +
// `emitNote` in engine/sequencer/SequencerProcessor.cpp).
//
// ── THE FAILURE CLASS THIS FILE CLOSES: DRIFTING SUB-STEP ARITHMETIC ────────
// A ratchet turns ONE step into up to eight notes at sub-step positions, and every
// one of those positions, lengths and velocities is derived arithmetic that nothing
// in the suite previously constrained. Four specific mistakes are all invisible to
// the existing goldens (every one of them was baked at RATCHET 1, where the child
// list has one entry and the arithmetic collapses to Phase 6's):
//
//   1. CUMULATIVE ONSETS. Writing child c's position as
//      `previous + 1 / noteCount` instead of `c / noteCount` is the natural loop
//      shape and is WRONG for every non-dyadic child count: 1/7 is not
//      representable, so three additions of the slot do not equal 3/7 bitwise, and
//      at the canonical clock child 6 of 7 ends up ~6 samples early. Nothing but an
//      EXACT `==` against `c / n` can see the difference, which is why the onset
//      case below compares with `==` and not with a tolerance.
//   2. LEN MEASURED AGAINST THE WHOLE STEP instead of the child's own slot. Also
//      invisible at RATCHET 1 (the slot IS the step). At RATCHET 8 it makes every
//      child except the last immediately cut short by its successor, so LEN has no
//      audible effect below 800 % — see the LEN case for the exact fails-without
//      shape (all the gaps collapse to one sample).
//   3. A RUNNING STREAM behind the per-child probability roll instead of
//      `rng::subStepHash`'s pure hash of the (step, child) PAIR. The retrigger
//      lookahead asks about children of steps it is not emitting, a variable number
//      of times, so a stream's pull count would make prediction and emission
//      disagree about which children exist — #36/#46/#48's buffer-size-dependent
//      class one level up.
//   4. A LOST SHORT-CIRCUIT. At the PROB default of 100 `ratchetChildPasses`
//      evaluates NO hash at all, which is what makes a RATCHET-8 pattern on a fresh
//      document provably insensitive to anything Phase 12 does to the seed
//      composition. `hash % 100 < 100` is always true, so deleting the branch
//      changes nothing TODAY and moves every default-PROB golden later.
//
// ── THE REFERENCE IMPLEMENTATIONS ARE WRITTEN OUT LONGHAND, ON PURPOSE ──────
// `referenceSubStepHash` re-types splitmix64's body, the 0x5243 ratchet salt,
// `stepHash`'s nesting and `subStepHash`'s outer round. It never calls `rng::` —
// engine/generative/Rng.h is deliberately NOT included by this file. A test that
// computes its expectation by calling the function under test asserts only
// self-consistency; a mistyped multiplier, a lost XOR or a swapped salt leaves a
// function that is perfectly self-consistent and audibly different. The reference
// is itself checked against splitmix64's published known-answer vector BEFORE it is
// trusted as an oracle, so a typo here cannot silently excuse a typo there.
// `independentFloorMod` is likewise written as `((a % b) + b) % b`, a different
// expression from `stepFloorMod`'s `r < 0 ? r + b : r`.
//
// ── THE n = 7 SAMPLE ROW: A KNOWN DISCREPANCY WITH THE PHASE-7 PLAN PROSE ───
// READ THIS BEFORE "FIXING" THE LITERALS. The Phase-7.2/7.3 plan text lists child
// onsets for RATCHET 7 at the canonical clock (125 BPM / 48 kHz / gridPpq 0.25,
// i.e. 5760 samples per step) as
//
//     0, 823, 1646, 2469, 3291, 4114, 4937      <- ROUND-to-nearest
//
// The landed engine produces
//
//     0, 822, 1645, 2468, 3291, 4114, 4937      <- SNAP-then-FLOOR
//
// because `SequencerProcessor::sampleForPpq` is
// `floor (rawOffset + sampleOffsetSnapSamples)` — ONE snap-then-floor, never a
// round-to-nearest (the header calls it "THE ONLY SNAP-THEN-FLOOR IN THIS FILE").
// The plan's list assumed rounding; the three children whose fractional part
// exceeds ½ (c = 1, 2, 3) therefore differ by one sample. THE ENGINE'S BEHAVIOUR IS
// WHAT IS PINNED HERE, deliberately: the snap-then-floor convention is shared with
// `ownsPpq`'s snapped ceiling and with the retrigger lookahead's prediction (issue
// #54), so "rounding would read nicer" is not a reason to move it — a one-sided
// change to either derivation resurrects #36/#46/#48. The literals are cross-checked
// against INTEGER floor division `(c * 5760) / 7` in the test itself, so if this
// arithmetic is wrong the test says so rather than quietly agreeing with the engine.
// ─────────────────────────────────────────────────────────────────────────────

#include "support/MidiRenderHarness.h"
#include "support/SequencerRenderRig.h"

#include "engine/graph/EngineCommand.h"
#include "engine/sequencer/PatternDocument.h"
#include "engine/sequencer/PatternSnapshot.h"
#include "engine/sequencer/PatternTypes.h"
#include "engine/sequencer/Provenance.h"
#include "engine/sequencer/SequencerProcessor.h"
#include "engine/sequencer/StepLogic.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using arpbox::engine::buildPatternSnapshot;
using arpbox::engine::DirectionMode;
using arpbox::engine::EngineCommandType;
using arpbox::engine::evaluateStep;
using arpbox::engine::LaneId;
using arpbox::engine::laneDefault;
using arpbox::engine::laneOf;
using arpbox::engine::laneRange;
using arpbox::engine::LaneState;
using arpbox::engine::maxRatchetChildren;
using arpbox::engine::maxRatchetVelocityRampPct;
using arpbox::engine::maxSteps;
using arpbox::engine::minRatchetVelocityRampPct;
using arpbox::engine::numLanes;
using arpbox::engine::PatternData;
using arpbox::engine::PatternDocument;
using arpbox::engine::PatternSetState;
using arpbox::engine::PatternSnapshot;
using arpbox::engine::PoolSnapshot;
using arpbox::engine::ratchetChildCount;
using arpbox::engine::ratchetChildPasses;
using arpbox::engine::ratchetVelocity;
using arpbox::engine::StepEmission;
using arpbox::engine::StepRuntime;
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

/** Floor-modulus, written as `((a % b) + b) % b` — DELIBERATELY a different
    expression from `stepFloorMod`'s `r < 0 ? r + b : r`, so a mistake in one cannot
    be mirrored by the other. Precondition: `b > 0`. */
std::int64_t independentFloorMod (std::int64_t a, std::int64_t b) noexcept
{
    return ((a % b) + b) % b;
}

/** splitmix64, re-typed from the canonical constants (Steele/Lea/Flood) rather than
    included from `engine/generative/Rng.h`. THE POINT IS THE DUPLICATION — see the
    header note. Checked against its known-answer vector before it is used. */
std::uint64_t referenceSplitmix64 (std::uint64_t x) noexcept
{
    x += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/** `RngDomain::ratchetProbability`, as a literal. 0x5243 is ASCII 'R','C' (RatChet).
    The salt is XORed into the seed, so a changed value silently rewrites every
    thinned ratchet ever produced — writing it out here is what makes a change to the
    append-only registry visible as a test diff rather than as a golden diff. */
constexpr std::uint64_t referenceRatchetSalt = 0x5243ULL;

/** `rng::stepHash`, re-derived: `splitmix64 (splitmix64 (seed ^ salt) ^ index)`. Two
    nested calls, not one — the inner is §5.2's effective stream seed with the
    Phase-12 operator/loop-lock terms elided (they XOR with 0 today), the outer is
    §12.3's per-index-hash idiom. */
std::uint64_t referenceStepHash (std::uint64_t masterSeed, std::int64_t stepIndex) noexcept
{
    return referenceSplitmix64 (referenceSplitmix64 (masterSeed ^ referenceRatchetSalt) ^
                                static_cast<std::uint64_t> (stepIndex));
}

/** `rng::subStepHash`, re-derived: ONE further splitmix64 round over
    `stepHash (...) ^ subIndex`. Not `stepHash (seed, domain, step * 8 + sub)` — the
    header on `subStepHash` argues at length why folding the sub-index into the step
    index would hard-code §12.1's RATCHET ceiling into the seed composition. */
std::uint64_t referenceSubStepHash (std::uint64_t masterSeed, std::int64_t stepIndex, int subIndex) noexcept
{
    return referenceSplitmix64 (referenceStepHash (masterSeed, stepIndex) ^ static_cast<std::uint64_t> (subIndex));
}

/** The whole of `ratchetChildPasses`' contract, written from §5.1 L2 / §12.1: child
    0 is never re-rolled, `p >= 100` passes WITHOUT hashing, `p <= 0` fails without
    hashing, otherwise `hash % 100 < p`. */
bool expectedChildPasses (std::uint64_t masterSeed, int percent, std::int64_t stepIndex, int childIndex) noexcept
{
    if (childIndex <= 0)
        return true;

    if (percent >= 100)
        return true;

    if (percent <= 0)
        return false;

    return referenceSubStepHash (masterSeed, stepIndex, childIndex) % 100ULL < static_cast<std::uint64_t> (percent);
}

/** `ratchetVelocity`'s contract, written from §5.1 L2 and the range note in
    PatternTypes.h: base UNCHANGED when there is nothing to ramp, otherwise linear in
    `min (childIndex, childCount - 1) / (childCount - 1)` from the base at child 0 to
    `base * (1 + ramp / 100)` at the last child, rounded half-away-from-zero and
    clamped into MIDI's 1..127. Written with `std::llround` because that is what the
    contract says (the .cpp names it); the LITERAL tables below are the outside
    witness on the arithmetic. */
int expectedRatchetVelocity (int baseVelocity, int childIndex, int childCount, double rampPct) noexcept
{
    if (childCount <= 1 || childIndex <= 0 || rampPct == 0.0)
        return baseVelocity;

    const int capped = childIndex < childCount - 1 ? childIndex : childCount - 1;
    const double t = static_cast<double> (capped) / static_cast<double> (childCount - 1);
    const double scaled = static_cast<double> (baseVelocity) * (1.0 + rampPct / 100.0 * t);
    const auto rounded = static_cast<int> (std::llround (scaled));

    return rounded < 1 ? 1 : (rounded > 127 ? 127 : rounded);
}

// ─────────────────────────────────────────────────────────────────────────────
// B. Snapshot fixtures (direct `ratchetChildCount` / `evaluateStep` calls)
// ─────────────────────────────────────────────────────────────────────────────

constexpr std::uint64_t seedA = 0x0123456789ABCDEFULL;
constexpr std::uint64_t seedB = 0xFEDCBA9876543210ULL;
constexpr std::uint64_t seedC = 0xA5A5A5A5A5A5A5A5ULL;

/** Pattern 0 with every lane at its §12.1 default, GATE all-on across a 16-step
    cycle (so every swept index — negative ones included — actually emits), and a
    ONE-NOTE pool.

    THE POOL IS ONE NOTE DELIBERATELY: every emitted note then shares a pitch, so
    the same-pitch retrigger chain §5.5 describes runs unbroken through a step's
    children AND across the step boundary. That is exactly the geometry the LEN-400 %
    case needs, and it makes the render-level literals readable (every note-on is
    pitch 60). */
PatternSetState ratchetFixtureState (std::uint64_t masterSeed)
{
    PatternSetState state {};
    state.gridStepPpq = 0.25;

    PoolSnapshot pool {};
    pool.size = 1;
    pool.sorted[0] = 60;
    pool.asPlayed[0] = 60;
    state.pool = pool;

    auto& pattern = state.patterns[0];
    pattern.masterSeed = masterSeed;
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

/** The fixture with the RATCHET lane pinned to `childCount` at every step (length 1,
    so the value holds for every global index including the negative ones) and the LEN
    lane pinned to `lenPercent`. */
PatternSetState fixtureWithRatchet (int childCount, int lenPercent)
{
    PatternSetState state = ratchetFixtureState (seedA);

    LaneState& ratchet = laneOf (state.patterns[0], LaneId::ratchet);
    ratchet.length = 1;
    ratchet.values[0] = static_cast<std::int16_t> (childCount);

    LaneState& len = laneOf (state.patterns[0], LaneId::len);
    len.length = 1;
    len.values[0] = static_cast<std::int16_t> (lenPercent);

    return state;
}

/** A MUTABLE copy of a built snapshot, so a sweep can poke one field per iteration
    without paying for a fresh ~125 KB build each time.

    Poking a copy is the same idiom `step_probability.cpp` uses on `PatternData`, and
    it is legitimate here for the same reason: the values poked in are all INSIDE
    their §12.1 ranges (the builder's clamp has nothing to do), and the cases that DO
    exercise the clamp build through `buildPatternSnapshot` explicitly. */
std::unique_ptr<PatternSnapshot> mutableSnapshotOf (const PatternSetState& state)
{
    const auto built = buildPatternSnapshot (state, 1);

    if (built == nullptr)
        return nullptr;

    return std::make_unique<PatternSnapshot> (*built);
}

/** Writes `value` into pattern 0's `lane` slot 0 and pins the lane's length to 1, on
    a MUTABLE snapshot (`laneOf (const PatternData&, …)` is const-only). */
void pokeLane (PatternSnapshot& snapshot, LaneId lane, int value)
{
    LaneState& target = snapshot.patterns[0].lanes[static_cast<std::size_t> (lane)];
    target.length = 1;
    target.division = 1;
    target.values[0] = static_cast<std::int16_t> (value);
}

// ─────────────────────────────────────────────────────────────────────────────
// C. Render fixtures (the black-box halves)
// ─────────────────────────────────────────────────────────────────────────────

// THE CANONICAL CLOCK for every render in this file: 125 BPM @ 48 kHz on the default
// 1/16 grid ⇒ one step is EXACTLY 5760 samples (0.25 * (60/125) * 48000). Chosen
// because 5760 = 2^7 * 45 is divisible by every legal ratchet count except 7, so the
// dyadic rows land on whole samples a reader can check by hand and the n = 7 row is
// the one place the snap-then-floor convention is observable.
constexpr double clockSampleRate = 48000.0;
constexpr double clockBpm = 125.0;
constexpr std::int64_t samplesPerStep = 5760;

/** Sets pattern 0's RATCHET lane to `childCount` and its LEN lane to `lenPercent` on
    every step, sets the PROB lane to `probPercent`, replaces the pool with the
    single note 60, and sets the master seed. Everything else stays the default
    document (GATE all-on across 16 steps, `up` traversal, VEL 100). */
void configure (PatternDocument& document,
                std::uint64_t masterSeed,
                int childCount,
                int lenPercent,
                int probPercent,
                double rampPct)
{
    PoolSnapshot pool {};
    pool.size = 1;
    pool.sorted[0] = 60;
    pool.asPlayed[0] = 60;

    document.beginTransaction ();
    document.setMasterSeed (0, masterSeed);
    document.setPool (pool);
    document.setRatchetVelocityRamp (rampPct);

    for (int step = 0; step < 16; ++step)
    {
        document.setLaneValue (0, LaneId::ratchet, step, childCount);
        document.setLaneValue (0, LaneId::len, step, lenPercent);
        document.setLaneValue (0, LaneId::prob, step, probPercent);
    }

    document.endTransaction ();
}

MidiRenderConfig clockConfig (std::int64_t spanSamples, int blockSize)
{
    auto config = MidiRenderConfig::samples (spanSamples, clockSampleRate, blockSize);
    config.numChannels = 1;
    config.eventReserve = 16384;
    config.midiReserveBytes = 32768;
    return config;
}

/** Renders `spanSamples` of the configured pattern from transport position 0. */
MidiRenderResult renderRatchet (std::uint64_t masterSeed,
                                int childCount,
                                int lenPercent,
                                int probPercent,
                                double rampPct,
                                std::int64_t spanSamples,
                                int blockSize)
{
    SequencerRig rig { clockSampleRate, blockSize };
    configure (rig.patternDocument, masterSeed, childCount, lenPercent, probPercent, rampPct);

    const std::vector<ScheduledCommand> schedule {
        ScheduledCommand { 0, engineCommand (EngineCommandType::setTempoBpm, clockBpm) },
        ScheduledCommand { 0, engineCommand (EngineCommandType::transportPlay) }
    };

    return renderSequencer (rig, clockConfig (spanSamples, blockSize), schedule);
}

std::vector<std::int64_t> noteOnSamples (const MidiRenderResult& render)
{
    std::vector<std::int64_t> samples;

    for (const auto& event : render.events)
        if (event.message.isNoteOn ())
            samples.push_back (event.absoluteSample);

    return samples;
}

std::vector<std::int64_t> noteOffSamples (const MidiRenderResult& render)
{
    std::vector<std::int64_t> samples;

    for (const auto& event : render.events)
        if (event.message.isNoteOff ())
            samples.push_back (event.absoluteSample);

    return samples;
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1. THE CHILD COUNT COMES FROM THE LANE, THROUGH THE §12.1 CLAMP
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/ratchet: the child count is the RATCHET lane read through the 1..8 clamp", "[unit]")
{
    // §12.1's RATCHET range, in figures. `maxRatchetChildren` is DERIVED from it and
    // the whole sub-step geometry (`maxChildAheadSteps`,
    // `maxRetriggerLookaheadSteps`, `StepEmission::notes`'s size) descends from that,
    // so a change here is a change to five other constants — say the two numbers out
    // loud rather than only through the derivation.
    REQUIRE (laneRange (LaneId::ratchet).lo == 1);
    REQUIRE (laneRange (LaneId::ratchet).hi == 8);
    REQUIRE (maxRatchetChildren == 8);
    REQUIRE (laneDefault (LaneId::ratchet) == 1);

    // A RATCHET lane of LENGTH 8 holding 1, 2, … 8, so the count at global step g is
    // `1 + floorMod (g, 8)` — via the independent floor-mod expression, and swept
    // through NEGATIVE indices because the retrigger lookahead's backward scan and
    // the locate paths evaluate them (C++ `%` alone would read step -1 as slot -1).
    PatternSetState state = ratchetFixtureState (seedA);
    LaneState& ratchet = laneOf (state.patterns[0], LaneId::ratchet);
    ratchet.length = 8;
    ratchet.division = 1;

    for (int step = 0; step < 8; ++step)
        ratchet.values[static_cast<std::size_t> (step)] = static_cast<std::int16_t> (step + 1);

    const auto snapshot = buildPatternSnapshot (state, 1);
    REQUIRE (snapshot != nullptr);
    const PatternData& data = snapshot->patterns[0];

    constexpr std::int64_t firstIndex = -256;
    constexpr std::int64_t lastIndex = 512;

    int checks = 0;
    int mismatches = 0;
    int distinctSeen = 0;
    bool seen[9] = {};
    std::string firstMismatch;

    for (std::int64_t index = firstIndex; index <= lastIndex; ++index)
    {
        const int expected = 1 + static_cast<int> (independentFloorMod (index, 8));
        const int actual = ratchetChildCount (data, index);
        ++checks;

        if (actual != expected)
        {
            ++mismatches;

            if (firstMismatch.empty ())
                firstMismatch = "index " + std::to_string (index) + " expected " + std::to_string (expected) + " got " +
                                std::to_string (actual);
        }

        if (actual >= 1 && actual <= 8 && ! seen[actual])
        {
            seen[actual] = true;
            ++distinctSeen;
        }
    }

    INFO ("checks " << checks << ", distinct counts " << distinctSeen << ", first mismatch: " << firstMismatch);
    REQUIRE (checks == static_cast<int> (lastIndex - firstIndex + 1));
    REQUIRE (mismatches == 0);
    // ANTI-VACUITY: a `ratchetChildCount` that always returned 1 would have to be
    // matched by a reference that always returned 1 for the sweep to pass. State the
    // shape directly instead — all eight counts are actually produced.
    REQUIRE (distinctSeen == 8);
}

TEST_CASE ("sequencer/ratchet: an out-of-range RATCHET value is clamped, not trusted", "[unit]")
{
    // TWO clamps, and both are load-bearing for different reasons.
    //
    // THE BUILDER'S: `buildPatternSnapshot` runs every lane value through
    // `clampLaneValue`, so nothing out of range reaches the RT path on the normal
    // route. Checked first, so the second half is provably testing the OTHER clamp.
    {
        PatternSetState state = ratchetFixtureState (seedA);
        LaneState& ratchet = laneOf (state.patterns[0], LaneId::ratchet);
        ratchet.length = 4;
        ratchet.values[0] = 0;
        ratchet.values[1] = -7;
        ratchet.values[2] = 9;
        ratchet.values[3] = 32767;

        const auto snapshot = buildPatternSnapshot (state, 1);
        REQUIRE (snapshot != nullptr);
        const LaneState& built = laneOf (snapshot->patterns[0], LaneId::ratchet);

        REQUIRE (built.values[0] == 1);
        REQUIRE (built.values[1] == 1);
        REQUIRE (built.values[2] == 8);
        REQUIRE (built.values[3] == 8);
    }

    // `ratchetChildCount`'S OWN: the RT path clamps AGAIN even though the builder
    // already did, because that number sizes a loop writing into a fixed-size array
    // on the audio thread from data that arrived through a pointer swap. This is the
    // only place that second clamp is reachable — poke past the builder, the same
    // shape a corrupt project blob or a future operator write would take.
    const auto snapshot = mutableSnapshotOf (ratchetFixtureState (seedA));
    REQUIRE (snapshot != nullptr);

    int checks = 0;
    int outOfRange = 0;
    std::string firstBad;

    for (const int poked : { -32768, -100, -1, 0, 9, 64, 400, 32767 })
    {
        pokeLane (*snapshot, LaneId::ratchet, poked);
        const int expected = poked < 1 ? 1 : 8;

        for (std::int64_t index = -40; index <= 40; ++index)
        {
            const int actual = ratchetChildCount (snapshot->patterns[0], index);
            ++checks;

            if (actual != expected)
            {
                ++outOfRange;

                if (firstBad.empty ())
                    firstBad = "poked " + std::to_string (poked) + " index " + std::to_string (index) + " expected " +
                               std::to_string (expected) + " got " + std::to_string (actual);
            }
        }
    }

    INFO ("checks " << checks << ", first bad: " << firstBad);
    REQUIRE (checks == 8 * 81);
    REQUIRE (outOfRange == 0);
}

TEST_CASE ("sequencer/ratchet: evaluateStep emits exactly childCount notes at the PROB default", "[unit]")
{
    // The lane read and the note LIST agree. At PROB 100 nothing is thinned, so
    // `noteCount` is the RATCHET lane value exactly — which is also the statement
    // that makes the sub-step geometry's `maxRatchetChildren`-sized array the right
    // size rather than merely a large enough one.
    constexpr StepRuntime runtime {};

    int checks = 0;
    int mismatches = 0;
    int childrenSeen = 0;
    std::string firstMismatch;

    for (int childCount = 1; childCount <= maxRatchetChildren; ++childCount)
    {
        const auto snapshot = buildPatternSnapshot (fixtureWithRatchet (childCount, 50), 1);
        REQUIRE (snapshot != nullptr);

        for (std::int64_t index = -33; index <= 96; ++index)
        {
            const StepEmission emission = evaluateStep (*snapshot, 0, index, runtime);
            const int laneCount = ratchetChildCount (snapshot->patterns[0], index);
            ++checks;
            childrenSeen += emission.noteCount;

            const bool ok = emission.gate && emission.noteCount == childCount && laneCount == childCount;

            if (! ok)
            {
                ++mismatches;

                if (firstMismatch.empty ())
                    firstMismatch = "childCount " + std::to_string (childCount) + " index " + std::to_string (index) +
                                    " gate " + (emission.gate ? "1" : "0") + " noteCount " +
                                    std::to_string (emission.noteCount) + " lane " + std::to_string (laneCount);
            }
        }
    }

    INFO ("checks " << checks << ", notes " << childrenSeen << ", first mismatch: " << firstMismatch);
    REQUIRE (checks == 8 * 130);
    REQUIRE (mismatches == 0);
    // 130 indices x (1 + 2 + … + 8) = 130 x 36.
    REQUIRE (childrenSeen == 130 * 36);
}

TEST_CASE ("sequencer/ratchet: only children past child 0 carry the ratchetChild provenance bit", "[unit]")
{
    // §5.4 X-RAY. Child 0 IS the step's own onset, so it is `core` and nothing else;
    // children 1.. are `core | ratchetChild`. The distinction is what lets the UI
    // badge a roll without badging the step that carries it.
    constexpr StepRuntime runtime {};
    const auto snapshot = buildPatternSnapshot (fixtureWithRatchet (maxRatchetChildren, 50), 1);
    REQUIRE (snapshot != nullptr);

    int checks = 0;
    int wrong = 0;

    for (std::int64_t index = -8; index <= 32; ++index)
    {
        const StepEmission emission = evaluateStep (*snapshot, 0, index, runtime);

        for (int child = 0; child < emission.noteCount; ++child)
        {
            const std::uint32_t expected =
                child > 0 ? (arpbox::engine::provenance::core | arpbox::engine::provenance::ratchetChild)
                          : arpbox::engine::provenance::core;
            ++checks;

            if (emission.notes[static_cast<std::size_t> (child)].provenance != expected)
                ++wrong;
        }
    }

    INFO ("checks " << checks);
    REQUIRE (checks == 41 * 8);
    REQUIRE (wrong == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. ONSETS ARE DERIVED FROM THE PARENT, NEVER ACCUMULATED
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/ratchet: child c of n sits at EXACTLY c / n steps past its parent", "[unit]")
{
    // THE `==` IS THE WHOLE TEST. Ratchet children are evenly spaced, so the
    // difference between `c / n` and `previous + 1 / n` is invisible to any tolerance
    // — and for every non-dyadic `n` it is a real drift, because `1 / 7` is not
    // representable. Three cumulative additions of the rounded slot do NOT equal
    // `3.0 / 7` bitwise, and at the canonical clock child 6 of 7 lands ~6 samples
    // early. A `Approx`-style comparison would pass on the broken implementation, so
    // this compares bit patterns.
    constexpr StepRuntime runtime {};

    int checks = 0;
    int inexact = 0;
    std::string firstInexact;

    for (int childCount = 1; childCount <= maxRatchetChildren; ++childCount)
    {
        const auto snapshot = buildPatternSnapshot (fixtureWithRatchet (childCount, 50), 1);
        REQUIRE (snapshot != nullptr);

        for (std::int64_t index = -17; index <= 48; ++index)
        {
            const StepEmission emission = evaluateStep (*snapshot, 0, index, runtime);

            if (emission.noteCount != childCount)
            {
                ++inexact;
                continue;
            }

            for (int child = 0; child < childCount; ++child)
            {
                const double expected = static_cast<double> (child) / static_cast<double> (childCount);
                const double actual = emission.notes[static_cast<std::size_t> (child)].positionInStep;
                ++checks;

                if (actual != expected)
                {
                    ++inexact;

                    if (firstInexact.empty ())
                        firstInexact = "n " + std::to_string (childCount) + " c " + std::to_string (child) + " index " +
                                       std::to_string (index) + " expected " + std::to_string (expected) + " got " +
                                       std::to_string (actual);
                }
            }

            // Child 0 IS the parent's onset — `positionInStep` is bit-zero there, so
            // `gridPpq + 0.0 * stepPpq` is bit-identical to `gridPpq` and no pre-7.2
            // golden can move on account of the child list existing.
            if (emission.notes[0].positionInStep != 0.0)
                ++inexact;

            // And no child ever reaches the NEXT step: `positionInStep` is `[0, 1)`,
            // which is what makes `maxChildAheadSteps` (7/8) the true ceiling the
            // retrigger scan-back derivation rests on.
            const double last = emission.notes[static_cast<std::size_t> (childCount - 1)].positionInStep;

            if (! (last < 1.0))
                ++inexact;
        }
    }

    INFO ("checks " << checks << ", first inexact: " << firstInexact);
    REQUIRE (checks == 66 * 36); // 66 indices x (1 + 2 + … + 8)
    REQUIRE (inexact == 0);

    // ── THE FAILURE MODE, STATED AS ARITHMETIC RATHER THAN AS PROSE ──────────
    // So the reason for the exact `==` above is checkable. Two forms, and the second
    // is the audible one.
    //
    // (a) BITWISE. Repeatedly adding the double nearest 1/7 does not reproduce
    //     `c / 7`. It happens to agree for c = 1..4 and diverges from c = 5 — which
    //     is exactly why a spot-check at one child count and one child index proves
    //     nothing and the sweep above is exhaustive over (n, c).
    const double slotOfSeven = 1.0 / 7.0;
    double cumulative = 0.0;

    for (int c = 0; c < 5; ++c)
        cumulative += slotOfSeven;

    REQUIRE (cumulative != 5.0 / 7.0);

    // (b) IN SAMPLES, which is what a listener would hear. A slot ROUNDED to whole
    //     samples and then accumulated drifts away from the parent-derived position:
    //     at the canonical clock the slot is 5760/7 = 822.857 samples, so six
    //     floored slots land five samples EARLY and six rounded slots one sample
    //     LATE, against the engine's floor (6 x 5760 / 7) = 4937.
    REQUIRE ((6 * samplesPerStep) / 7 == 4937);
    REQUIRE (6 * (samplesPerStep / 7) == 4932);
    REQUIRE (6 * ((samplesPerStep * 2 + 7) / 14) == 4938);
}

TEST_CASE ("sequencer/ratchet: the n = 7 child row lands on the engine's snap-then-FLOOR samples", "[unit]")
{
    // ── THE PLAN PROSE AND THE ENGINE DISAGREE; THE ENGINE IS PINNED ─────────
    // See the long note in this file's header. 5760 samples per step at 125 BPM /
    // 48 kHz on the 1/16 grid; `sampleForPpq` is ONE snap-then-floor, so child c of 7
    // lands on `floor (c * 5760 / 7)` and NOT on `llround (c * 5760 / 7)`. The
    // Phase-7 plan text lists the rounded row (…823, 1646, 2469…), which differs at
    // the three children whose fractional part exceeds ½.
    //
    // Both derivations are computed below in INTEGER arithmetic — `(c * 5760) / 7`
    // truncates toward zero and every operand is positive, so it IS floor — and the
    // literals are checked against them before the engine is asked. If this
    // arithmetic is wrong, the test says so instead of quietly agreeing with the
    // engine.
    constexpr std::int64_t floorRow[7] = { 0, 822, 1645, 2468, 3291, 4114, 4937 };
    constexpr std::int64_t planRoundedRow[7] = { 0, 823, 1646, 2469, 3291, 4114, 4937 };

    REQUIRE (samplesPerStep == 5760);

    for (int c = 0; c < 7; ++c)
    {
        const std::int64_t derivedFloor = (static_cast<std::int64_t> (c) * samplesPerStep) / 7;
        const std::int64_t derivedRounded = (static_cast<std::int64_t> (c) * samplesPerStep * 2 + 7) / 14;

        INFO ("child " << c << " floor " << derivedFloor << " rounded " << derivedRounded);
        REQUIRE (derivedFloor == floorRow[c]);
        REQUIRE (derivedRounded == planRoundedRow[c]);
    }

    // The two rows genuinely differ, so "the engine matches floorRow" is a real
    // statement about the rounding convention rather than a coincidence.
    REQUIRE (floorRow[1] != planRoundedRow[1]);
    REQUIRE (floorRow[2] != planRoundedRow[2]);
    REQUIRE (floorRow[3] != planRoundedRow[3]);
    REQUIRE (floorRow[4] == planRoundedRow[4]);

    // Two steps of RATCHET 7 at the default LEN 50 % — the gate (411 samples) is well
    // inside the child spacing (822), so no cutoff interferes and every note-on is a
    // pure placement.
    const auto render = renderRatchet (seedA, 7, 50, 100, 0.0, 2 * samplesPerStep, 128);
    const auto ons = noteOnSamples (render);

    INFO (render.describe (20));
    REQUIRE (render.isSampleSorted ());
    REQUIRE (ons.size () == 14);

    int matched = 0;
    std::string firstBad;

    for (int step = 0; step < 2; ++step)
        for (int c = 0; c < 7; ++c)
        {
            const std::int64_t expected = static_cast<std::int64_t> (step) * samplesPerStep + floorRow[c];
            const std::int64_t actual = ons[static_cast<std::size_t> (step * 7 + c)];

            if (actual == expected)
                ++matched;
            else if (firstBad.empty ())
                firstBad = "step " + std::to_string (step) + " child " + std::to_string (c) + " expected " +
                           std::to_string (expected) + " got " + std::to_string (actual);
        }

    INFO ("first bad: " << firstBad);
    REQUIRE (matched == 14);

    // The literal row, spelled out once for a reader who wants to check by hand.
    REQUIRE (ons[0] == 0);
    REQUIRE (ons[1] == 822);
    REQUIRE (ons[2] == 1645);
    REQUIRE (ons[3] == 2468);
    REQUIRE (ons[4] == 3291);
    REQUIRE (ons[5] == 4114);
    REQUIRE (ons[6] == 4937);
    REQUIRE (ons[7] == 5760);
    REQUIRE (ons[8] == 6582);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. THE VELOCITY RAMP
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/ratchet: a zero ramp returns the base velocity UNCHANGED, not rounded", "[unit]")
{
    // SHORT-CIRCUITED, NOT MERELY OPTIMISED. `llround (v * 1.0)` is only PROBABLY
    // `v`; the explicit early return makes every pre-7.2 golden's velocities a
    // property of the code rather than of the FPU. The same branch also covers
    // `childCount <= 1` (RATCHET's default, i.e. every existing pattern) and
    // `childIndex <= 0`.
    int checks = 0;
    int moved = 0;

    for (int base = 1; base <= 127; ++base)
    {
        for (int childCount = 1; childCount <= maxRatchetChildren; ++childCount)
            for (int child = 0; child < childCount; ++child)
            {
                ++checks;

                if (ratchetVelocity (base, child, childCount, 0.0) != base)
                    ++moved;
            }

        // `childCount <= 1` ignores the ramp entirely, at any ramp value.
        for (const double ramp : { -100.0, -50.0, 37.5, 100.0 })
        {
            ++checks;

            if (ratchetVelocity (base, 5, 1, ramp) != base)
                ++moved;
        }

        // …and so does child 0, at any ramp value.
        for (const double ramp : { -100.0, -50.0, 37.5, 100.0 })
        {
            ++checks;

            if (ratchetVelocity (base, 0, 8, ramp) != base)
                ++moved;
        }
    }

    INFO ("checks " << checks);
    REQUIRE (checks == 127 * (36 + 4 + 4));
    REQUIRE (moved == 0);

    // A negative child index is defensive, not reachable — child 0 is the step's own
    // onset and callers pass `>= 0`. It must still degrade to the base velocity
    // rather than extrapolating backwards.
    REQUIRE (ratchetVelocity (100, -1, 8, -50.0) == 100);
    REQUIRE (ratchetVelocity (100, -1000, 8, 100.0) == 100);
}

TEST_CASE ("sequencer/ratchet: the ramp is linear over childCount - 1 and clamps into MIDI 1..127", "[unit]")
{
    // LITERAL TABLES, hand-derived from "linear from `base` at child 0 to
    // `base * (1 + ramp / 100)` at child `childCount - 1`, `llround`ed, clamped to
    // 1..127". Interpolating over `childCount - 1` (not `childCount`) is what makes
    // "-50 % halves the LAST child" literally true at every child count, which is the
    // only reading a user can predict by ear — and it is why the -50 rows all end on
    // exactly 50 and the -100 rows all end on the clamp floor.
    //
    // Worked example for the eight-child row: t = c / 7, v = llround (100 - 50 t) ⇒
    // 100, 92.857→93, 85.714→86, 78.571→79, 71.428→71, 64.285→64, 57.142→57, 50.

    SECTION ("decaying rolls: ramp -50 % at five child counts")
    {
        const int cc2[2] = { 100, 50 };
        const int cc3[3] = { 100, 75, 50 };
        const int cc5[5] = { 100, 88, 75, 63, 50 };
        const int cc7[7] = { 100, 92, 83, 75, 67, 58, 50 };
        const int cc8[8] = { 100, 93, 86, 79, 71, 64, 57, 50 };

        for (int c = 0; c < 2; ++c)
        {
            INFO ("cc2 child " << c);
            REQUIRE (ratchetVelocity (100, c, 2, -50.0) == cc2[c]);
        }

        for (int c = 0; c < 3; ++c)
        {
            INFO ("cc3 child " << c);
            REQUIRE (ratchetVelocity (100, c, 3, -50.0) == cc3[c]);
        }

        for (int c = 0; c < 5; ++c)
        {
            INFO ("cc5 child " << c);
            REQUIRE (ratchetVelocity (100, c, 5, -50.0) == cc5[c]);
        }

        for (int c = 0; c < 7; ++c)
        {
            INFO ("cc7 child " << c);
            REQUIRE (ratchetVelocity (100, c, 7, -50.0) == cc7[c]);
        }

        for (int c = 0; c < 8; ++c)
        {
            INFO ("cc8 child " << c);
            REQUIRE (ratchetVelocity (100, c, 8, -50.0) == cc8[c]);
        }
    }

    SECTION ("rising rolls saturate at the MIDI ceiling")
    {
        // +100 % at childCount 2 asks for 200 and gets 127 — the clamp, at the top.
        REQUIRE (ratchetVelocity (100, 0, 2, 100.0) == 100);
        REQUIRE (ratchetVelocity (100, 1, 2, 100.0) == 127);

        const int rise100[8] = { 100, 114, 127, 127, 127, 127, 127, 127 };
        const int rise50[8] = { 100, 107, 114, 121, 127, 127, 127, 127 };

        for (int c = 0; c < 8; ++c)
        {
            INFO ("+100 % child " << c);
            REQUIRE (ratchetVelocity (100, c, 8, 100.0) == rise100[c]);
        }

        for (int c = 0; c < 8; ++c)
        {
            INFO ("+50 % child " << c);
            REQUIRE (ratchetVelocity (100, c, 8, 50.0) == rise50[c]);
        }

        // A base already at the ceiling cannot rise at all.
        for (int c = 0; c < 8; ++c)
        {
            INFO ("base 127 child " << c);
            REQUIRE (ratchetVelocity (127, c, 8, 100.0) == 127);
        }
    }

    SECTION ("silencing rolls saturate at the MIDI floor, never at 0")
    {
        // §12.1 puts VEL at 1..127, so a note can never be velocity 0 (which is a
        // note-OFF on the wire). -100 % asks for 0 at the last child and gets 1.
        const int fall100[4] = { 100, 67, 33, 1 };

        for (int c = 0; c < 4; ++c)
        {
            INFO ("-100 % child " << c);
            REQUIRE (ratchetVelocity (100, c, 4, -100.0) == fall100[c]);
        }

        REQUIRE (ratchetVelocity (5, 1, 2, -100.0) == 1);

        for (int c = 0; c < 8; ++c)
        {
            INFO ("base 1 child " << c);
            REQUIRE (ratchetVelocity (1, c, 8, -100.0) == 1);
        }
    }

    SECTION ("a child index past the last child holds the last child's value")
    {
        // `jmin (childIndex, childCount - 1)`. Not reachable from `evaluateStep` (it
        // loops to `childCount`), but the clamp is what stops a future caller
        // extrapolating past the ramp's endpoint.
        REQUIRE (ratchetVelocity (100, 7, 8, -50.0) == 50);
        REQUIRE (ratchetVelocity (100, 8, 8, -50.0) == 50);
        REQUIRE (ratchetVelocity (100, 99, 8, -50.0) == 50);
        REQUIRE (ratchetVelocity (100, 99, 8, 100.0) == 127);
    }

    SECTION ("the whole surface agrees with the independently written contract")
    {
        // The sweep behind the literal tables: every base, every child count, every
        // child, over a ramp grid that includes both range ends, both signs, zero and
        // a non-dyadic value.
        const double ramps[] = { -100.0, -75.0, -50.0, -12.5, -1.0, 0.0, 1.0, 33.0, 50.0, 87.5, 100.0 };

        int checks = 0;
        int mismatches = 0;
        int atCeiling = 0;
        int atFloor = 0;
        std::string firstMismatch;

        for (const double ramp : ramps)
            for (int base = 1; base <= 127; ++base)
                for (int childCount = 1; childCount <= maxRatchetChildren; ++childCount)
                    for (int child = 0; child < childCount; ++child)
                    {
                        const int actual = ratchetVelocity (base, child, childCount, ramp);
                        const int expected = expectedRatchetVelocity (base, child, childCount, ramp);
                        ++checks;

                        if (actual == 127)
                            ++atCeiling;

                        if (actual == 1)
                            ++atFloor;

                        if (actual != expected)
                        {
                            ++mismatches;

                            if (firstMismatch.empty ())
                                firstMismatch = "ramp " + std::to_string (ramp) + " base " + std::to_string (base) +
                                                " n " + std::to_string (childCount) + " c " + std::to_string (child) +
                                                " expected " + std::to_string (expected) + " got " +
                                                std::to_string (actual);
                        }
                    }

        INFO ("checks " << checks << ", at 127 " << atCeiling << ", at 1 " << atFloor
                        << ", first mismatch: " << firstMismatch);
        REQUIRE (checks == 11 * 127 * 36);
        REQUIRE (mismatches == 0);
        // ANTI-VACUITY: both clamp arms are actually exercised by the sweep.
        REQUIRE (atCeiling > 0);
        REQUIRE (atFloor > 0);
    }
}

TEST_CASE ("sequencer/ratchet: the document clamps the ramp into -100..+100 and it reaches the snapshot", "[unit]")
{
    // PROJECT-LEVEL, like swing and the grid (§8.1 groups feel controls under
    // `transport`), so there is no pattern index. CLAMPED rather than rejected,
    // because §5.3's macros and mod matrix will eventually drive this continuously
    // and must not fail on overshoot; a NON-FINITE value IS rejected, because there
    // is nothing to clamp a NaN to and it would reach `llround`.
    REQUIRE (minRatchetVelocityRampPct == -100.0);
    REQUIRE (maxRatchetVelocityRampPct == 100.0);

    struct Row
    {
        double requested;
        double stored;
    };

    const Row rows[] = { { 0.0, 0.0 },      { -50.0, -50.0 },   { 50.0, 50.0 },   { -100.0, -100.0 },
                         { 100.0, 100.0 },  { -100.5, -100.0 }, { 100.5, 100.0 }, { -4000.0, -100.0 },
                         { 4000.0, 100.0 }, { 37.5, 37.5 } };

    int checks = 0;
    int wrong = 0;
    std::string firstWrong;

    for (const auto& row : rows)
    {
        PatternDocument document;
        REQUIRE (document.state ().ratchetVelocityRampPct == 0.0);

        document.setRatchetVelocityRamp (row.requested);
        const auto snapshot = buildPatternSnapshot (document.state (), 1);
        ++checks;

        if (snapshot == nullptr || snapshot->ratchetVelocityRampPct != row.stored)
        {
            ++wrong;

            if (firstWrong.empty ())
                firstWrong =
                    "requested " + std::to_string (row.requested) + " expected " + std::to_string (row.stored) +
                    " got " +
                    (snapshot != nullptr ? std::to_string (snapshot->ratchetVelocityRampPct) : std::string ("<null>"));
        }
    }

    INFO ("checks " << checks << ", first wrong: " << firstWrong);
    REQUIRE (checks == 10);
    REQUIRE (wrong == 0);

    // The default is FLAT and setting it again is a no-op — no undo slot, no ~125 KB
    // rebuild. That is what makes "every child of a default project carries its
    // step's own VEL byte-identically" a property of the pipeline.
    PatternDocument document;
    REQUIRE (! document.setRatchetVelocityRamp (0.0));
    REQUIRE (document.setRatchetVelocityRamp (-50.0));
    REQUIRE (! document.setRatchetVelocityRamp (-50.0));
    // Clamped BEFORE the no-op comparison, so 4000 -> 100 twice is one edit.
    REQUIRE (document.setRatchetVelocityRamp (4000.0));
    REQUIRE (! document.setRatchetVelocityRamp (999.0));
}

TEST_CASE ("sequencer/ratchet: evaluateStep applies the snapshot ramp to every child", "[unit]")
{
    // The wiring, end to end inside the pure core: the project ramp reaches the note
    // list through `ratchetVelocity`, against the step's OWN VEL rather than against
    // a constant.
    constexpr StepRuntime runtime {};

    const auto snapshot = mutableSnapshotOf (fixtureWithRatchet (maxRatchetChildren, 50));
    REQUIRE (snapshot != nullptr);
    snapshot->ratchetVelocityRampPct = -50.0;
    pokeLane (*snapshot, LaneId::vel, 100);

    const StepEmission emission = evaluateStep (*snapshot, 0, 0, runtime);
    REQUIRE (emission.gate);
    REQUIRE (emission.noteCount == 8);

    const int expected[8] = { 100, 93, 86, 79, 71, 64, 57, 50 };

    for (int c = 0; c < 8; ++c)
    {
        INFO ("child " << c);
        REQUIRE (emission.notes[static_cast<std::size_t> (c)].velocity == expected[c]);
    }

    // A different step VEL rescales the whole ramp — the ramp is a PERCENTAGE of the
    // step's velocity, not an absolute offset.
    pokeLane (*snapshot, LaneId::vel, 64);
    const StepEmission quiet = evaluateStep (*snapshot, 0, 0, runtime);
    REQUIRE (quiet.noteCount == 8);

    for (int c = 0; c < 8; ++c)
    {
        INFO ("quiet child " << c);
        REQUIRE (quiet.notes[static_cast<std::size_t> (c)].velocity == expectedRatchetVelocity (64, c, 8, -50.0));
    }

    REQUIRE (quiet.notes[0].velocity == 64);
    REQUIRE (quiet.notes[7].velocity == 32);
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. PER-CHILD PROBABILITY: SEED-EXACT, AND SHORT-CIRCUITED AT THE DEFAULTS
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/ratchet: per-child probability matches an independently written subStepHash", "[unit]")
{
    // The reference is checked against splitmix64's published known-answer vector
    // BEFORE it is used as an oracle. Without this, a typo in the reference and a
    // typo in the engine could cancel — and the whole point of a second
    // implementation is that they cannot.
    REQUIRE (referenceSplitmix64 (0) == 0xE220A8397B1DCDAFULL);

    const int percents[] = { 1, 25, 50, 75, 99 };
    const std::uint64_t seeds[] = { 0ULL, seedA, seedB, seedC };

    // Index range straddles zero: NEGATIVE step indices are legal (the retrigger
    // lookahead scans one step BACK, and the locate paths sweep below 0), and the
    // conversion to uint64 is modulo 2^64, so index -1 hashes as
    // 0xFFFF'FFFF'FFFF'FFFF. A reference that agreed only on the non-negative half
    // would be worthless exactly where the lookahead lives.
    constexpr std::int64_t firstIndex = -400;
    constexpr std::int64_t lastIndex = 400;

    int checks = 0;
    int mismatches = 0;
    int passed = 0;
    std::string firstMismatch;

    for (const std::uint64_t seed : seeds)
        for (const int percent : percents)
        {
            const auto snapshot = mutableSnapshotOf (ratchetFixtureState (seed));
            REQUIRE (snapshot != nullptr);
            pokeLane (*snapshot, LaneId::prob, percent);
            const PatternData& data = snapshot->patterns[0];
            REQUIRE (data.masterSeed == seed);

            for (std::int64_t index = firstIndex; index <= lastIndex; ++index)
                // Children 1..8. Child 0 is short-circuited (its own case below) and
                // 8 is one past §12.1's ceiling — `ratchetChildPasses` does not bound
                // `childIndex`, and the reference must agree there too.
                for (int child = 1; child <= 8; ++child)
                {
                    const bool actual = ratchetChildPasses (data, index, child);
                    const bool expected = expectedChildPasses (seed, percent, index, child);
                    ++checks;
                    passed += actual ? 1 : 0;

                    if (actual != expected)
                    {
                        ++mismatches;

                        if (firstMismatch.empty ())
                            firstMismatch = "seed " + std::to_string (seed) + " p " + std::to_string (percent) +
                                            " index " + std::to_string (index) + " child " + std::to_string (child) +
                                            " expected " + (expected ? "true" : "false") + " got " +
                                            (actual ? "true" : "false");
                    }
                }
        }

    INFO ("checks " << checks << ", passed " << passed << ", first mismatch: " << firstMismatch);
    REQUIRE (checks == 4 * 5 * static_cast<int> (lastIndex - firstIndex + 1) * 8);
    REQUIRE (checks > 100000);
    REQUIRE (mismatches == 0);
    // ANTI-VACUITY: a constant-returning implementation would need a
    // constant-returning reference. State the shape directly — the five percentages
    // average 50 %, so roughly half of the rolls pass and BOTH outcomes are present.
    REQUIRE (passed > checks / 4);
    REQUIRE (passed < (checks * 3) / 4);
}

TEST_CASE ("sequencer/ratchet: the PROB short-circuits and child 0 consume no randomness", "[unit]")
{
    // THREE EARLY EXITS, all contracts rather than optimisations, and the observable
    // form of "consumed no hash" is "the answer does not depend on the seed".
    //
    //   childIndex <= 0  — child 0 IS the step's own onset; its fate was decided by
    //     `conditionPasses` + `probabilityPasses`. Rolling again for it would mean a
    //     RATCHET-1 step (the default) consumed different randomness from a
    //     RATCHET-1 step before Phase 7.2, i.e. EVERY existing pattern would change.
    //   percent >= 100  — the PROB lane default, so a RATCHET-8 pattern on a fresh
    //     document is fully deterministic and provably insensitive to anything
    //     Phase 12 does to the seed composition.
    //   percent <= 0    — symmetric: never fires, never rolls.
    int checks = 0;
    int wrong = 0;
    std::string firstWrong;

    for (const std::uint64_t seed : { 0ULL, seedA, seedB, seedC, 0xFFFFFFFFFFFFFFFFULL })
    {
        const auto snapshot = mutableSnapshotOf (ratchetFixtureState (seed));
        REQUIRE (snapshot != nullptr);

        for (const int percent : { -32768, -1, 0, 100, 101, 32767 })
        {
            pokeLane (*snapshot, LaneId::prob, percent);
            const PatternData& data = snapshot->patterns[0];
            const bool expectedForChildren = percent >= 100;

            for (std::int64_t index = -60; index <= 60; ++index)
            {
                // Child 0 (and any defensive negative index) is ALWAYS true, even at
                // PROB 0 — the `childIndex <= 0` guard runs before the lane is read.
                for (const int child : { -5, 0 })
                {
                    ++checks;

                    if (! ratchetChildPasses (data, index, child))
                    {
                        ++wrong;

                        if (firstWrong.empty ())
                            firstWrong = "child " + std::to_string (child) + " must always pass (p " +
                                         std::to_string (percent) + ", index " + std::to_string (index) + ")";
                    }
                }

                for (int child = 1; child <= 8; ++child)
                {
                    ++checks;

                    if (ratchetChildPasses (data, index, child) != expectedForChildren)
                    {
                        ++wrong;

                        if (firstWrong.empty ())
                            firstWrong = "seed " + std::to_string (seed) + " p " + std::to_string (percent) +
                                         " index " + std::to_string (index) + " child " + std::to_string (child) +
                                         " expected " + (expectedForChildren ? "true" : "false");
                    }
                }
            }
        }
    }

    INFO ("checks " << checks << ", first wrong: " << firstWrong);
    REQUIRE (checks == 5 * 6 * 121 * 10);
    REQUIRE (wrong == 0);
}

TEST_CASE ("sequencer/ratchet: a RATCHET-8 PROB-100 pattern renders identically under three master seeds", "[unit]")
{
    // THE BLACK-BOX HALF of the `>= 100` short-circuit, and the tripwire on the path
    // that loses it. `hash % 100 < 100` is always true, so deleting the branch
    // changes nothing TODAY; then Phase 12 adds LOOP LOCK or a new `RngDomain`, the
    // hash input shifts, and every default-PROB pattern — including every baked
    // golden — moves, with the diff pointing at Phase 12 rather than at the deletion.
    //
    // Eight children per step is what makes this probe worth running here rather than
    // leaving it to step_probability.cpp: the per-child roll is a SECOND consumer of
    // the seed on the audible path, reached seven times per step, and it would carry
    // the divergence on its own.
    constexpr std::int64_t span = 4 * samplesPerStep;

    const auto renderA = renderRatchet (seedA, 8, 50, 100, 0.0, span, 128);
    const auto renderB = renderRatchet (seedB, 8, 50, 100, 0.0, span, 128);
    const auto renderC = renderRatchet (seedC, 8, 50, 100, 0.0, span, 128);

    INFO (renderA.describeDifference (renderB));
    REQUIRE (renderA.events.size () == renderB.events.size ());
    REQUIRE (renderA.toByteStream () == renderB.toByteStream ());
    REQUIRE (renderA.toByteStream () == renderC.toByteStream ());

    // ANTI-VACUITY: the render has to contain the full un-thinned roll, or
    // "identical" is trivially true of two empty streams. 4 steps x 8 children.
    const auto ons = noteOnSamples (renderA);
    INFO (renderA.describe (12));
    REQUIRE (ons.size () == 32);
    REQUIRE (ons.front () == 0);
    REQUIRE (ons[1] == samplesPerStep / 8);
}

TEST_CASE ("sequencer/ratchet: at PROB 50 the same seeds DIVERGE and the roll is thinned", "[unit]")
{
    // THE COMPLEMENT, and it is not optional. Without it the probe above passes for
    // the boring reason that `masterSeed` reaches nothing on the audible path — which
    // is exactly what would happen if `ratchetChildPasses` were short-circuited for
    // every value, or if the child loop stopped consulting it. This case is what says
    // the per-child roll IS wired through and the probe's green means what it claims.
    constexpr std::int64_t span = 8 * samplesPerStep;

    const auto full = renderRatchet (seedA, 8, 50, 100, 0.0, span, 128);
    const auto thinnedA = renderRatchet (seedA, 8, 50, 50, 0.0, span, 128);
    const auto thinnedB = renderRatchet (seedB, 8, 50, 50, 0.0, span, 128);

    REQUIRE (thinnedA.toByteStream () != thinnedB.toByteStream ());
    REQUIRE (thinnedA.toByteStream () != full.toByteStream ());

    const auto fullOns = noteOnSamples (full);
    const auto onsA = noteOnSamples (thinnedA);
    const auto onsB = noteOnSamples (thinnedB);

    INFO ("full " << fullOns.size () << ", seedA " << onsA.size () << ", seedB " << onsB.size ());
    REQUIRE (fullOns.size () == 64); // 8 steps x 8 children, nothing thinned

    // Loose NON-VACUITY floors, not a distributional contract. At PROB 50 child 0
    // survives half the time and takes the whole step with it when it does not, so
    // the expectation is ~8 x 0.5 x (1 + 7 x 0.5) = 18 note-ons.
    REQUIRE (onsA.size () > 4);
    REQUIRE (onsA.size () < 56);
    REQUIRE (onsB.size () > 4);
    REQUIRE (onsB.size () < 56);
    REQUIRE (onsA != onsB);

    // PROB THINS; IT NEVER MOVES A NOTE OR INVENTS ONE. Every surviving child sits on
    // a position the un-thinned roll also used.
    int offGrid = 0;

    for (const std::int64_t sample : onsA)
        if (std::find (fullOns.begin (), fullOns.end (), sample) == fullOns.end ())
            ++offGrid;

    INFO ("positions absent from the PROB-100 roll: " << offGrid);
    REQUIRE (offGrid == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. LEN APPLIES TO THE CHILD'S OWN SLOT
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/ratchet: gateFractionOfStep is LEN divided by the child count, exactly", "[unit]")
{
    // §12.1 stores LEN as a percentage of the STEP; `StepNote::gateFractionOfStep` is
    // that percentage measured against the child's OWN slot, i.e. already divided by
    // `noteCount`. Two consequences, both wanted (see the long note on
    // `StepEmission::noteCount`):
    //
    //   * at `noteCount == 1` — RATCHET's default, and every pre-7.2 golden — the
    //     slot IS the step, so the arithmetic is bit-identical and no existing note
    //     changes length by a sample;
    //   * it is the only reading under which LEN > 100 % ties ACROSS CHILDREN the way
    //     it already ties across steps.
    constexpr StepRuntime runtime {};

    int checks = 0;
    int inexact = 0;
    std::string firstInexact;

    for (const int lenPercent : { 1, 25, 50, 100, 137, 200, 400 })
        for (int childCount = 1; childCount <= maxRatchetChildren; ++childCount)
        {
            const auto snapshot = buildPatternSnapshot (fixtureWithRatchet (childCount, lenPercent), 1);
            REQUIRE (snapshot != nullptr);

            // The SAME expression shape the engine uses — LEN over 100, then over the
            // child count — so the comparison is bitwise rather than approximate.
            const double expected = static_cast<double> (lenPercent) / 100.0 / static_cast<double> (childCount);

            for (std::int64_t index = -9; index <= 24; ++index)
            {
                const StepEmission emission = evaluateStep (*snapshot, 0, index, runtime);

                for (int child = 0; child < emission.noteCount; ++child)
                {
                    ++checks;

                    if (emission.notes[static_cast<std::size_t> (child)].gateFractionOfStep != expected)
                    {
                        ++inexact;

                        if (firstInexact.empty ())
                            firstInexact =
                                "LEN " + std::to_string (lenPercent) + " n " + std::to_string (childCount) + " c " +
                                std::to_string (child) + " expected " + std::to_string (expected) + " got " +
                                std::to_string (emission.notes[static_cast<std::size_t> (child)].gateFractionOfStep);
                    }
                }
            }
        }

    INFO ("checks " << checks << ", first inexact: " << firstInexact);
    REQUIRE (checks == 7 * 34 * 36); // 7 LEN values x 34 indices x (1 + 2 + … + 8)
    REQUIRE (inexact == 0);

    // At RATCHET 1 the slot IS the step: the value is LEN/100 untouched, which is the
    // statement that makes every pre-7.2 golden provably unmoved.
    const auto single = buildPatternSnapshot (fixtureWithRatchet (1, 50), 1);
    REQUIRE (single != nullptr);
    const StepEmission one = evaluateStep (*single, 0, 0, runtime);
    REQUIRE (one.noteCount == 1);
    REQUIRE (one.notes[0].gateFractionOfStep == 0.5);
}

TEST_CASE ("sequencer/ratchet: a RATCHET-8 LEN-50 % roll leaves a 360-sample gap before each child", "[unit]")
{
    // ── THE FAILS-WITHOUT SHAPE, STATED SO THE LITERALS BELOW MEAN SOMETHING ──
    // At the canonical clock a step is 5760 samples, so eight children sit 720 apart.
    // LEN 50 % against the CHILD'S OWN SLOT gives each note
    // `llround (0.5 / 8 x 5760)` = 360 samples, i.e. half its slot, and a 360-sample
    // gap before the next child. Under the REJECTED reading — LEN against the WHOLE
    // step — each child would ask for `llround (0.5 x 5760)` = 2880 samples, every
    // child except the last would be cut short by its successor at
    // `nextOnset - 1`, and all seven gaps would collapse to EXACTLY ONE SAMPLE. So
    // "360" is not a magic number: it is the difference between the two readings, and
    // the LEN-400 % case below is what a genuine one-sample chain looks like.
    constexpr std::int64_t span = 4 * samplesPerStep;
    constexpr std::int64_t childSpacing = 720; // 5760 / 8
    constexpr std::int64_t childLength = 360;  // llround (0.5 / 8 x 5760)

    REQUIRE (samplesPerStep / 8 == childSpacing);
    REQUIRE (childSpacing / 2 == childLength);

    const auto render = renderRatchet (seedA, 8, 50, 100, 0.0, span, 128);
    const auto ons = noteOnSamples (render);
    const auto offs = noteOffSamples (render);

    INFO (render.describe (16));
    REQUIRE (render.isSampleSorted ());
    REQUIRE (ons.size () == 32); // 4 steps x 8 children
    REQUIRE (offs.size () == 32);

    int spacedPairs = 0;
    int correctLengths = 0;
    std::string firstBad;

    for (std::size_t i = 0; i < ons.size (); ++i)
    {
        if (offs[i] == ons[i] + childLength)
            ++correctLengths;
        else if (firstBad.empty ())
            firstBad = "note " + std::to_string (i) + " on " + std::to_string (ons[i]) + " off " +
                       std::to_string (offs[i]) + " expected off " + std::to_string (ons[i] + childLength);

        if (i + 1 < ons.size () && ons[i + 1] == ons[i] + childSpacing)
            ++spacedPairs;
    }

    INFO ("first bad: " << firstBad);
    REQUIRE (correctLengths == 32);
    REQUIRE (spacedPairs == 31);

    // The literal row, so a reader can check the geometry by hand: every child is
    // half its slot long and the gap before its successor is the other half.
    REQUIRE (ons[0] == 0);
    REQUIRE (offs[0] == 360);
    REQUIRE (ons[1] == 720);
    REQUIRE (offs[1] == 1080);
    REQUIRE (ons[7] == 5040);
    REQUIRE (offs[7] == 5400);
    REQUIRE (ons[8] == 5760); // child 0 of the next step, exactly on the grid
    REQUIRE (offs[8] == 6120);

    // AND THE GAP IS NOT ONE SAMPLE — the rejected reading's signature.
    REQUIRE (ons[1] - offs[0] == 360);
    REQUIRE (ons[8] - offs[7] == 360);
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. THE INTRA-STEP SAME-PITCH CHAIN AND ITS 1-SAMPLE GAPS
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/ratchet: LEN 400 % chains every child with §5.5's 1-sample gap", "[unit]")
{
    // ONE-NOTE POOL + RATCHET 8 + LEN 400 % ⇒ every child is a SAME-PITCH RETRIGGER
    // of its predecessor. Each child asks for `llround (4.0 / 8 x 5760)` = 2880
    // samples — four slots — so §5.5's overlap policy applies at every child boundary
    // and at every step boundary: note-off, then note-on, with a one-sample gap.
    //
    // AND THE GAP COMES FROM THE ABSOLUTE-TIMELINE SCHEDULE, NOT FROM A BLOCK OFFSET
    // (issues #36/#46/#48). `cutoffForSamePitch` finds child c+1 in its `ahead == 0`
    // band and schedules child c's off at `onset (c + 1) - 1`; the table then places
    // it in whichever block contains that sample — INCLUDING THE PREVIOUS BLOCK, when
    // the retrigger lands on a block head. `offset - 1` structurally could not reach
    // there, which is why the block-size sweep below is part of this case and not a
    // separate one.
    constexpr std::int64_t span = 32 * samplesPerStep; // 184320 — a multiple of 32, 128 AND 4096
    constexpr std::int64_t childSpacing = 720;

    static_assert (span == 184320, "the sweep's span must divide by every swept block size");
    static_assert (span % 4096 == 0, "4096-sample blocks must cover exactly the same span as 32-sample ones");

    const auto reference = renderRatchet (seedA, 8, 400, 100, 0.0, span, 128);
    const auto ons = noteOnSamples (reference);
    const auto offs = noteOffSamples (reference);

    INFO (reference.describe (16));
    REQUIRE (reference.isSampleSorted ());
    REQUIRE (ons.size () == 256); // 32 steps x 8 children
    REQUIRE (offs.size () == 256);

    // EVERY consecutive pair, children and step boundaries alike: the outgoing note's
    // off sits exactly one sample before the incoming note's on.
    int chainedPairs = 0;
    int spacedPairs = 0;
    std::string firstBad;

    for (std::size_t i = 0; i + 1 < ons.size (); ++i)
    {
        if (offs[i] == ons[i + 1] - 1)
            ++chainedPairs;
        else if (firstBad.empty ())
            firstBad = "pair " + std::to_string (i) + " off " + std::to_string (offs[i]) + " next on " +
                       std::to_string (ons[i + 1]) + " expected off " + std::to_string (ons[i + 1] - 1);

        if (ons[i + 1] == ons[i] + childSpacing)
            ++spacedPairs;
    }

    INFO ("first bad: " << firstBad);
    REQUIRE (chainedPairs == 255);
    REQUIRE (spacedPairs == 255);

    // Absolute-sample literals: the first intra-step gap, and the one that straddles
    // a step boundary (child 7 of step 0 into child 0 of step 1).
    REQUIRE (ons[0] == 0);
    REQUIRE (offs[0] == 719);
    REQUIRE (ons[1] == 720);
    REQUIRE (ons[7] == 5040);
    REQUIRE (offs[7] == 5759);
    REQUIRE (ons[8] == 5760);

    // Nothing hangs: the final child's off is scheduled against the step that would
    // have followed it and lands on the last sample of the span.
    REQUIRE (ons.back () == span - childSpacing);
    REQUIRE (offs.back () == span - 1);

    // THE BUFFER-SIZE SWEEP. `span` is a multiple of all three block sizes, so every
    // render covers exactly the same musical time and the byte streams are directly
    // comparable with no trimming.
    const auto referenceStream = reference.toByteStream ();
    REQUIRE (! referenceStream.empty ());

    int sizesChecked = 0;
    int sizesMatched = 0;
    std::string firstMismatch;

    for (const int blockSize : { 32, 128, 4096 })
    {
        const auto render = renderRatchet (seedA, 8, 400, 100, 0.0, span, blockSize);
        ++sizesChecked;
        REQUIRE (render.numSamples == span);

        if (render.toByteStream () == referenceStream)
            ++sizesMatched;
        else if (firstMismatch.empty ())
            firstMismatch = "block size " + std::to_string (blockSize) + ": " +
                            reference.describeDifference (render).toStdString ();
    }

    INFO (firstMismatch);
    REQUIRE (sizesChecked == 3);
    REQUIRE (sizesMatched == 3);
}
