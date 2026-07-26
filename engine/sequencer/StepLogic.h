#pragma once

#include "../EngineGuiGuard.h"
#include "PatternSnapshot.h"
#include "PatternTypes.h"

#include <cstdint>
#include <type_traits>

namespace arpbox::engine
{
// ─────────────────────────────────────────────────────────────────────────────
// StepLogic.h — LAYER L2 of the pipeline (ARCHITECTURE §5.1 "step logic", §12.2).
// Phase 7.1's half: the per-step PROBABILITY roll and the 39 TRIG CONDITIONS.
// Phase 7.2's half: SWING (`swingShiftSteps`) and the RATCHET decisions — child
// count, per-child probability, velocity ramp. What 7.2 did NOT put here is the
// SHAPE the results take: `StepEmission`'s note list and the composed
// `shiftSteps` live in sequencer/SequencerProcessor.h, because the step walk's scan
// widening and the retrigger lookahead's reach are derived from the same geometry
// and must sit beside it. This file answers "what does the lane say"; that one
// answers "where does the resulting event go".
//
// ── FREE FUNCTIONS, NO CLASS, NO STATE — AND THAT IS THE DESIGN ─────────────
// This file has the same posture issue #53 gave `evaluateStep`, for the same
// reason and by the same means. `SequencerProcessor::cutoffForSamePitch` decides a
// note-off by PREDICTING what steps k+1, k+2 … will emit; the prediction is sound
// only while evaluating a step is a pure function of its arguments. The natural
// Phase 7.1 impurities are an RNG cursor advanced once per roll and a `PRE` result
// cached by the emitting walk — both of which want somewhere to live.
//
// There is nowhere. These are free functions at namespace scope: no `this`, no
// member, nothing to hang a cache on. A `static` local would still compile, which
// is why `tests/step_purity.cpp` exists as the behavioural half — it calls the
// emission core reversed, repeated, shuffled and interleaved and demands identical
// answers. Between the two, "the same step evaluates the same way in any order" is
// checked rather than asserted in a comment.
//
// ── THE RANDOMNESS IS A PER-INDEX HASH, NOT A STREAM ────────────────────────
// `rng::stepHash (masterSeed, domain, stepIndex)` — see engine/generative/Rng.h.
// A stream's output depends on how many times it has been pulled, and pull count
// is a property of the block carving and of how many steps the lookahead happened
// to peek at, not of the music. A hash keyed on the global step index has no such
// dependence, so §1.2's "same (pattern, seeds, N bars) ⇒ byte-identical MIDI"
// falls out instead of being defended.
//
// ── HOW L2 COMPOSES WITH L1, AND WHAT IT MUST NOT TOUCH ─────────────────────
// Both functions here are consulted AFTER the GATE check and BEFORE the pool read
// (`evaluateStep`). A step suppressed by a condition or a probability roll STILL
// CONSUMES ITS GATED ORDINAL — the pool cursor is defined by the GATE lane alone.
// See "PHASE 7, READ THIS BEFORE YOU 'FIX' IT" on `PatternSnapshot::gatedOrdinal`:
// folding PROB/COND into the ordinal would make the arpeggio's PITCH sequence
// depend on RNG outcomes, i.e. on block carving. Rhythm is stochastic; pitch
// order is not.
// ─────────────────────────────────────────────────────────────────────────────

/** The live, NON-SNAPSHOT inputs one step evaluation needs (§12.2 `FILL`/`!FILL`).

    ── WHY THIS EXISTS AT ALL ──────────────────────────────────────────────────
    Everything else `evaluateStep` reads is snapshot state, a pure function of
    (snapshot, pattern index, step index). FILL is not: it is pad 16 held down
    RIGHT NOW, it changes on both press AND release, and putting it on
    `PatternSnapshot` would mean rebuilding and republishing the whole ~120 KB
    document twice per pad tap — contending with the piano-roll edit flow, which
    already rebuilds on every mouse move (§4). Wrong layer. It arrives instead as
    an `EngineCommandType::setFillHeld` command and is latched once per block.

    ── WHY PURITY SURVIVES IT ──────────────────────────────────────────────────
    It is a VALUE, not a handle to mutable state; it is `const` for the whole
    block; and the walk passes the SAME latched value to the lookahead and to the
    emission, so a prediction and the eventual emission cannot disagree about it.
    Keep all three properties if this struct ever grows a second field. */
struct StepRuntime
{
    bool fillHeld = false; ///< §12.2 FILL — pad 16 held (`EngineCommandType::setFillHeld`).
};

static_assert (std::is_trivially_copyable_v<StepRuntime>,
               "StepRuntime is passed BY VALUE into the emission core precisely so it cannot be a "
               "channel for mutable state. Keep it a POD.");

/** Longest `PRE`/`!PRE` chain that is followed before the walk gives up (user
    decision D6).

    ── THE BUDGET IS WHAT MAKES `PRE` A TOTAL FUNCTION ─────────────────────────
    Not a performance concession. `PRE` refers to the previous gated step's result,
    so a run of consecutive `PRE` steps is a recurrence — and if EVERY gated step
    in the gate cycle carries `PRE`, the recurrence has no anchor and is not
    well-founded: "all true" and "all false" are both fixed points of it, and a
    recursive implementation would not terminate at all. A depth budget with a
    defined base case is what turns "undefined" into "defined". D6 fixes the base
    case at `false`, so an unanchored chain decays to silence rather than to a
    self-sustaining note. */
inline constexpr int maxPreChainDepth = 8;

/** The MOD A value at or above which `NEI` reads as "the neighbour passed" (user
    decision D7). Mid-scale of MOD A's 0..127 range (§12.1), so a lane sitting at
    its default 0 reads as `!NEI` and a lane painted to full reads as `NEI`.

    D7 IS THE v1 READING OF §12.2's "neighbour mod lane's result", chosen because
    it is the only one available before Phase 14's mod matrix exists. It makes MOD
    A AUDIBLE three phases early — see the lane annotation in PatternTypes.h, which
    Phase 14.1 must not read as "MOD A is unused". */
inline constexpr std::int16_t neighbourModThreshold = 64;

// RT-SAFE: audio thread. One floor-modulus and two multiplies. No state.
/** The SWING displacement of global step `stepIndex`, in STEPS (§8.1
    `transport.swingPct`, §12.1's "swing applies on top" of MICRO).

    50 % is straight and returns EXACTLY 0.0 — not "approximately zero". That is
    what makes every pre-7.2 golden provably unmoved by swing's arrival: the
    displacement term is bit-zero, so `gridPpq + 0.0 * stepPpq` is `gridPpq`.
    75 % delays every odd step by half a step, which is `maxSubStepShiftSteps`.

    ── `stepFloorMod`, NEVER `stepIndex % 2` ───────────────────────────────────
    C++ `%` yields NEGATIVE residues for negative operands, so `-1 % 2 == -1` and
    step -1 would read as EVEN — pairing the whole negative timeline half a step
    out of phase with the positive one. Negative indices are not hypothetical:
    the retrigger lookahead's backward scan and the locate paths evaluate them,
    and tests/step_purity.cpp and tests/boundary_agreement.cpp both sweep below 0.

    ── PAIRING IS ON THE *GLOBAL* INDEX, NOT THE PATTERN-LOOP INDEX ────────────
    Same reasoning the project-level grid got. If swing paired on a per-pattern
    phase, a quantized pattern switch landing on an odd global step would re-phase
    the shuffle mid-flight — the downbeat/upbeat roles would swap for the rest of
    the session, from a control event that is supposed to be inaudible except as a
    change of material.

    @param swingPct   `PatternSnapshot::swingPct`, already clamped to [50, 75].
    @param stepIndex  GLOBAL step index; negative values are legal. */
constexpr double swingShiftSteps (double swingPct, std::int64_t stepIndex) noexcept
{
    if (stepFloorMod (stepIndex, 2) != 1)
        return 0.0;

    return (swingPct / 100.0 - 0.5) * 2.0;
}

static_assert (swingShiftSteps (50.0, 1) == 0.0, "50 % swing must be EXACTLY straight, or every golden moves.");
static_assert (swingShiftSteps (75.0, 1) == 0.5, "75 % swing delays odd steps by half a step.");
static_assert (swingShiftSteps (75.0, 2) == 0.0, "Even steps are never displaced by swing.");
static_assert (swingShiftSteps (75.0, -1) == 0.5, "Floor-mod: step -1 is ODD, not even (`%` would say even).");
static_assert (swingShiftSteps (75.0, -2) == 0.0, "Floor-mod: step -2 is EVEN.");

/** A decoded §12.2 `A:B` cycle: fires on loop `a` of every `b` pattern loops.
    `b == 0` means the condition is not an `A:B` one. */
struct AbCycle
{
    int a = 0; ///< 1-based loop within the cycle, 1..`b`.
    int b = 0; ///< Cycle length in pattern loops: 2, 4, 8 or 16. 0 ⇒ not an A:B condition.
};

// RT-SAFE: audio thread. Pure arithmetic over a 4-entry constant table.
/** Decodes an `A:B` `TrigCondition` ordinal into its `(a, b)` pair.

    ── DERIVED, NOT TABULATED PER CONDITION ────────────────────────────────────
    The 30 `A:B` enumerators form ONE contiguous ordinal block starting at
    `ab1of2` (pinned by the `static_assert`s in PatternTypes.h), laid out as four
    families B ∈ {2, 4, 8, 16} with A running 1..B — so cumulative offsets
    {0, 2, 6, 14} within the block. A 30-arm switch would be 30 chances to write
    `3:4` where `4:4` belongs, and the compiler could not tell. This derives the
    pair from the ordinal instead, and `abDecodeIsExhaustive` below checks all 30
    at compile time.

    @returns `{0, 0}` for `none`, the named conditions, and any out-of-enum value. */
constexpr AbCycle abCycleFor (TrigCondition cond) noexcept
{
    constexpr int numAbFamilies = 4;
    constexpr int familyB[numAbFamilies] = { 2, 4, 8, 16 };
    constexpr int familyStart[numAbFamilies] = { 0, 2, 6, 14 };

    const int ordinal = static_cast<int> (cond);

    if (ordinal < static_cast<int> (TrigCondition::ab1of2) || ordinal > static_cast<int> (TrigCondition::ab16of16))
        return {};

    const int r = ordinal - static_cast<int> (TrigCondition::ab1of2);

    for (int family = numAbFamilies - 1; family >= 0; --family)
        if (r >= familyStart[family])
            return { r - familyStart[family] + 1, familyB[family] };

    return {};
}

/** Compile-time exhaustive check of `abCycleFor` over the whole A:B block: walks
    the four families in declaration order and requires the decode to agree at every
    one of the 30 ordinals, and that the block is exactly 30 long. */
constexpr bool abDecodeIsExhaustive () noexcept
{
    constexpr int familyB[4] = { 2, 4, 8, 16 };
    int ordinal = static_cast<int> (TrigCondition::ab1of2);

    for (const int b : familyB)
        for (int a = 1; a <= b; ++a)
        {
            const AbCycle decoded = abCycleFor (static_cast<TrigCondition> (ordinal));

            if (decoded.a != a || decoded.b != b)
                return false;

            ++ordinal;
        }

    return ordinal == static_cast<int> (TrigCondition::ab16of16) + 1;
}

static_assert (abDecodeIsExhaustive (),
               "abCycleFor disagrees with the TrigCondition ordinal layout. Either the enum grew an "
               "entry inside the A:B block (forbidden — PatternTypes.h is append-only) or the family "
               "offsets here are wrong. Both silently change which loop a saved pattern fires on.");
static_assert (abCycleFor (TrigCondition::none).b == 0, "`none` is not an A:B condition.");
static_assert (abCycleFor (TrigCondition::first).b == 0, "The named conditions are not A:B conditions.");
static_assert (abCycleFor (TrigCondition::notNei).b == 0, "The named conditions are not A:B conditions.");
static_assert (abCycleFor (TrigCondition::count).b == 0, "Out-of-range ordinals decode to nothing.");

// RT-SAFE: audio thread. Allocation-free, lock-free, no state. Bounded by
// `maxPreChainDepth` table reads plus at most that many hashes.
/** Whether the §12.2 trig condition on step `stepIndex` of `data` passes.

    Conditions GATE BEFORE probability (§5.1 L2) — `probabilityPasses` is a separate
    call the caller makes after this one.

    @param data       The pattern, as the audio thread reads it.
    @param stepIndex  GLOBAL step index; negative values are legal (the lookahead
                      and the locate paths sweep below 0).
    @param runtime    The block's latched live inputs — see `StepRuntime`. */
bool conditionPasses (const PatternData& data, std::int64_t stepIndex, StepRuntime runtime) noexcept;

// RT-SAFE: audio thread. One lane read and at most one hash. No state.
/** Whether step `stepIndex`'s PROB lane roll passes (§5.1 L2, §12.1 PROB 0–100 %).

    Seed-exact and re-derivable: the roll is `rng::stepHash` over the pattern's
    `masterSeed` and the GLOBAL step index, never a draw from a running stream, so
    asking twice — or asking about a future step from the retrigger lookahead —
    gives the same answer as the emission eventually does. */
bool probabilityPasses (const PatternData& data, std::int64_t stepIndex) noexcept;

// ── RATCHETS (§5.1 L2 "Ratchets (1–8) with per-ratchet velocity ramp and ratchet
//    probability", §12.1 RATCHET, Phase 7.2) ──────────────────────────────────

// The ramp's RANGE constants live in PatternTypes.h beside swing's, because
// `PatternSnapshot` has to name them and this header includes PatternSnapshot.h
// (not the reverse). See `minRatchetVelocityRampPct` there for why the ramp is
// project-level rather than a lane.

// RT-SAFE: audio thread. One lane read and one clamp.
/** How many notes step `stepIndex` fires before per-child probability thins them
    — the RATCHET lane value clamped into `[1, laneRange(ratchet).hi]`.

    Clamped even though `buildPatternSnapshot` already clamps every lane value:
    this number sizes a loop that writes into a fixed-size array on the audio
    thread from data that arrived through a pointer swap. */
int ratchetChildCount (const PatternData& data, std::int64_t stepIndex) noexcept;

// RT-SAFE: audio thread. One lane read and at most one hash. No state.
/** Whether ratchet child `childIndex` (`>= 1`) of step `stepIndex` fires.

    ── CHILD 0 IS NOT ASKED, AND MUST NOT BE ───────────────────────────────────
    Child 0 IS the step's own onset; whether it fires was already decided by
    `conditionPasses` + `probabilityPasses`. Rolling again for it would mean a
    RATCHET-1 step (the default) consumed a different amount of randomness from a
    RATCHET-1 step before Phase 7.2, i.e. every existing pattern would change.
    Callers pass `childIndex >= 1`; this returns true for 0 defensively.

    ── THE ROLL REUSES THE PROB LANE, AND IS A PURE HASH OF FOUR THINGS ────────
    §12.1 has no per-ratchet probability lane, so the v1 reading is the step's own
    PROB: a 50 % step fires half the time, and each of its extra children
    independently fires half the time — the roll thins the ROLL as well as the
    rhythm. The `>= 100` short-circuit is inherited from `probabilityPasses` and
    is a contract for the same reason: at PROB 100 (the default) NO randomness is
    consumed, so a RATCHET-8 pattern at default PROB is fully deterministic and
    provably identical across an `rngVersion` change.

    IT IS `rng::subStepHash (masterSeed, ratchetProbability, stepIndex,
    childIndex)` — A PURE HASH, NEVER A RUNNING STREAM, AND THIS IS THE SINGLE
    MOST LOAD-BEARING SENTENCE IN THE RATCHET IMPLEMENTATION. A stream advanced
    once per call would make its output a function of HOW MANY TIMES IT HAD BEEN
    PULLED — and the retrigger lookahead pulls it for up to nine steps' worth of
    children per emitted note, a count that depends on note lengths, on the block
    carving and on how far the scan happened to reach. The lookahead's prediction
    and the eventual emission would then disagree about which children exist, and
    note-off placement would become a function of call count: the #36/#46/#48
    family one level up. */
bool ratchetChildPasses (const PatternData& data, std::int64_t stepIndex, int childIndex) noexcept;

// RT-SAFE: audio thread. Pure arithmetic plus one round and one clamp.
/** Child `childIndex` of `childCount`'s velocity, given the step's own VEL and the
    project ramp: linear from `baseVelocity` at child 0 to
    `baseVelocity * (1 + rampPct / 100)` at child `childCount - 1`, clamped into
    MIDI's 1..127.

    Returns `baseVelocity` UNCHANGED (not "rounded to the same value") when
    `childCount <= 1` or the ramp is zero — the arithmetic is short-circuited, so a
    default project cannot lose a velocity to a rounding edge. */
int ratchetVelocity (int baseVelocity, int childIndex, int childCount, double rampPct) noexcept;
} // namespace arpbox::engine
