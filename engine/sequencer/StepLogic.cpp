#include "StepLogic.h"

// EngineGuiGuard.h is pulled in FIRST by StepLogic.h (before any JUCE include),
// which is where the tripwire needs to sit; repeating it here would be a no-op.
// Same convention as sequencer/PatternSnapshot.cpp.
#include "../generative/Rng.h"
#include "PatternSnapshot.h"
#include "PatternTypes.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace arpbox::engine
{
namespace
{
    /** The number the PROB lane's percentage is rolled against (§12.1: PROB is
        0–100 %). Named so the modulo bias note below has something to point at. */
    constexpr std::uint64_t probabilityScale = 100;

    // RT-SAFE: audio thread. One lane read.
    /** The §12.2 condition on step `stepIndex`.

        The cast is total: `buildPatternSnapshot` runs every COND value through
        `clampLaneValue`, whose range for the lane is `[0, numTrigConditions - 1]`,
        so nothing out of enum can reach the RT path. `conditionPasses` still has a
        `default:` arm anyway — see the note there. */
    TrigCondition conditionAt (const PatternData& data, std::int64_t stepIndex) noexcept
    {
        return static_cast<TrigCondition> (laneValueAt (laneOf (data, LaneId::cond), stepIndex));
    }

    // RT-SAFE: audio thread. A short comparison ladder, at most one lane read. No
    // recursion.
    /** Every §12.2 condition EXCEPT `PRE`/`!PRE`, evaluated with no reference to any
        other step's result. `conditionPasses` routes `PRE`/`!PRE` away from here and
        uses this to evaluate a chain's ANCHOR, which is why it must stay
        non-recursive: it is the thing that terminates the chain walk.

        ── WHY THIS IS NOT A `switch`, THOUGH IT OBVIOUSLY WANTS TO BE ──────────
        `TrigCondition` has 39 enumerators and 30 of them — the whole A:B block —
        are handled ARITHMETICALLY by `abCycleFor` rather than case by case (see the
        note there: a 30-arm switch is 30 chances to write `3:4` where `4:4`
        belongs). A `switch` that leaves those 30 to a `default:` arm compiles
        cleanly but trips `-Wswitch-enum`, which this project's build has on. So the
        named conditions are dispatched by comparison instead. Do not "tidy" this
        back into a switch without also deciding what to do about that warning. */
    bool acyclicConditionPasses (const PatternData& data,
                                 std::int64_t stepIndex,
                                 StepRuntime runtime,
                                 TrigCondition cond) noexcept
    {
        if (cond == TrigCondition::none)
            return true;

        // ── 1ST / !1ST (user decision D5) ────────────────────────────────────
        // `== 0`, NEVER `<= 0`. `loopIndexAt` floor-divides, so a negative step
        // index is loop -1, -2 … — earlier loops, not "the first loop". `<=` is the
        // natural typo, it is invisible during ordinary forward playback (indices
        // are non-negative there), and it surfaces only through the retrigger
        // lookahead and the locate paths, i.e. as a note-length bug that looks like
        // a timing bug. See the D5 note on `PatternSnapshot::loopIndexAt`.
        if (cond == TrigCondition::first)
            return PatternSnapshot::loopIndexAt (data, stepIndex) == 0;

        if (cond == TrigCondition::notFirst)
            return PatternSnapshot::loopIndexAt (data, stepIndex) != 0;

        // ── FILL / !FILL ─────────────────────────────────────────────────────
        // The one input that is not a function of (snapshot, step) — see
        // `StepRuntime`.
        if (cond == TrigCondition::fill)
            return runtime.fillHeld;

        if (cond == TrigCondition::notFill)
            return ! runtime.fillHeld;

        // ── NEI / !NEI (user decision D7) ────────────────────────────────────
        if (cond == TrigCondition::nei)
            return laneValueAt (laneOf (data, LaneId::modA), stepIndex) >= neighbourModThreshold;

        if (cond == TrigCondition::notNei)
            return laneValueAt (laneOf (data, LaneId::modA), stepIndex) < neighbourModThreshold;

        if (cond == TrigCondition::pre || cond == TrigCondition::notPre)
        {
            // UNREACHABLE by contract: `conditionPasses` intercepts both before
            // calling here. Degrade to "no condition" rather than to silence, for
            // the same reason as the unknown-ordinal arm below.
            jassertfalse;
            return true;
        }

        // ── A:B (user decision D5) ───────────────────────────────────────────
        // Everything that reaches here is either one of the 30 A:B ordinals or an
        // out-of-enum value.
        const AbCycle cycle = abCycleFor (cond);

        if (cycle.b <= 0)
        {
            // DELIBERATE GRACEFUL DEGRADATION, NOT AN OVERSIGHT. This is the
            // `default:` arm the ladder does not spell out, and the only way to
            // reach it is a COND ordinal this build does not know — i.e. a project
            // saved by a NEWER schema version, loaded by an older binary. Returning
            // `false` would SILENCE every step carrying the unknown condition, so
            // the user's pattern would come back with holes in it and no error
            // anywhere. Returning `true` degrades it to "no condition": the step
            // fires, which is the same thing an un-conditioned step does and is
            // recoverable by ear. code-style.md's "graceful degradation, never
            // crash", applied to a schema mismatch. Do not "tighten" this to
            // `false`.
            return true;
        }

        // `stepFloorMod`, NEVER `%`. C++ `%` yields NEGATIVE residues for negative
        // operands, and `loopIndexAt` is deliberately floor-divided, so at step -1
        // the loop index is -1 and `-1 % 4` is `-1` — which equals no `a - 1` for
        // any A, so every A:B condition would silently fail for the whole of loop
        // -1 (tests/step_purity.cpp sweeps from index -37).
        //
        // THE `a - 1` IS WHERE THE ACCEPTANCE CRITERION LIVES. Loops are 0-based
        // here and 1-based in §12.2's notation: "3:4 fires on loop 3 of every 4"
        // means loop INDEX 2 of every 4, so the comparison is against `a - 1` and
        // not against `a`. Dropping the `- 1` fires 3:4 on musical loop 4 — one
        // loop late, forever, and every A:B golden would bake it in.
        return stepFloorMod (PatternSnapshot::loopIndexAt (data, stepIndex), cycle.b) == cycle.a - 1;
    }
} // namespace

// RT-SAFE:
bool probabilityPasses (const PatternData& data, std::int64_t stepIndex) noexcept
{
    const int percent = laneValueAt (laneOf (data, LaneId::prob), stepIndex);

    // ── THE `>= 100` SHORT-CIRCUIT IS A CONTRACT, NOT AN OPTIMISATION ────────
    // `laneDefault (LaneId::prob)` is 100, so EVERY pattern that has never had its
    // PROB lane touched takes this branch — and therefore consumes no randomness at
    // all. That is what makes "`rngVersion: 0` ⇒ this project's audible path used no
    // RNG" literally true rather than approximately true, and it is why the six
    // Phase-6 goldens are provably unmoved by this phase: not "the hash happened to
    // agree", but "the hash was never called".
    //
    // Deleting the short-circuit (e.g. "the hash is cheap, just always roll") would
    // be a determinism break dressed as a simplification: `hash % 100 < 100` is
    // always true, so nothing audible changes TODAY — and then Phase 12's LOOP LOCK
    // or a new RngDomain shifts the stream and every default-PROB golden moves.
    if (percent >= 100)
        return true;

    // Symmetrically: PROB 0 never fires and never rolls.
    if (percent <= 0)
        return false;

    // ── THE CAST IS MANDATORY ────────────────────────────────────────────────
    // `laneValueAt` returns `std::int16_t`. Without the explicit widening the
    // comparison would be between a `std::uint64_t` and an `int`, and the usual
    // arithmetic conversions would convert the int to `std::uint64_t` anyway — but
    // only because the guards above have already excluded the negative range.
    // Making it explicit means a future edit to those guards cannot turn a negative
    // percentage into 18446744073709551516.
    //
    // ── ON `% 100` BIAS: LEAVE IT ALONE ──────────────────────────────────────
    // 2^64 is not a multiple of 100, so 16 of the 2^64 hash outputs are "extra" —
    // a bias of 16 in 2^64, roughly 1e-18, which no listener and no test can
    // observe, and which is EXACTLY REPRODUCIBLE in any case (the determinism
    // contract cares that the answer is the same every time, not that it is
    // perfectly uniform). The textbook fix, rejection sampling, draws a VARIABLE
    // NUMBER OF TIMES — which is a cursor, which is precisely what Rng.h and issue
    // #53 exist to keep out of this call path. Do not "fix" this.
    return rng::stepHash (data.masterSeed, rng::RngDomain::stepProbability, stepIndex) % probabilityScale <
           static_cast<std::uint64_t> (percent);
}

// RT-SAFE:
int ratchetChildCount (const PatternData& data, std::int64_t stepIndex) noexcept
{
    constexpr auto range = laneRange (LaneId::ratchet);

    return juce::jlimit (static_cast<int> (range.lo),
                         static_cast<int> (range.hi),
                         static_cast<int> (laneValueAt (laneOf (data, LaneId::ratchet), stepIndex)));
}

// RT-SAFE:
bool ratchetChildPasses (const PatternData& data, std::int64_t stepIndex, int childIndex) noexcept
{
    // Child 0 is the step's own onset — already decided upstream. See the header.
    if (childIndex <= 0)
        return true;

    const int percent = laneValueAt (laneOf (data, LaneId::prob), stepIndex);

    // THE SAME `>= 100` CONTRACT AS `probabilityPasses`, and load-bearing for the
    // same reason: at PROB 100 (the lane default) NOT ONE HASH IS EVALUATED, so a
    // RATCHET-8 pattern on a fresh document is fully deterministic and provably
    // insensitive to anything Phase 12 does to the seed composition.
    if (percent >= 100)
        return true;

    if (percent <= 0)
        return false;

    // A PURE HASH OF (seed, domain, stepIndex, childIndex) — never a stream. The
    // header spells out why at length; the short version is that the retrigger
    // lookahead asks this question about steps it is not emitting, a variable
    // number of times, and must get the same answers the emission eventually does.
    return rng::subStepHash (data.masterSeed, rng::RngDomain::ratchetProbability, stepIndex, childIndex) %
               probabilityScale <
           static_cast<std::uint64_t> (percent);
}

// RT-SAFE:
int ratchetVelocity (int baseVelocity, int childIndex, int childCount, double rampPct) noexcept
{
    constexpr auto range = laneRange (LaneId::vel);

    // SHORT-CIRCUITED, not merely optimised: a default project (`rampPct == 0`) must
    // return the step's VEL byte-identically, and `llround (v * 1.0)` is only
    // *probably* that. Explicitly returning it makes the pre-7.2 goldens' velocities
    // a property of the code rather than of the FPU.
    if (childCount <= 1 || childIndex <= 0 || rampPct == 0.0)
        return baseVelocity;

    // LINEAR from child 0 (no scaling) to the LAST child (full ramp). Interpolating
    // over `childCount - 1` rather than `childCount` is what makes "-50 % halves the
    // last child" literally true at every child count, which is the only reading a
    // user can predict by ear.
    const double t =
        static_cast<double> (juce::jmin (childIndex, childCount - 1)) / static_cast<double> (childCount - 1);
    const double scaled = static_cast<double> (baseVelocity) * (1.0 + rampPct / 100.0 * t);

    return juce::jlimit (static_cast<int> (range.lo),
                         static_cast<int> (range.hi),
                         static_cast<int> (std::llround (scaled)));
}

// RT-SAFE:
bool conditionPasses (const PatternData& data, std::int64_t stepIndex, StepRuntime runtime) noexcept
{
    const TrigCondition cond = conditionAt (data, stepIndex);

    if (cond != TrigCondition::pre && cond != TrigCondition::notPre)
        return acyclicConditionPasses (data, stepIndex, runtime, cond);

    // ── PRE / !PRE (user decision D6) ────────────────────────────────────────
    // "The previous step's result" means the previous GATED step's result: PRE is
    // about the rhythm the listener hears, and a rest is not a step that "failed".
    // `previousGatedOffset` is the precomputed hop back (see PatternSnapshot.h).
    //
    // ITERATIVE, NEVER RECURSIVE — and the reason is correctness before it is
    // stack depth. A run of consecutive PRE steps is a recurrence with no base
    // case of its own; if every gated step in the gate cycle carries PRE the
    // recurrence is genuinely non-well-founded (all-true and all-false are both
    // fixed points), so a recursive formulation does not merely go deep, it does
    // not terminate. The explicit budget below is what makes this function TOTAL.
    //
    // Two walks. BACKWARD: hop from gated step to gated step while the step we land
    // on also carries PRE/!PRE, recording the chain, until one of three stops.
    // FORWARD: replay the recorded chain from the oldest link, applying
    // PRE / !PRE and conjoining each link's own probability.

    std::array<std::int64_t, maxPreChainDepth> chain {};
    int depth = 0;
    std::int64_t index = stepIndex;
    bool previousResult = false;

    for (;;)
    {
        if (depth >= maxPreChainDepth)
        {
            // STOP 1 — BUDGET EXHAUSTED. D6 fixes the base case at `false`, so a PRE
            // run longer than `maxPreChainDepth` consecutive gated steps decays to
            // silence rather than to a self-sustaining note. Frozen by the
            // `cond-pre-chain` golden, which sits deliberately on this boundary.
            previousResult = false;
            break;
        }

        chain[static_cast<std::size_t> (depth)] = index;
        ++depth;

        const std::int64_t back = PatternSnapshot::previousGatedOffsetAt (data, index);

        if (back <= 0)
        {
            // STOP 2 — NO PREVIOUS GATED STEP EXISTS (the GATE lane is entirely
            // off, so the offset table is all-zero). Base case `false`.
            previousResult = false;
            break;
        }

        const std::int64_t previousIndex = index - back;
        const TrigCondition previousCond = conditionAt (data, previousIndex);

        if (previousCond != TrigCondition::pre && previousCond != TrigCondition::notPre)
        {
            // STOP 3 — AN ANCHOR: a gated step whose condition does not itself refer
            // backwards. Evaluate it exactly as the forward walk evaluates a link,
            // condition AND probability, so that "result" means the same thing at
            // every position in the chain: did that step actually sound. Seeding the
            // anchor from its condition alone would make a PRE step downstream of a
            // 30 %-probability anchor disagree with what the listener heard.
            previousResult = acyclicConditionPasses (data, previousIndex, runtime, previousCond) &&
                             probabilityPasses (data, previousIndex);
            break;
        }

        index = previousIndex;
    }

    // FORWARD: `chain[depth - 1]` is the OLDEST link, `chain[0]` is `stepIndex`.
    for (int link = depth - 1; link >= 0; --link)
    {
        const std::int64_t linkIndex = chain[static_cast<std::size_t> (link)];
        const bool inverted = conditionAt (data, linkIndex) == TrigCondition::notPre;
        const bool conditionResult = inverted ? ! previousResult : previousResult;

        // Each link's own PROB is conjoined here so the NEXT link sees whether this
        // one actually sounded. For the final link (`stepIndex` itself) the caller
        // also calls `probabilityPasses` separately — harmless, because the roll is
        // a pure hash of the index and is therefore idempotent, and worth the
        // duplication to keep `evaluateStep`'s cond-then-prob order (§5.1 L2)
        // written out plainly at the call site.
        previousResult = conditionResult && probabilityPasses (data, linkIndex);
    }

    return previousResult;
}
} // namespace arpbox::engine
