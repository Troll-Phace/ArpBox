// ─────────────────────────────────────────────────────────────────────────────
// pattern_polymeter — Phase 6.2's L1 lane engine against a CLOSED FORM WRITTEN
// INDEPENDENTLY IN THIS FILE (ARCHITECTURE §5.1 L1 "each lane has independent
// length + clock division ⇒ polymeter", §12.1; INSTRUCTIONS Phase 6 success
// criterion "Polymetric lanes phase correctly (verified against hand-computed
// golden)").
//
// ── WHAT MAKES THIS A HAND-COMPUTED CHECK AND NOT A MIRROR ──────────────────
// Nothing here calls `laneIndex`, `laneValueAt`, `isLaneTick`, `gatedOrdinal` or
// `poolNoteAtDegree`. The expected gate / pitch / velocity for step n are
// `G[n%5]`, `P[n%3]`, `V[n%7]` written out longhand, and the pool degree walk is
// re-implemented in eight lines below. A test that called the production formula
// would agree with any bug in it.
//
// ── STEP INDICES ARE RECOVERED FROM ABSOLUTE SAMPLES, NEVER FROM ORDINALS ───
// It is tempting to say "the i-th note-on is step i". It is also wrong the moment
// the GATE lane has a rest in it — which is the entire point of this file — and
// the error would hide precisely the gate bug the set assertion exists to catch.
// So every note-on's step index is recovered as `absoluteSample / 6000` (120 BPM
// @ 48 kHz on the 1/16 grid makes a step exactly 6000 samples, and the test
// asserts the divisibility before dividing), and the emitted step SET is then
// compared against the model set — which catches an EXTRA step as well as a
// missing one.
//
// ── THE POOL ────────────────────────────────────────────────────────────────
// Five notes spanning 11 semitones: {60, 62, 65, 67, 71}. The span being under an
// octave makes `poolNoteAtDegree` STRICTLY INCREASING in the degree, so a MIDI
// note number maps back to exactly one degree — which is what lets the pitch
// LANE OFFSET be recovered from the emitted stream (see `observedOf`). The
// default 8-note C-major stub pool cannot do this: its top note 72 is also its
// bottom note 60 an octave up, so degree 7 and degree 8 both emit 72.
// ─────────────────────────────────────────────────────────────────────────────

#include "support/MidiRenderHarness.h"
#include "support/NoteLifecycleCheck.h"
#include "support/SequencerRenderRig.h"

#include "engine/graph/EngineCommand.h"
#include "engine/midi/NotePool.h"
#include "engine/sequencer/PatternDocument.h"
#include "engine/sequencer/PatternTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

using arpbox::engine::EngineCommandType;
using arpbox::engine::LaneId;
using arpbox::engine::PatternDocument;
using arpbox::engine::PoolSnapshot;
using arpbox::testing::engineCommand;
using arpbox::testing::MidiRenderConfig;
using arpbox::testing::MidiRenderResult;
using arpbox::testing::NoteLifecycleTracker;
using arpbox::testing::renderSequencer;
using arpbox::testing::ScheduledCommand;
using arpbox::testing::SequencerRig;
using arpbox::testing::TimedMidiEvent;

namespace
{
constexpr double testSampleRate = 48000.0;
constexpr double testBpm = 120.0;
constexpr int testBlockSize = 128;

/** 1/16 note at 120 BPM / 48 kHz. Exact, so `sample / stepSamples` recovers the
    step index with no rounding question. */
constexpr std::int64_t stepSamples = 6000;

// ── THE POOL, AND A LOCAL RE-IMPLEMENTATION OF THE DEGREE WALK ───────────────

constexpr int poolSize = 5;
constexpr std::uint8_t poolPitches[poolSize] = { 60, 62, 65, 67, 71 };

/** Floor division, written out here rather than borrowed from NotePool.h. */
int floorDivLocal (int a, int b) noexcept
{
    const int q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

/** The pool note at signed degree `k`, with octave carry — an independent copy of
    the §12.1 PITCH semantics. */
int noteAtDegree (int k) noexcept
{
    const int oct = floorDivLocal (k, poolSize);
    return static_cast<int> (poolPitches[static_cast<std::size_t> (k - oct * poolSize)]) + 12 * oct;
}

/** The unique degree that emits `note`, or a sentinel. Well-defined only because
    the pool spans under an octave — asserted in the test below. */
constexpr int noDegree = -9999;
int degreeOfNote (int note) noexcept
{
    for (int k = -40; k <= 40; ++k)
        if (noteAtDegree (k) == note)
            return k;
    return noDegree;
}

// ── THE POLYMETRIC MODEL (lengths 5 / 3 / 7 ⇒ combined period lcm = 105) ─────

constexpr int gateLen = 5;
constexpr int pitchLen = 3;
constexpr int velLen = 7;

constexpr int gateCycle[gateLen] = { 1, 0, 1, 1, 0 };
constexpr int pitchCycle[pitchLen] = { 0, 1, -2 };
constexpr int velCycle[velLen] = { 10, 24, 38, 52, 66, 80, 94 };

bool modelGated (int n) noexcept
{
    return gateCycle[static_cast<std::size_t> (n % gateLen)] != 0;
}

/** Gated steps STRICTLY BEFORE `n` — the pool cursor under the gated-cursor rule.
    Counted by brute force on purpose: the production side uses a prefix table and
    a closed form, and a second closed form here would share its blind spots. */
int modelOrdinal (int n) noexcept
{
    int count = 0;
    for (int m = 0; m < n; ++m)
        if (modelGated (m))
            ++count;
    return count;
}

int modelPitchOffset (int n) noexcept
{
    return pitchCycle[static_cast<std::size_t> (n % pitchLen)];
}

int modelVelocity (int n) noexcept
{
    return velCycle[static_cast<std::size_t> (n % velLen)];
}

int modelNote (int n) noexcept
{
    return noteAtDegree (modelOrdinal (n) % poolSize + modelPitchOffset (n));
}

// ── OBSERVATION ─────────────────────────────────────────────────────────────

/** What one step looked like IN THE EMITTED STREAM. Every field is derived from
    MIDI bytes and absolute sample positions — nothing is read back out of the
    engine — which is what makes the period searches below statements about the
    performance rather than about the model.

    ── TWO EQUALITIES, BECAUSE THERE ARE TWO PERIODS ───────────────────────────
    `sameLaneValues` compares what the three POLYMETRIC LANES contributed: the
    gate, the velocity, and the PITCH LANE'S OFFSET (recovered by subtracting the
    pool cursor back out). That tuple is a pure function of `n % 5`, `n % 7` and
    `n % 3`, so its period is lcm (5, 3, 7) = 105 — the polymeter claim.

    `sameEmission` additionally compares the MIDI NOTE, which folds in the gated
    pool cursor. That cursor is NOT 105-periodic: this GATE lane fires 3 times per
    5 steps over a 5-note pool, so `ordinal % 5` only realigns every 25 steps, and
    the full performance therefore repeats at lcm (25, 3, 7) = 525. Conflating the
    two is what a naive "the tuple at step 0 recurs at step 105" check does, and it
    is why that check fails on a correct engine. */
struct ObservedStep
{
    bool gated = false;
    int note = -1;
    int velocity = -1;
    int pitchOffset = noDegree; ///< Recovered: degree(note) − (gated ordinal % poolSize).

    bool sameLaneValues (const ObservedStep& other) const noexcept
    {
        return gated == other.gated && velocity == other.velocity && pitchOffset == other.pitchOffset;
    }

    bool sameEmission (const ObservedStep& other) const noexcept
    {
        return sameLaneValues (other) && note == other.note;
    }
};

/** Recovers one `ObservedStep` per step index in `[0, numSteps)` from a render.

    `outOfGrid` counts note-ons whose absolute sample is not a whole number of
    steps, and `duplicates` counts steps carrying more than one note-on; both must
    be zero before anything else here means anything. */
std::vector<ObservedStep> observedOf (const MidiRenderResult& render, int numSteps, int& outOfGrid, int& duplicates)
{
    std::vector<ObservedStep> steps (static_cast<std::size_t> (numSteps));
    outOfGrid = 0;
    duplicates = 0;

    for (const auto& event : render)
    {
        if (! event.message.isNoteOn ())
            continue;

        if (event.absoluteSample % stepSamples != 0)
        {
            ++outOfGrid;
            continue;
        }

        const auto index = event.absoluteSample / stepSamples;
        if (index < 0 || index >= numSteps)
            continue;

        auto& slot = steps[static_cast<std::size_t> (index)];
        if (slot.gated)
        {
            ++duplicates;
            continue;
        }

        slot.gated = true;
        slot.note = event.message.getNoteNumber ();
        slot.velocity = event.message.getVelocity ();
    }

    // The gated ordinal, counted from the OBSERVED gate set — so the recovered
    // pitch offset owes nothing to the model.
    int ordinal = 0;
    for (auto& slot : steps)
    {
        if (! slot.gated)
            continue;

        const int degree = degreeOfNote (slot.note);
        slot.pitchOffset = (degree == noDegree) ? noDegree : degree - (ordinal % poolSize);
        ++ordinal;
    }

    return steps;
}

// ── RIG SETUP ───────────────────────────────────────────────────────────────

/** The five-note pool described in the header. `sorted == asPlayed` (a stub pool
    has no arrival history — NotePool.h). */
PoolSnapshot testPool ()
{
    PoolSnapshot pool {};
    pool.size = static_cast<std::uint8_t> (poolSize);
    for (int i = 0; i < poolSize; ++i)
    {
        pool.sorted[static_cast<std::size_t> (i)] = poolPitches[i];
        pool.asPlayed[static_cast<std::size_t> (i)] = poolPitches[i];
    }
    return pool;
}

std::unique_ptr<SequencerRig> makeRig ()
{
    auto rig = std::make_unique<SequencerRig> (testSampleRate, testBlockSize);
    rig->patternDocument.setPool (testPool ());
    return rig;
}

std::vector<ScheduledCommand> startPlaying ()
{
    return { ScheduledCommand { 0, engineCommand (EngineCommandType::setTempoBpm, testBpm) },
             ScheduledCommand { 0, engineCommand (EngineCommandType::transportPlay) } };
}

MidiRenderConfig stepsConfig (int numSteps)
{
    auto config = MidiRenderConfig::samples (numSteps * stepSamples, testSampleRate, testBlockSize);
    config.numChannels = 1;
    config.eventReserve = 16384;
    return config;
}

/** Emitted note-ons in emission order — the "pitch sequence" the gated-cursor rule
    is about. */
std::vector<int> pitchSequenceOf (const MidiRenderResult& render, std::int64_t beforeSample)
{
    std::vector<int> pitches;
    for (const auto& event : render)
        if (event.message.isNoteOn () && event.absoluteSample < beforeSample)
            pitches.push_back (event.message.getNoteNumber ());
    return pitches;
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// B. Three lanes at three lengths, against the closed form
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/polymeter: lanes of length 5, 3 and 7 phase against the hand-computed closed form", "[unit]")
{
    // The pool must be degree-invertible, or `ObservedStep::pitchOffset` is a lie.
    for (int k = -39; k <= 40; ++k)
        REQUIRE (noteAtDegree (k) > noteAtDegree (k - 1));

    // 1050 = 10 x 105 = 2 x 525, so BOTH periods the analysis below establishes fit
    // at least twice over — a shift search needs room on both sides of the shift.
    constexpr int analysisSteps = 1050;
    constexpr int countedPrefix = 128; ///< The window the 77 literal is stated over.

    auto rig = makeRig ();
    auto& document = rig->patternDocument;

    document.beginTransaction ();
    document.setLaneLength (0, LaneId::gate, gateLen);
    document.setLaneLength (0, LaneId::pitch, pitchLen);
    document.setLaneLength (0, LaneId::vel, velLen);

    for (int s = 0; s < gateLen; ++s)
        document.setLaneValue (0, LaneId::gate, s, gateCycle[s]);
    for (int s = 0; s < pitchLen; ++s)
        document.setLaneValue (0, LaneId::pitch, s, pitchCycle[s]);
    for (int s = 0; s < velLen; ++s)
        document.setLaneValue (0, LaneId::vel, s, velCycle[s]);
    document.endTransaction ();

    const auto render = renderSequencer (*rig, stepsConfig (analysisSteps), startPlaying ());
    INFO (render.summary ());
    REQUIRE (render.isSampleSorted ());

    int outOfGrid = 0;
    int duplicates = 0;
    const auto observed = observedOf (render, analysisSteps, outOfGrid, duplicates);

    // Preconditions for the step-index recovery itself.
    REQUIRE (outOfGrid == 0);
    REQUIRE (duplicates == 0);

    // ── (1) THE GATED STEP SET ───────────────────────────────────────────────
    // Set equality, not a count: this catches an EXTRA fired step exactly as well
    // as a missing one, which a total alone would not.
    int gateMismatches = 0;
    int firstGateMismatch = -1;
    for (int n = 0; n < analysisSteps; ++n)
    {
        if (observed[static_cast<std::size_t> (n)].gated == modelGated (n))
            continue;

        ++gateMismatches;
        if (firstGateMismatch < 0)
            firstGateMismatch = n;
    }
    INFO ("first gate-set mismatch at step " << firstGateMismatch);
    REQUIRE (gateMismatches == 0);

    // ── (2) THE COUNT LITERAL ────────────────────────────────────────────────
    // Over the first 128 steps: 25 whole 5-cycles x 3 pulses = 75, plus n = 125
    // (125 % 5 == 0) and n = 127 (127 % 5 == 2) = 77. Derived by hand here and
    // recomputed from the model, so a formula that mirrored a bug cannot satisfy
    // both.
    int firedInPrefix = 0;
    int modelInPrefix = 0;
    for (int n = 0; n < countedPrefix; ++n)
    {
        firedInPrefix += observed[static_cast<std::size_t> (n)].gated ? 1 : 0;
        modelInPrefix += modelGated (n) ? 1 : 0;
    }
    REQUIRE (modelInPrefix == 25 * 3 + 2);
    REQUIRE (firedInPrefix == 77);

    // ── (3) PITCH AND VELOCITY AT EVERY GATED STEP ───────────────────────────
    int noteMismatches = 0;
    int velMismatches = 0;
    int offsetMismatches = 0;
    int firstBadStep = -1;
    int firstBadExpectedNote = 0;
    int firstBadActualNote = 0;

    for (int n = 0; n < analysisSteps; ++n)
    {
        const auto& step = observed[static_cast<std::size_t> (n)];
        if (! step.gated)
            continue;

        const bool badNote = step.note != modelNote (n);
        const bool badVel = step.velocity != modelVelocity (n);
        const bool badOffset = step.pitchOffset != modelPitchOffset (n);

        noteMismatches += badNote ? 1 : 0;
        velMismatches += badVel ? 1 : 0;
        offsetMismatches += badOffset ? 1 : 0;

        if ((badNote || badVel || badOffset) && firstBadStep < 0)
        {
            firstBadStep = n;
            firstBadExpectedNote = modelNote (n);
            firstBadActualNote = step.note;
        }
    }

    INFO ("first mismatch at step " << firstBadStep << ": expected note " << firstBadExpectedNote << ", got "
                                    << firstBadActualNote);
    REQUIRE (noteMismatches == 0);
    REQUIRE (velMismatches == 0);
    REQUIRE (offsetMismatches == 0);

    // ── (4) THE NEGATIVE CONTROL ─────────────────────────────────────────────
    // A non-polymetric reading would index the PITCH lane's raw storage at
    // `n % maxLaneLength` instead of `n % length`. Beyond index 2 that storage
    // holds `laneDefault (pitch)` == 0, so the two readings disagree constantly —
    // but only if the lane's own length is actually being honoured.
    int differsFromNonPolymetric = 0;
    for (int n = 0; n < analysisSteps; ++n)
    {
        const auto& step = observed[static_cast<std::size_t> (n)];
        if (! step.gated)
            continue;

        const int nonPolymetric = (n % 16 < pitchLen) ? pitchCycle[static_cast<std::size_t> (n % 16)] : 0;
        if (step.pitchOffset != nonPolymetric)
            ++differsFromNonPolymetric;
    }
    REQUIRE (differsFromNonPolymetric > 0);

    // ── (5) THE COMBINED LANE PERIOD IS EXACTLY 105 ──────────────────────────
    // THE "phases correctly" assertion, and the FORM of it is the whole point.
    //
    // The obvious phrasing — "step 0 and step 105 look the same, and no step in
    // between does" — is ARITHMETICALLY FALSE for this configuration and would
    // have to be weakened to pass. Step 42 has `42 % 5 == 2` (gate on),
    // `42 % 3 == 0` and `42 % 7 == 0`, so it presents exactly step 0's tuple. One
    // step's tuple recurring early says nothing about the SEQUENCE's period, and
    // it is the sequence's period that the word "polymeter" names.
    //
    // So the claim tested here is the stronger and correct one: the sequence is
    // invariant under a shift of 105 and under NO SMALLER SHIFT, established by
    // trying every candidate exhaustively. That rules out a lane silently reading
    // at some other length (any wrong length divides or fails to divide 105 and
    // moves the answer), which the single-step phrasing would not.
    const auto laneInvariantUnderShift = [&observed, analysisSteps] (int shift)
    {
        for (int n = 0; n + shift < analysisSteps; ++n)
            if (! observed[static_cast<std::size_t> (n)].sameLaneValues (
                    observed[static_cast<std::size_t> (n + shift)]))
                return false;
        return true;
    };

    REQUIRE (laneInvariantUnderShift (105));
    REQUIRE (observed[0].sameLaneValues (observed[105]));
    REQUIRE (observed[0].sameLaneValues (observed[210]));

    int smallestLanePeriod = 0;
    for (int shift = 1; shift <= 105; ++shift)
    {
        if (laneInvariantUnderShift (shift))
        {
            smallestLanePeriod = shift;
            break;
        }
    }

    INFO ("smallest shift the LANE VALUES are invariant under");
    REQUIRE (smallestLanePeriod == 105);
    REQUIRE (smallestLanePeriod == gateLen * pitchLen * velLen); // lcm(5,3,7), coprime

    // ── (6) …AND THE PERFORMANCE ITSELF REPEATS AT 525, NOT AT 105 ───────────
    // Worth pinning explicitly, because it is the counter-intuitive consequence of
    // the gated-cursor rule and a future reader WILL assume the two numbers are the
    // same. The emitted MIDI note also depends on the pool cursor, which advances 3
    // places per 5 steps over a 5-note pool and therefore only realigns every 25
    // steps — so the audible loop is lcm (25, 3, 7) = 525 steps even though the lane
    // data repeats after 105. An engine that folded PROB or block carving into the
    // cursor would move this number; that is exactly what `gatedOrdinal`'s closed
    // form exists to prevent.
    const auto emissionInvariantUnderShift = [&observed, analysisSteps] (int shift)
    {
        for (int n = 0; n + shift < analysisSteps; ++n)
            if (! observed[static_cast<std::size_t> (n)].sameEmission (observed[static_cast<std::size_t> (n + shift)]))
                return false;
        return true;
    };

    int smallestEmissionPeriod = 0;
    for (int shift = 1; shift <= 525; ++shift)
    {
        if (emissionInvariantUnderShift (shift))
        {
            smallestEmissionPeriod = shift;
            break;
        }
    }

    INFO ("smallest shift the EMITTED NOTES are invariant under");
    REQUIRE (smallestEmissionPeriod == 525);
    REQUIRE (smallestEmissionPeriod == 25 * pitchLen * velLen); // lcm(25,3,7)
    // 525 = 5 x 105: the audible loop is a whole number of lane loops, but FIVE of
    // them, because the pool cursor needs 25 steps to come back to the same degree.
    REQUIRE (smallestEmissionPeriod % smallestLanePeriod == 0);
    REQUIRE (smallestEmissionPeriod == 5 * smallestLanePeriod);

    NoteLifecycleTracker tracker;
    tracker.observeAll (render);
    INFO (tracker.describe ());
    REQUIRE (tracker.noteOnsSeen () > 100);
    REQUIRE (tracker.orphanNoteOffs () == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// B (cont). The gated-cursor rule: a rest does not consume a pool note
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/polymeter: a rest thins the rhythm without consuming a pool note", "[unit]")
{
    // THE USER DECISION `PatternSnapshot::gatedOrdinal` exists to implement, and the
    // one place it is directly observable. Two readings of "which pool note does
    // step n play" are possible and they sound completely different:
    //
    //   RAW-INDEX CURSOR  poolIndex = n % poolSize. A rest SKIPS a pool note: the
    //                     note that would have sounded there is simply never heard,
    //                     so the arpeggio develops holes in its pitch sequence.
    //   GATED CURSOR      poolIndex = (gated steps before n) % poolSize. A rest
    //                     thins the RHYTHM and the pitch sequence continues
    //                     unbroken from where it left off. This is what hardware
    //                     arpeggiators do, and it is what ARPBOX does.
    //
    // So the assertion is about the pitch SEQUENCE, not about per-step pitches:
    // punching a gate off must leave the emitted note sequence
    // pool[0], pool[1], pool[2], … intact, with only the timings changing.
    constexpr int analysisSteps = 60;
    constexpr std::int64_t analysisEnd = analysisSteps * stepSamples;

    const auto configureBase = [] (PatternDocument& document)
    {
        document.beginTransaction ();
        document.setLaneLength (0, LaneId::gate, gateLen);
        for (int s = 0; s < gateLen; ++s)
            document.setLaneValue (0, LaneId::gate, s, gateCycle[s]);
        document.endTransaction ();
    };

    // ── Baseline: G = {1,0,1,1,0} ────────────────────────────────────────────
    auto baseRig = makeRig ();
    configureBase (baseRig->patternDocument);
    const auto baseRender = renderSequencer (*baseRig, stepsConfig (analysisSteps), startPlaying ());
    const auto basePitches = pitchSequenceOf (baseRender, analysisEnd);

    INFO (baseRender.summary ());
    REQUIRE (basePitches.size () > 20u);

    for (std::size_t k = 0; k < basePitches.size (); ++k)
    {
        INFO ("baseline note " << k);
        REQUIRE (basePitches[k] == static_cast<int> (poolPitches[k % poolSize]));
    }

    // ── One gate punched off: G = {1,0,1,0,0} ────────────────────────────────
    auto thinnedRig = makeRig ();
    configureBase (thinnedRig->patternDocument);
    REQUIRE (thinnedRig->patternDocument.setLaneValue (0, LaneId::gate, 3, 0));

    const auto thinnedRender = renderSequencer (*thinnedRig, stepsConfig (analysisSteps), startPlaying ());
    const auto thinnedPitches = pitchSequenceOf (thinnedRender, analysisEnd);

    INFO (thinnedRender.summary ());

    // Non-vacuity: the edit really did remove trigs (3 per 5-cycle became 2).
    REQUIRE (thinnedPitches.size () < basePitches.size ());
    REQUIRE (thinnedPitches.size () > 10u);

    // THE assertion: the pitch sequence is unbroken.
    for (std::size_t k = 0; k < thinnedPitches.size (); ++k)
    {
        INFO ("thinned note " << k);
        REQUIRE (thinnedPitches[k] == static_cast<int> (poolPitches[k % poolSize]));
    }

    // …and the negative control, stated as the sound a raw-index cursor would make.
    // With G = {1,0,1,0,0} the gated steps are 0, 2, 5, 7, 10, 12, … so `n % 5`
    // would alternate between pool degrees 0 and 2 forever: 60, 65, 60, 65, …
    REQUIRE (thinnedPitches[0] == 60);
    REQUIRE (thinnedPitches[1] == 62); // gated cursor — NOT 65
    REQUIRE (thinnedPitches[1] != 65); // raw-index cursor would say 65
    REQUIRE (thinnedPitches[2] == 65);
    REQUIRE (thinnedPitches[3] == 67);
    REQUIRE (thinnedPitches[4] == 71);
    REQUIRE (thinnedPitches[5] == 60);

    // The timings DID change, which is what "thins the rhythm" means.
    const auto baseSteps = pitchSequenceOf (baseRender, analysisEnd).size ();
    REQUIRE (baseSteps != thinnedPitches.size ());
}

// ─────────────────────────────────────────────────────────────────────────────
// B (cont). Clock division: GATE divides the TRIGGER RATE, everything else holds
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/polymeter: GATE division is a clock divider and other lanes are value-hold", "[unit]")
{
    // The asymmetry documented on `isLaneTick`, and the reason it is documented: a
    // lane's `division` has two defensible readings that sound completely
    // different, and reading GATE as value-hold would REPEAT each gate value
    // instead of halving the trigger rate.

    SECTION ("GATE at division 2 halves the trigger rate and never double-triggers")
    {
        constexpr int analysisSteps = 64;
        constexpr int gateDivision = 2;

        auto rig = makeRig ();
        auto& document = rig->patternDocument;

        document.beginTransaction ();
        document.setLaneLength (0, LaneId::gate, 8);
        document.setLaneDivision (0, LaneId::gate, gateDivision);
        for (int s = 0; s < 8; ++s)
            document.setLaneValue (0, LaneId::gate, s, 1);
        document.endTransaction ();

        const auto render = renderSequencer (*rig, stepsConfig (analysisSteps), startPlaying ());

        int outOfGrid = 0;
        int duplicates = 0;
        const auto observed = observedOf (render, analysisSteps, outOfGrid, duplicates);

        INFO (render.describe (8));
        REQUIRE (outOfGrid == 0);
        REQUIRE (duplicates == 0); // NOT double-triggered

        // Exactly the even steps fire, so the rate is HALVED, not doubled.
        int fired = 0;
        int wrongParity = 0;
        for (int n = 0; n < analysisSteps; ++n)
        {
            const bool gated = observed[static_cast<std::size_t> (n)].gated;
            fired += gated ? 1 : 0;
            if (gated != (n % gateDivision == 0))
                ++wrongParity;
        }

        REQUIRE (wrongParity == 0);
        REQUIRE (fired == analysisSteps / gateDivision);
        REQUIRE (! observed[1].gated);
        REQUIRE (! observed[3].gated);
        REQUIRE (observed[0].gated);
        REQUIRE (observed[2].gated);

        // The gated ordinal advances once per FIRED step, so the pool walks at half
        // the step rate too: step n plays pool degree (n / 2) % 5.
        int poolMismatches = 0;
        for (int n = 0; n < analysisSteps; n += gateDivision)
        {
            if (observed[static_cast<std::size_t> (n)].note !=
                static_cast<int> (poolPitches[static_cast<std::size_t> ((n / gateDivision) % poolSize)]))
                ++poolMismatches;
        }
        REQUIRE (poolMismatches == 0);
    }

    SECTION ("VEL at division 4 holds each value across four gate steps")
    {
        constexpr int analysisSteps = 32;
        constexpr int velDivision = 4;
        constexpr int heldVelocities[4] = { 20, 40, 60, 80 };

        auto rig = makeRig ();
        auto& document = rig->patternDocument;

        document.beginTransaction ();
        document.setLaneLength (0, LaneId::vel, 4);
        document.setLaneDivision (0, LaneId::vel, velDivision);
        for (int s = 0; s < 4; ++s)
            document.setLaneValue (0, LaneId::vel, s, heldVelocities[s]);
        document.endTransaction ();

        const auto render = renderSequencer (*rig, stepsConfig (analysisSteps), startPlaying ());

        int outOfGrid = 0;
        int duplicates = 0;
        const auto observed = observedOf (render, analysisSteps, outOfGrid, duplicates);

        INFO (render.describe (8));
        REQUIRE (outOfGrid == 0);
        REQUIRE (duplicates == 0);

        // THE contrast with GATE: a division on VEL does not gate anything. Every
        // step still fires; only the VALUE is held.
        int fired = 0;
        int velMismatches = 0;
        for (int n = 0; n < analysisSteps; ++n)
        {
            const auto& step = observed[static_cast<std::size_t> (n)];
            if (! step.gated)
                continue;

            ++fired;
            if (step.velocity != heldVelocities[(n / velDivision) % 4])
                ++velMismatches;
        }

        REQUIRE (fired == analysisSteps); // NOT a quarter of them
        REQUIRE (velMismatches == 0);

        // Spelled out for the first cycle, so the "held across four" shape is
        // legible rather than only implied by the formula above.
        REQUIRE (observed[0].velocity == 20);
        REQUIRE (observed[3].velocity == 20);
        REQUIRE (observed[4].velocity == 40);
        REQUIRE (observed[7].velocity == 40);
        REQUIRE (observed[8].velocity == 60);
        REQUIRE (observed[12].velocity == 80);
        REQUIRE (observed[16].velocity == 20); // wrapped at length 4 x division 4
    }
}
