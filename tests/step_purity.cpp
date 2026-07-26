// ─────────────────────────────────────────────────────────────────────────────
// step_purity — the BEHAVIOURAL half of issue #53's enforcement.
//
// ── WHAT IS BEING PROTECTED, AND WHY IT IS NOT OBVIOUS ──────────────────────
// `SequencerProcessor::cutoffForSamePitch` schedules a note-off by PREDICTING what
// steps k+1, k+2, … will emit (the issue #46 fix). A prediction is only sound if
// asking about a step gives the same answer as eventually emitting it — i.e. if
// `evaluateStep (snapshot, patternIndex, stepIndex, runtime)` is a PURE function of
// those arguments. If it ever consults a running RNG cursor, an accumulator, or a
// `PRE`/`NEI` result cached by the emitting walk, then the answer starts depending
// on the ORDER and COUNT of calls, the lookahead diverges from emission, and the
// note-off placement becomes a function of how the timeline was carved into
// blocks. That is exactly the #36 / #46 / #48 failure class, resurrected one level
// up, and Phase 6 spent most of its budget there.
//
// ── HOW THIS FILE DIVIDES THE WORK WITH THE COMPILER ────────────────────────
// The compile-time half is the SIGNATURE: `evaluateStep` is a free function at
// namespace scope, so no member of `SequencerProcessor` — the obvious home for a
// Phase 7.1 RNG cursor — is nameable from inside it. That half is checked by the
// build, needs no test, and cannot be "temporarily" bypassed.
//
// This file covers what a type system cannot see: mutable state parked SOMEWHERE
// ELSE. A file-scope `static` in SequencerProcessor.cpp, a function-local
// `static`, a thread_local, a memo keyed on the snapshot pointer — none of those
// are member state, all of them compile, and every one of them makes the result
// depend on call history. So the strategy here is not "call it and check the
// value" (that would only re-derive the pattern core, which pattern_polymeter.cpp
// already does against a hand-written closed form). It is:
//
//     take an IN-ORDER reference sweep, then demand the identical answers back
//     under call orders no cursor could survive.
//
// Six perturbations, each defeating a different shape of impurity:
//   1. REPEATED     — same index four times. Any per-call advance reddens.
//   2. REVERSED     — descending sweep. A cursor keyed on call count reddens.
//   3. SHUFFLED     — a seeded permutation. Defeats a cursor that happens to be
//                     symmetric under reversal.
//   4. INTERLEAVED  — round-robin across patterns. A cache with one slot (a
//                     plausible `PRE` memo) reddens; a per-pattern one survives 1–3.
//   5. LOOKAHEAD    — literally `cutoffForSamePitch`'s access pattern: peek at
//                     k+1..k+5, THEN evaluate k. This is the production sequence,
//                     so it is the one that must never drift.
//   6. RUNTIME-INTERLEAVED (Phase 7.1) — the SAME index asked alternately with
//                     `fillHeld` false and true, each answer required to match its
//                     OWN reference sweep. This is what turns Phase 7.1's signature
//                     widening from an unchecked widening into a checked one: a
//                     fourth parameter that was quietly ignored, or a FILL result
//                     cached across calls, reddens here and nowhere else.
// Plus a SECOND, independently built byte-identical snapshot, which reddens a memo
// keyed on the snapshot's address rather than its contents.
//
// ── ANTI-VACUITY, AND WHY IT HAD TO GROW IN PHASE 7.1 ───────────────────────
// A purity test over an all-rest pattern proves nothing: every call returns the
// same empty emission, so every perturbation trivially "agrees". The fixture
// therefore asserts up front that the reference sweep contains gated steps, that
// it contains rests, and that it contains several DISTINCT emissions — and the
// seeded direction modes (`walk`, `randomNoRepeat`) are in the fixture precisely
// because they are the modes most likely to acquire a stream cursor later.
//
// THE SAME ARGUMENT APPLIES ONE LAYER DOWN, AND UNTIL PHASE 7.1 IT WAS UNMET.
// The Phase-6 fixture left PROB at its default 100 and COND at `none` on every
// step, so `probabilityPasses` returned through its `p >= 100` short-circuit
// WITHOUT EVER HASHING and `conditionPasses` returned through its `none` arm
// without reading anything. All five perturbations swept straight past the whole
// of L2. An RNG cursor parked in `StepLogic.cpp` — risk-register item 1, the
// single most likely Phase 7.1 mistake — would have left this file green.
// (Demonstrated, not assumed: a deliberate `static int calls` impurity placed
// past the short-circuit was invisible to the old fixture and reddens the
// SHUFFLED / INTERLEAVED sections with the fixture below.)
//
// So the fixture now carries sub-100 PROB, several A:B conditions, an anchored
// PRE chain, NEI steps and FILL-conditioned steps — and the anti-vacuity case
// asserts, by counting, that each of those paths is actually TAKEN: that steps are
// suppressed by COND, that steps are suppressed by PROB, and that some steps
// answer differently for the two values of `fillHeld`.
//
// AND THE SAME ARGUMENT AGAIN FOR PHASE 7.2, ONE LAYER FURTHER DOWN. The 7.1
// fixture left RATCHET at its default 1, MICRO at its default 0 and the project
// swing straight, and each of those defaults is a short-circuit of its own:
// `ratchetChildCount` returns 1, `ratchetChildPasses` IS NEVER CALLED AT ALL
// (child 0 is deliberately not asked — see the note on it), `swingShiftSteps`
// returns through its even/straight arm, and the composed displacement is bit-zero
// so the `jlimit` has nothing to clamp. All six perturbations therefore swept past
// the per-child probability path (`ratchetChildPasses` → `rng::subStepHash`), past
// the per-child velocity ramp, and past the displacement composition entirely. A
// cursor parked in the per-child roll — RISK-REGISTER ITEM 4, and the single most
// likely way to break Phase 7 — would have left this file green, for the plain
// reason that a stream pulled once per child cannot be caught out on a fixture
// that never has a second child.
//
// So the fixture now carries RATCHET spanning 1..8, MICRO straddling zero and
// reaching both ±50, a non-straight project swing and a non-flat ratchet velocity
// ramp — and the anti-vacuity case counts multi-note emissions, FULL eight-child
// emissions, displaced emissions, emissions whose displacement SATURATED the
// ±`maxSubStepShiftSteps` clamp, and emissions carrying two children at different
// velocities. The before/after fails-without demonstration for exactly that
// per-child impurity is run by the parent agent rather than asserted here.
// ─────────────────────────────────────────────────────────────────────────────

#include "engine/sequencer/PatternSnapshot.h"
#include "engine/sequencer/PatternTypes.h"
#include "engine/sequencer/SequencerProcessor.h"
#include "engine/sequencer/StepLogic.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <tuple>
#include <vector>

using arpbox::engine::buildPatternSnapshot;
using arpbox::engine::conditionPasses;
using arpbox::engine::DirectionMode;
using arpbox::engine::evaluateStep;
using arpbox::engine::LaneId;
using arpbox::engine::laneOf;
using arpbox::engine::maxRatchetChildren;
using arpbox::engine::maxSubStepShiftSteps;
using arpbox::engine::PatternSetState;
using arpbox::engine::PatternSnapshot;
using arpbox::engine::PoolSnapshot;
using arpbox::engine::probabilityPasses;
using arpbox::engine::StepEmission;
using arpbox::engine::StepRuntime;
using arpbox::engine::TrigCondition;

namespace
{
/** Patterns exercised. Six is enough to make the INTERLEAVED case meaningful
    without making the sweep slow. */
constexpr int numTestPatterns = 6;

/** The fourth `evaluateStep` input (Phase 7.1's signature widening).

    PERTURBATIONS 1–5 HOLD IT CONSTANT, deliberately: their contract is "same
    inputs, any call order, same answer", so the runtime has to be fixed for the
    call-order variation to be the only variable. Perturbation 6 is the one that
    varies it — and it varies it against TWO reference sweeps, one per value, so it
    is still an equality against a known answer rather than a self-comparison. */
constexpr StepRuntime fixtureRuntime {};

/** The other value of the flag — pad 16 held. See perturbation 6. */
constexpr StepRuntime fillHeldRuntime { true };

/** How far the retrigger lookahead reaches — the depth perturbations 5 and 6
    reproduce.

    ── ISSUE #69 IS CLOSED HERE ────────────────────────────────────────────────
    This was `constexpr std::int64_t retriggerLookaheadSteps = 5;`, a HAND-COPIED
    literal, because `maxRetriggerLookaheadSteps` was a PRIVATE member constant of
    `SequencerProcessor` that no test could name. #69 predicted exactly what then
    happened: Phase 7.2 re-derived the constant from the lane ranges (LEN's 400 %
    ceiling + the last ratchet child's offset + two sub-step displacements) and it
    moved 5 -> 7, which would have left the sections below exercising a
    TWO-STEP-SHALLOWER access pattern than production while staying green.

    Phase 7.2 lifted the constant to NAMESPACE SCOPE for this reason, so this is
    now a reference rather than a copy and the divergence #69 named is no longer
    writable. If the geometry moves again, these sections follow automatically. */
constexpr auto retriggerLookaheadSteps = static_cast<std::int64_t> (arpbox::engine::maxRetriggerLookaheadSteps);

/** Step indices swept per pattern: `[firstStep, lastStep]`, deliberately spanning
    NEGATIVE indices (the polymeter floor-mod path) and several pattern loops (the
    gated-ordinal path, which is where a counter would be tempting). */
constexpr std::int64_t firstStep = -37;
constexpr std::int64_t lastStep = 260;

/** One (pattern, step) query and the answer it must always give. */
struct Sample
{
    int patternIndex = 0;
    std::int64_t stepIndex = 0;
    StepEmission emission {};
};

bool sameEmission (const StepEmission& a, const StepEmission& b) noexcept
{
    // EVERY FIELD, INCLUDING THE WHOLE NOTE LIST. A purity guard that compared only
    // some fields would let an impurity hide in the rest, and Phase 7.2 added four
    // fields per note plus the step's displacement. `noteCount` is compared first so
    // the loop below cannot read past either list's live region.
    if (! (a.gate == b.gate && a.channel == b.channel && a.noteCount == b.noteCount && a.shiftSteps == b.shiftSteps))
        return false;

    for (int i = 0; i < a.noteCount; ++i)
    {
        const auto& x = a.notes[static_cast<std::size_t> (i)];
        const auto& y = b.notes[static_cast<std::size_t> (i)];

        if (! (x.note == y.note && x.velocity == y.velocity && x.positionInStep == y.positionInStep &&
               x.gateFractionOfStep == y.gateFractionOfStep && x.provenance == y.provenance))
            return false;
    }

    return true;
}

/** A deterministic permutation of `[0, n)` — a 64-bit LCG walked over an index
    array. Seeded and self-contained: no `std::random_device`, no `std::shuffle`
    whose implementation is free to differ between standard libraries (a test that
    permutes differently on another toolchain still passes, but it should permute
    the SAME way when someone re-runs it to debug a failure). */
std::vector<std::size_t> seededPermutation (std::size_t n, std::uint64_t seed)
{
    std::vector<std::size_t> order (n);

    for (std::size_t i = 0; i < n; ++i)
        order[i] = i;

    std::uint64_t s = seed;

    for (std::size_t i = n; i > 1; --i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::size_t j = static_cast<std::size_t> ((s >> 33) % i);
        const std::size_t tmp = order[i - 1];
        order[i - 1] = order[j];
        order[j] = tmp;
    }

    return order;
}

/** THE FIXTURE. Six patterns that between them light up every input `evaluateStep`
    reads: unequal lane lengths and divisions (polymeter), gate rests, non-zero
    PITCH / OCT / VEL / LEN lanes, sub-100 PROB and every condition FAMILY (Phase
    7.1), RATCHET spanning 1..8 and MICRO spanning ±50 against a non-straight project
    swing (Phase 7.2), five direction modes including BOTH seeded ones, and a pool
    whose size is not the default. Written as one function so the two snapshot builds
    below are provably from identical state.

    EVERY LANE LENGTH HERE IS CHOSEN TO PHASE AGAINST THE OTHERS, never to line up:
    a lane whose length shares a factor with GATE's repeats in lockstep with the
    rhythm, so its values only ever land on the same gate phases and most of its
    range is never asked about at a gated step. */
PatternSetState purityFixtureState ()
{
    PatternSetState state {};

    state.gridStepPpq = 0.25;
    state.outputChannel = 3;

    // ── PHASE 7.2: THE TWO PROJECT-LEVEL FEEL FIELDS ──────────────────────────
    // Both are set here, on the state, exactly as `gridStepPpq` and `outputChannel`
    // are — they are project-level, not per pattern (see the swing note in
    // PatternTypes.h), so every pattern in the fixture inherits them.
    //
    // 62 % SWING RATHER THAN 50: at 50 `swingShiftSteps` returns EXACTLY 0.0 and the
    // displacement composition in `evaluateStep` degenerates to MICRO alone, which
    // means neither the odd/even `stepFloorMod` arm nor the composed `jlimit` is
    // exercised. 62 delays every odd GLOBAL step by 0.24 steps — deliberately not a
    // value that any MICRO entry below can cancel to exactly zero, so "displaced" and
    // "not displaced" stay distinguishable.
    state.swingPct = 62.0;

    // A DECAYING ramp rather than a rising one: with the fixture's VEL lane reaching
    // 125, a positive ramp would push most children into the 127 clamp and the
    // per-child velocities would come back FLAT — which is what the ramp counter in
    // the anti-vacuity case is there to detect, so it must not be manufactured by the
    // fixture's own choice of sign.
    state.ratchetVelocityRampPct = -35.0;

    PoolSnapshot pool {};
    pool.size = 5;
    const std::uint8_t pitches[5] = { 60, 63, 67, 70, 74 };

    for (std::size_t i = 0; i < 5; ++i)
    {
        pool.sorted[i] = pitches[i];
        // A DIFFERENT arrival order, so `asPlayed` vs `sorted` is an observable
        // difference and the `asPlayed` pattern below is not a duplicate of `up`.
        pool.asPlayed[i] = pitches[(i * 3 + 2) % 5];
    }

    state.pool = pool;

    const DirectionMode modes[numTestPatterns] = {
        DirectionMode::up,
        DirectionMode::converge,
        DirectionMode::spiral,
        DirectionMode::asPlayed,
        DirectionMode::walk,           // seeded — the likeliest future cursor site
        DirectionMode::randomNoRepeat, // seeded — likewise
    };

    // Lane lengths that are mutually coprime-ish, so the lanes phase against each
    // other across the swept range instead of repeating in lockstep.
    const int gateLengths[numTestPatterns] = { 16, 5, 7, 12, 9, 13 };
    const int pitchLengths[numTestPatterns] = { 3, 16, 5, 7, 11, 4 };
    const int gateDivisions[numTestPatterns] = { 1, 1, 2, 1, 3, 1 };

    for (int p = 0; p < numTestPatterns; ++p)
    {
        auto& pattern = state.patterns[static_cast<std::size_t> (p)];

        pattern.direction = modes[p];
        pattern.masterSeed = 0x9E3779B97F4A7C15ULL + static_cast<std::uint64_t> (p) * 0x1234567ULL;

        for (int lane = 0; lane < arpbox::engine::numLanes; ++lane)
        {
            auto& laneState = pattern.lanes[static_cast<std::size_t> (lane)];
            laneState.length = 16;
            laneState.division = 1;

            for (int step = 0; step < arpbox::engine::maxSteps; ++step)
                laneState.values[static_cast<std::size_t> (step)] =
                    arpbox::engine::laneDefault (static_cast<LaneId> (lane));
        }

        auto& gate = laneOf (pattern, LaneId::gate);
        gate.length = static_cast<std::uint8_t> (gateLengths[p]);
        gate.division = static_cast<std::uint8_t> (gateDivisions[p]);

        // RESTS MATTER: the gated-ordinal table is what keeps the pool cursor pure,
        // and it is only exercised when some steps are off.
        for (int step = 0; step < gateLengths[p]; ++step)
            gate.values[static_cast<std::size_t> (step)] = ((step + p) % 4 == 3) ? 0 : 1;

        auto& pitch = laneOf (pattern, LaneId::pitch);
        pitch.length = static_cast<std::uint8_t> (pitchLengths[p]);

        for (int step = 0; step < pitchLengths[p]; ++step)
            pitch.values[static_cast<std::size_t> (step)] = static_cast<std::int16_t> ((step % 5) - 2);

        auto& oct = laneOf (pattern, LaneId::oct);
        oct.length = 4;
        oct.values[1] = 1;
        oct.values[3] = -1;

        auto& vel = laneOf (pattern, LaneId::vel);
        vel.length = 6;
        vel.division = 2;

        for (int step = 0; step < 6; ++step)
            vel.values[static_cast<std::size_t> (step)] = static_cast<std::int16_t> (40 + step * 17);

        auto& len = laneOf (pattern, LaneId::len);
        len.length = 5;

        for (int step = 0; step < 5; ++step)
            len.values[static_cast<std::size_t> (step)] = static_cast<std::int16_t> (25 + step * 60);

        // ── PHASE 7.1: THE L2 LANES ──────────────────────────────────────────
        // Without these the fixture never leaves the two short-circuits
        // (`p >= 100` in `probabilityPasses`, `none` in `conditionPasses`) and the
        // perturbations below sweep straight past all of step logic. See the
        // anti-vacuity note in the file header — this block is the reason that
        // note had to be rewritten.
        //
        // PROB: some steps at 100 (so the short-circuit path stays exercised too)
        // and some below it (so the HASH path is taken). Length 7 against GATE's
        // 5/7/9/12/13/16 so the two phase against each other.
        auto& prob = laneOf (pattern, LaneId::prob);
        prob.length = 7;
        const std::int16_t probValues[7] = { 100, 100, 60, 100, 25, 100, 85 };

        for (int step = 0; step < 7; ++step)
            prob.values[static_cast<std::size_t> (step)] = probValues[step];

        // MOD A: NEI reads `>= 64`, so this lane must straddle the threshold or
        // both NEI arms below would be constants.
        auto& modA = laneOf (pattern, LaneId::modA);
        modA.length = 5;
        const std::int16_t modAValues[5] = { 0, 64, 127, 30, 90 };

        for (int step = 0; step < 5; ++step)
            modA.values[static_cast<std::size_t> (step)] = modAValues[step];

        // COND: one of every FAMILY, not one of every ordinal — the 39-entry truth
        // table lives in step_conditions.cpp. What this file needs is that each
        // KIND of evaluation path is entered: an A:B floor-mod, a loop-index
        // compare, a MOD A read, both FILL arms, and a PRE chain that actually
        // walks backwards through `previousGatedOffset`.
        //
        // `pre` at index 8 is ANCHORED (indices 0-7 of this lane are not PRE), so
        // the backward walk terminates on a real anchor rather than on the depth
        // budget — the anchored case is the one whose result depends on ANOTHER
        // step's outcome, and therefore the one a cache would be written for.
        //
        // Length 11 is coprime with every GATE length in the fixture, so a given
        // COND value lands on different gate phases as the sweep advances.
        auto& cond = laneOf (pattern, LaneId::cond);
        cond.length = 11;
        const TrigCondition condValues[11] = { TrigCondition::none,    TrigCondition::none,     TrigCondition::ab1of2,
                                               TrigCondition::none,    TrigCondition::notFirst, TrigCondition::nei,
                                               TrigCondition::fill,    TrigCondition::none,     TrigCondition::pre,
                                               TrigCondition::notFill, TrigCondition::ab3of4 };

        for (int step = 0; step < 11; ++step)
            cond.values[static_cast<std::size_t> (step)] = static_cast<std::int16_t> (condValues[step]);

        // ── PHASE 7.2: THE SUB-STEP LANES ────────────────────────────────────
        // Without these two the fixture never leaves three more short-circuits —
        // `ratchetChildCount` returns 1, `ratchetChildPasses` is never called at
        // all, and the composed displacement is bit-zero — so the perturbations
        // sweep past the per-child roll, the per-child velocity ramp and the
        // displacement composition. See the Phase 7.2 paragraph in the file header;
        // this block is the reason it had to be written.
        //
        // RATCHET: length 17, which is COPRIME WITH EVERY GATE LENGTH IN THE
        // FIXTURE ({16, 5, 7, 12, 9, 13} — 17 is prime and larger than all of
        // them) and with PROB's 7 and COND's 11. That is what makes the child count
        // phase against the gate instead of repeating in lockstep with it, so a
        // given RATCHET value lands on gated and rested steps, on 100 % and sub-100
        // PROB steps, and on every condition family as the sweep advances.
        //
        // THE VALUES SPAN 1..8 AND CONTAIN BOTH ENDS OFTEN. `1` is the pre-7.2
        // default and must stay represented (it is the path every existing golden
        // takes); `maxRatchetChildren` appears six times out of seventeen so the
        // FULL eight-child emission — the one the fixed-size `notes` array is sized
        // for, and the one a per-child cursor has the most room to drift in — is
        // reached often enough for the counter below to have a floor worth setting.
        auto& ratchet = laneOf (pattern, LaneId::ratchet);
        ratchet.length = 17;
        const std::int16_t ratchetValues[17] = { 1, 8, 4, 8, 2, 1, 8, 6, 3, 8, 1, 5, 8, 2, 7, 1, 8 };

        for (int step = 0; step < 17; ++step)
            ratchet.values[static_cast<std::size_t> (step)] = ratchetValues[step];

        // MICRO: length 19 (prime, coprime with every other lane length here), and
        // the values STRADDLE ZERO and reach BOTH ±50 — §12.1's full range.
        //
        // REACHING THE ENDS IS THE POINT, not decoration. +50 composed with the
        // 62 % swing's +0.24 is +0.74, which SATURATES the ±`maxSubStepShiftSteps`
        // clamp; -50 on an even (unswung) step sits exactly ON the negative bound.
        // The clamp is the single enforcement point for the bound the step walk's
        // scan widening is derived from, and a fixture that never reached it would
        // leave the one `jlimit` in the emission core untested by every perturbation
        // in this file. Zeros are kept in the list so "no displacement at all"
        // remains a reachable answer and the displaced counter is not vacuous.
        auto& micro = laneOf (pattern, LaneId::micro);
        micro.length = 19;
        const std::int16_t microValues[19] = { 0,   25, -50, 10, 50, -20, 35, -50, 0, 50,
                                               -15, 40, -35, 50, 5,  -50, 20, -25, 45 };

        for (int step = 0; step < 19; ++step)
            micro.values[static_cast<std::size_t> (step)] = microValues[step];
    }

    return state;
}

/** The IN-ORDER reference: every (pattern, step) query, ascending, each asked
    exactly once, on a freshly built snapshot. Everything else is compared to this.

    `runtime` is a PARAMETER rather than a constant because perturbation 6 needs two
    of these — one per value of `fillHeld` — and comparing a `fillHeld == true` call
    against a `fillHeld == false` reference would assert the opposite of the
    contract. */
std::vector<Sample> referenceSweep (const PatternSnapshot& snapshot, StepRuntime runtime = fixtureRuntime)
{
    std::vector<Sample> samples;
    samples.reserve (static_cast<std::size_t> (numTestPatterns * (lastStep - firstStep + 1)));

    for (int p = 0; p < numTestPatterns; ++p)
        for (std::int64_t step = firstStep; step <= lastStep; ++step)
            samples.push_back ({ p, step, evaluateStep (snapshot, p, step, runtime) });

    return samples;
}
} // namespace

TEST_CASE ("sequencer/step-purity: the fixture actually exercises the emission core", "[unit]")
{
    // ANTI-VACUITY. Every assertion in this file is an equality against the
    // reference sweep, so a fixture that emits nothing (or emits one repeated
    // value) would make all of them pass while proving nothing at all. Pin the
    // properties the perturbation cases depend on, up front and out loud.
    const auto snapshot = buildPatternSnapshot (purityFixtureState (), 1);
    REQUIRE (snapshot != nullptr);

    const auto reference = referenceSweep (*snapshot);
    REQUIRE (reference.size () == static_cast<std::size_t> (numTestPatterns * (lastStep - firstStep + 1)));

    const auto fillReference = referenceSweep (*snapshot, fillHeldRuntime);
    REQUIRE (fillReference.size () == reference.size ());

    int gated = 0;
    int rests = 0;

    // EVERY FIELD `sameEmission` COMPARES, FOR EVERY NOTE IN THE LIST — see the
    // insertion site for why this is no longer keyed on note 0.
    std::set<std::tuple<int, int, int, double, double, double, std::uint32_t>> distinct;

    // ── THE PHASE 7.2 COUNTERS ───────────────────────────────────────────────
    // The 7.1 counters below say "L2 was entered". These say "L2's SUB-STEP half was
    // entered", and they exist because each of the 7.2 paths has a default that
    // silently skips it: RATCHET 1 means `ratchetChildPasses` is never called, MICRO 0
    // with straight swing means the composed displacement is bit-zero and the one
    // `jlimit` in the emission core never clamps, and a flat ramp means every child
    // carries its step's own VEL byte-for-byte. All three would leave the six
    // perturbations agreeing about paths they never took.
    int multiNoteEmissions = 0;
    int fullRatchetEmissions = 0;
    int displacedEmissions = 0;
    int saturatedLate = 0;
    int saturatedEarly = 0;
    int rampedEmissions = 0;

    // ── THE PHASE 7.1 COUNTERS ───────────────────────────────────────────────
    // `evaluateStep` returns an empty emission for THREE different reasons — the
    // GATE lane said no, a condition said no, a probability roll said no — and from
    // outside they are indistinguishable. So the L2 reasons are counted by asking
    // `conditionPasses` / `probabilityPasses` directly at the same index, which is
    // the only way to say "the sweep really entered the hash path" rather than
    // "the sweep produced some rests".
    int suppressedByCond = 0;
    int suppressedByProb = 0;
    int fillDependent = 0;

    for (std::size_t i = 0; i < reference.size (); ++i)
    {
        const auto& sample = reference[i];

        if (sample.emission.gate)
        {
            ++gated;

            // THE WHOLE NOTE LIST, NOT NOTE 0 — WIDENED IN PHASE 7.3, AS THIS COMMENT
            // ASKED FOR. It used to read note 0 only, which was sound while RATCHET sat
            // at its default 1 and note 0 was the step's only note, and it carried a
            // standing instruction: a fixture that exercises ratchets should widen this
            // to the whole list so a per-child impurity cannot hide behind child 0. The
            // fixture now exercises ratchets, so it has been done. Every field
            // `sameEmission` compares is in the key, including `positionInStep`,
            // `provenance` (which is where the `ratchetChild` bit lives) and the step's
            // own `shiftSteps` — so "the answers vary" now means they vary PER CHILD,
            // and a distinct count that collapsed would say the list had gone uniform.
            for (int childIndex = 0; childIndex < sample.emission.noteCount; ++childIndex)
            {
                const auto& child = sample.emission.notes[static_cast<std::size_t> (childIndex)];

                distinct.insert ({ sample.emission.channel,
                                   child.note,
                                   child.velocity,
                                   child.positionInStep,
                                   child.gateFractionOfStep,
                                   sample.emission.shiftSteps,
                                   child.provenance });
            }

            if (sample.emission.noteCount > 1)
                ++multiNoteEmissions;

            if (sample.emission.noteCount == maxRatchetChildren)
                ++fullRatchetEmissions;

            // COMPARED AGAINST THE LITERAL 0.0, so `-Wfloat-equal` is satisfied and no
            // tolerance is invented: `swingShiftSteps` returns EXACTLY 0.0 at 50 % and
            // MICRO 0 contributes exactly 0.0, so an undisplaced step is bit-zero by
            // contract (see the note on `StepEmission::shiftSteps`), not approximately
            // so.
            if (sample.emission.shiftSteps != 0.0)
                ++displacedEmissions;

            // SATURATION, WRITTEN AS `>=` RATHER THAN `==`. The clamp in `evaluateStep`
            // guarantees `|shiftSteps| <= maxSubStepShiftSteps`, so `>=` on the absolute
            // value IS equality with the bound — but it says so without a float `==`
            // against a named constant, which is the comparison
            // `juce_recommended_warning_flags` rejects (see the pinning note on
            // `maxChildAheadSteps`). Counted in BOTH directions because a one-sided
            // count would be satisfied by a clamp that only ever ran on one side, and
            // the walk's scan widening is derived from both bounds.
            if (std::abs (sample.emission.shiftSteps) >= maxSubStepShiftSteps)
            {
                if (sample.emission.shiftSteps > 0.0)
                    ++saturatedLate;
                else
                    ++saturatedEarly;
            }

            // THE RAMP IS AUDIBLE: at least two surviving children of this step carry
            // DIFFERENT velocity bytes. A ramp left at its flat 0 default — or a
            // `ratchetVelocity` that short-circuited unconditionally — makes every
            // child carry the step's own VEL, and the per-child velocity path would then
            // be swept past by every perturbation in this file.
            for (int childIndex = 1; childIndex < sample.emission.noteCount; ++childIndex)
            {
                if (sample.emission.notes[static_cast<std::size_t> (childIndex)].velocity !=
                    sample.emission.notes[0].velocity)
                {
                    ++rampedEmissions;
                    break;
                }
            }
        }
        else
        {
            ++rests;
        }

        const auto& data = snapshot->pattern (sample.patternIndex);

        if (PatternSnapshot::isGated (data, sample.stepIndex))
        {
            if (! conditionPasses (data, sample.stepIndex, fixtureRuntime))
                ++suppressedByCond;
            else if (! probabilityPasses (data, sample.stepIndex))
                ++suppressedByProb;
        }

        if (sample.emission.gate != fillReference[i].emission.gate)
            ++fillDependent;
    }

    INFO ("gated " << gated << ", rests " << rests << ", distinct emissions " << distinct.size ()
                   << ", suppressed by COND " << suppressedByCond << ", suppressed by PROB " << suppressedByProb
                   << ", FILL-dependent " << fillDependent);

    REQUIRE (gated > 500); // the core is genuinely producing notes
    REQUIRE (rests > 100); // …and genuinely producing rests (the ordinal path)

    // WAS `> 20` WHEN THE KEY WAS NOTE 0 ALONE. The key now carries every field of
    // every child, so the count it is compared against is a different quantity and the
    // old floor would be met by an emission list that had gone uniform from child 1
    // onwards — the exact thing the widening exists to catch.
    REQUIRE (distinct.size () > 100);

    // L2 IS ACTUALLY ENTERED. Before Phase 7.1 extended this fixture all three of
    // these were ZERO: PROB sat at its default 100 (short-circuit, no hash) and
    // COND at `none` (no evaluation), so the five call-order perturbations could
    // not have detected a cursor in StepLogic.cpp at all. These three floors are
    // what make that impossible to regress into silently.
    REQUIRE (suppressedByCond > 100); // conditions gate, and gate often
    REQUIRE (suppressedByProb > 50);  // the HASH path is taken, not just the short-circuit
    REQUIRE (fillDependent > 20);     // some answers genuinely depend on the fourth parameter

    // ── L2's SUB-STEP HALF IS ACTUALLY ENTERED (Phase 7.2 paths) ─────────────
    // Every one of these six was ZERO under the 7.1 fixture, and each closes a
    // different way for the six perturbations to agree about a path they never took.
    // The floors are deliberately far below what the fixture produces — they are
    // non-vacuity thresholds, not a distributional contract on the lane values.
    INFO ("multi-note " << multiNoteEmissions << ", full ratchet " << fullRatchetEmissions << ", displaced "
                        << displacedEmissions << ", saturated late " << saturatedLate << ", saturated early "
                        << saturatedEarly << ", ramped " << rampedEmissions);

    REQUIRE (multiNoteEmissions > 200);  // steps really fire more than one note
    REQUIRE (fullRatchetEmissions > 40); // …and some fill the fixed-size list completely
    REQUIRE (displacedEmissions > 300);  // MICRO + swing really move onsets off the grid
    REQUIRE (saturatedLate > 40);        // …far enough to clamp at +maxSubStepShiftSteps
    REQUIRE (saturatedEarly > 20);       // …and at -maxSubStepShiftSteps (two-sided, see above)
    REQUIRE (rampedEmissions > 200);     // the per-child velocity path really varies velocity

    // …and the FILL dependence is TWO-SIDED: at least one step that fires only when
    // the pad is held (`FILL`) and at least one that fires only when it is not
    // (`!FILL`). A one-sided count would be satisfied by a runtime parameter that
    // was silently forced to a constant.
    int firesOnlyWhenHeld = 0;
    int firesOnlyWhenReleased = 0;

    for (std::size_t i = 0; i < reference.size (); ++i)
    {
        if (! reference[i].emission.gate && fillReference[i].emission.gate)
            ++firesOnlyWhenHeld;

        if (reference[i].emission.gate && ! fillReference[i].emission.gate)
            ++firesOnlyWhenReleased;
    }

    INFO ("fires only when held " << firesOnlyWhenHeld << ", only when released " << firesOnlyWhenReleased);
    REQUIRE (firesOnlyWhenHeld > 10);
    REQUIRE (firesOnlyWhenReleased > 10);
}

TEST_CASE ("sequencer/step-purity: evaluateStep is order- and history-independent (issue #53)", "[unit][determinism]")
{
    const auto snapshot = buildPatternSnapshot (purityFixtureState (), 1);
    REQUIRE (snapshot != nullptr);

    const auto reference = referenceSweep (*snapshot);
    const auto n = reference.size ();

    SECTION ("repeated calls on the same index never drift")
    {
        // Defeats: any per-call state advance — an RNG stream stepped once per
        // evaluation being the canonical Phase 7.1 mistake.
        for (int repeat = 0; repeat < 4; ++repeat)
            for (const auto& sample : reference)
            {
                const auto again = evaluateStep (*snapshot, sample.patternIndex, sample.stepIndex, fixtureRuntime);
                INFO ("repeat " << repeat << " pattern " << sample.patternIndex << " step " << sample.stepIndex);
                REQUIRE (sameEmission (again, sample.emission));
            }
    }

    SECTION ("a descending sweep matches the ascending one")
    {
        // Defeats: state keyed on call count or on "the previous index I was asked
        // about" — e.g. a `PRE` condition result carried forward instead of
        // re-derived, which issue #53 names explicitly.
        for (std::size_t i = n; i > 0; --i)
        {
            const auto& sample = reference[i - 1];
            const auto again = evaluateStep (*snapshot, sample.patternIndex, sample.stepIndex, fixtureRuntime);
            INFO ("descending pattern " << sample.patternIndex << " step " << sample.stepIndex);
            REQUIRE (sameEmission (again, sample.emission));
        }
    }

    SECTION ("a seeded shuffle of the whole sweep matches")
    {
        // Defeats: anything that survives reversal by symmetry.
        for (const auto index : seededPermutation (n, 0xC0FFEEULL))
        {
            const auto& sample = reference[index];
            const auto again = evaluateStep (*snapshot, sample.patternIndex, sample.stepIndex, fixtureRuntime);
            INFO ("shuffled pattern " << sample.patternIndex << " step " << sample.stepIndex);
            REQUIRE (sameEmission (again, sample.emission));
        }
    }

    SECTION ("interleaving the patterns round-robin matches")
    {
        // Defeats: a single-slot cache or accumulator that a per-pattern sweep
        // would never disturb. Also the shape a real pattern switch produces.
        const auto perPattern = static_cast<std::size_t> (lastStep - firstStep + 1);

        for (std::size_t s = 0; s < perPattern; ++s)
            for (int p = 0; p < numTestPatterns; ++p)
            {
                const auto& sample = reference[static_cast<std::size_t> (p) * perPattern + s];
                const auto again = evaluateStep (*snapshot, sample.patternIndex, sample.stepIndex, fixtureRuntime);
                INFO ("interleaved pattern " << sample.patternIndex << " step " << sample.stepIndex);
                REQUIRE (sameEmission (again, sample.emission));
            }
    }

    SECTION ("the retrigger lookahead's own access pattern matches")
    {
        // THE PRODUCTION SEQUENCE, reproduced exactly: `cutoffForSamePitch` peeks at
        // k+1 … k+maxRetriggerLookaheadSteps and the walk then emits k. Both the
        // peeks and the emission must agree with the in-order reference — the peek
        // must not disturb the emission, and the emission must not disturb the next
        // peek. If only one case in this file may exist, it is this one.
        constexpr std::int64_t lookahead = retriggerLookaheadSteps; // see the note there (issue #69)

        for (int p = 0; p < numTestPatterns; ++p)
        {
            const auto base = static_cast<std::size_t> (p) * static_cast<std::size_t> (lastStep - firstStep + 1);

            for (std::int64_t step = firstStep; step + lookahead <= lastStep; ++step)
            {
                for (std::int64_t ahead = 1; ahead <= lookahead; ++ahead)
                {
                    const auto& peeked = reference[base + static_cast<std::size_t> (step + ahead - firstStep)];
                    const auto again = evaluateStep (*snapshot, p, step + ahead, fixtureRuntime);
                    INFO ("peek pattern " << p << " step " << (step + ahead));
                    REQUIRE (sameEmission (again, peeked.emission));
                }

                const auto& emitted = reference[base + static_cast<std::size_t> (step - firstStep)];
                const auto again = evaluateStep (*snapshot, p, step, fixtureRuntime);
                INFO ("emit-after-peek pattern " << p << " step " << step);
                REQUIRE (sameEmission (again, emitted.emission));
            }
        }
    }

    SECTION ("interleaving the two StepRuntime values matches two separate references")
    {
        // ── PERTURBATION 6: THE CHECKED WIDENING (Phase 7.1) ─────────────────
        // `evaluateStep` grew a FOURTH parameter. The header's purity note was
        // amended to argue it, and the argument rests on three properties: the flag
        // is a VALUE, it is CONST for the whole block, and the walk passes the SAME
        // latched value to the retrigger lookahead and to the emission. Properties
        // one and two are structural. This case is the behavioural check on what
        // remains: that the parameter is genuinely READ, that reading it leaves no
        // residue, and that a call with one value cannot contaminate the next call
        // with the other.
        //
        // TWO references, one per value, and every call compared against its OWN.
        // Comparing a `fillHeld == true` answer against a `fillHeld == false`
        // reference would assert that the parameter is IGNORED — the exact opposite
        // of the contract — and the anti-vacuity case above is what guarantees the
        // two references really do differ on some steps, so this is not two
        // identical sweeps checked twice.
        //
        // The alternation is per CALL, not per pass: pad 16 is a momentary control
        // and the flag flips between blocks in production, so the adversarial order
        // is false/true/false/true at the same index, not one whole sweep then the
        // other.
        const auto fillReference = referenceSweep (*snapshot, fillHeldRuntime);
        REQUIRE (fillReference.size () == n);

        int differing = 0;

        for (std::size_t i = 0; i < n; ++i)
        {
            const auto& released = reference[i];
            const auto& held = fillReference[i];

            // Interleaved, and in both orders — held-then-released as well as
            // released-then-held — so a one-slot cache cannot be defeated by the
            // alternation happening to line up with its refresh.
            const auto a = evaluateStep (*snapshot, released.patternIndex, released.stepIndex, fixtureRuntime);
            const auto b = evaluateStep (*snapshot, held.patternIndex, held.stepIndex, fillHeldRuntime);
            const auto c = evaluateStep (*snapshot, held.patternIndex, held.stepIndex, fillHeldRuntime);
            const auto d = evaluateStep (*snapshot, released.patternIndex, released.stepIndex, fixtureRuntime);

            INFO ("runtime-interleaved pattern " << released.patternIndex << " step " << released.stepIndex);
            REQUIRE (sameEmission (a, released.emission));
            REQUIRE (sameEmission (b, held.emission));
            REQUIRE (sameEmission (c, held.emission));
            REQUIRE (sameEmission (d, released.emission));

            if (! sameEmission (released.emission, held.emission))
                ++differing;
        }

        // ANTI-VACUITY, restated locally: if the two references were identical this
        // whole section would collapse into perturbation 1 run twice.
        INFO ("indices whose emission depends on fillHeld: " << differing);
        REQUIRE (differing > 20);
    }

    SECTION ("the lookahead's access pattern is stable across a mid-sweep FILL change")
    {
        // The production hazard the section above does not quite reach: a pad-16
        // press lands at a BLOCK HEAD, so within one block the flag is constant but
        // ACROSS blocks it flips — and `cutoffForSamePitch` peeks forward from the
        // last step of block k while the emission for those steps happens in block
        // k+1 under a possibly different flag. The engine's answer to that is
        // documented at the latch site: the prediction and the emission must be
        // internally consistent WITHIN a block. What must never happen is that a
        // peek under one flag value changes what a later call under the other
        // returns.
        constexpr std::int64_t lookahead = retriggerLookaheadSteps; // see the note there (issue #69)
        const auto fillReference = referenceSweep (*snapshot, fillHeldRuntime);

        for (int p = 0; p < numTestPatterns; ++p)
        {
            const auto base = static_cast<std::size_t> (p) * static_cast<std::size_t> (lastStep - firstStep + 1);

            for (std::int64_t step = firstStep; step + lookahead <= lastStep; step += 3)
            {
                // Peek forward with the pad HELD…
                for (std::int64_t ahead = 1; ahead <= lookahead; ++ahead)
                {
                    const auto& peeked = fillReference[base + static_cast<std::size_t> (step + ahead - firstStep)];
                    const auto again = evaluateStep (*snapshot, p, step + ahead, fillHeldRuntime);
                    INFO ("held peek pattern " << p << " step " << (step + ahead));
                    REQUIRE (sameEmission (again, peeked.emission));
                }

                // …then emit with it RELEASED, and require the released reference.
                const auto& emitted = reference[base + static_cast<std::size_t> (step - firstStep)];
                const auto again = evaluateStep (*snapshot, p, step, fixtureRuntime);
                INFO ("released emit-after-held-peek pattern " << p << " step " << step);
                REQUIRE (sameEmission (again, emitted.emission));
            }
        }
    }

    SECTION ("a second, independently built snapshot of the same state answers identically")
    {
        // Defeats: a memo keyed on the snapshot's ADDRESS rather than its contents,
        // and any state that survives between builds. The build counter differs on
        // purpose — it is diagnostics only and must never reach the emission.
        const auto rebuilt = buildPatternSnapshot (purityFixtureState (), 99);
        REQUIRE (rebuilt != nullptr);
        REQUIRE (rebuilt.get () != snapshot.get ());

        for (const auto& sample : reference)
        {
            const auto again = evaluateStep (*rebuilt, sample.patternIndex, sample.stepIndex, fixtureRuntime);
            INFO ("rebuilt pattern " << sample.patternIndex << " step " << sample.stepIndex);
            REQUIRE (sameEmission (again, sample.emission));
        }
    }
}
