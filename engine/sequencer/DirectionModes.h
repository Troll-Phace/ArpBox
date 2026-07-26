#pragma once

#include "../EngineGuiGuard.h"
#include "PatternTypes.h"

#include <cstdint>

namespace arpbox::engine::direction
{
// ─────────────────────────────────────────────────────────────────────────────
// DirectionModes.h — pool-traversal ORDER TABLES for the 11 direction modes of
// ARCHITECTURE §12.3.
//
// MESSAGE-THREAD ONLY, AND THAT IS THE ENTIRE POINT. Every mode — including the
// two stochastic ones — is resolved here, once, into a flat array of pool
// indices. The audio thread then runs exactly:
//
//     poolIndex = order[ordinal % period];
//     note      = poolNoteAtDegree (notes, poolSize, poolIndex + pitchDegrees);
//
// with ZERO mode branches, ZERO randomness and ZERO per-step state. A direction
// mode costs the same as `up` at every point in processBlock.
//
// ── THE ORDINAL IS A GATED-STEP COUNT, NOT A STEP INDEX ─────────────────────
// The pool cursor advances on GATED STEPS ONLY (hardware-arpeggiator semantics:
// a rest thins the rhythm, it does not transpose everything after it). So the
// `ordinal` indexing these tables counts the steps that actually fired, not raw
// step indices. This header does not implement the cursor — it only builds the
// table the cursor indexes — but the tables are periodic in the gated-step
// ordinal, so do not feed them a raw step index.
//
// ── ORDERS (n = poolSize, k = ordinal % period) ─────────────────────────────
//   up               period n        k
//   down             period n        n-1-k
//   upDownInclusive  period 2n       k < n ? k : 2n-1-k      (endpoints twice)
//   upDownExclusive  period max(1,2n-2)  k < n ? k : 2n-2-k  (endpoints once)
//   converge         period n        k even → k/2 ; k odd → n-1-k/2   OUTSIDE→MIDDLE
//   diverge          period n        converge[n-1-k]         MIDDLE→OUTSIDE (exact reverse)
//   outsideIn        period 2n       converge ++ diverge     the full round trip
//   asPlayed         period n        k — see `usesAsPlayedView` below
//   spiral           period 3n       up-2/back-1: 0,1,2, 1,2,3, 2,3,4, … (mod n)
//   walk             period 4n       reflecting ±1 brownian, seeded
//   randomNoRepeat   period 4n       uniform, never twice in a row, seeded
//
// converge/diverge/outsideIn are pinned exactly as above because Phase 6.4
// freezes them into golden MIDI. For n = 4: converge = 0,3,1,2 ·
// diverge = 2,1,3,0 · outsideIn = 0,3,1,2,2,1,3,0.
//
// OUTSIDE-IN REPEATS AT BOTH ENDS, DELIBERATELY. Because it is literally
// converge followed by diverge, the MIDDLE note sounds twice at the turnaround
// (…1,2,2,1… above) and the OUTER note sounds twice across the loop point
// (0 last, 0 first). That is the same shape `upDownInclusive` already has
// (0,1,2,3,3,2,1,0), so the two "round trip" modes behave consistently — and an
// exclusive variant is available by playing `converge` and `diverge` in
// sequence. If a future spec wants outside-in to skip the doubles, it is a
// different mode APPENDED to the enum, not an edit to this one: changing it
// here regenerates goldens (§1.2).
//
// ── DEGENERATE POOLS ────────────────────────────────────────────────────────
//   n == 1 ⇒ every mode is period 1, order {0}.
//   n <= 0 ⇒ period 0, nothing written. The caller must not emit notes.
// ─────────────────────────────────────────────────────────────────────────────

/** Longest table any mode can produce: `walk` and `randomNoRepeat` at 4n with a
    full pool. Size any traversal buffer with this. */
inline constexpr int maxTraversalPeriod = 4 * maxPoolSize;
static_assert (maxTraversalPeriod == 128, "walk/randomNoRepeat are 4n over a 32-note pool.");

// RT-SAFE: audio thread. Pure constexpr predicate.
/** True when `mode` selects the pool's ARRIVAL-ORDERED array rather than
    producing a distinct permutation — i.e. `DirectionMode::asPlayed`.

    `asPlayed` is not an ordering, it is a VIEW: its traversal table is the plain
    ascending `up` order, and what makes it "as played" is that the caller walks
    `PoolSnapshot::asPlayed` instead of `PoolSnapshot::sorted`. Exposing that as a
    predicate is what keeps the enum out of the RT path — resolve it ONCE at
    snapshot adoption via `poolNotes (pool, usesAsPlayedView (mode))` and the
    per-step code never branches on the mode at all. */
constexpr bool usesAsPlayedView (DirectionMode mode) noexcept
{
    return mode == DirectionMode::asPlayed;
}

// MESSAGE-THREAD ONLY: pure function of its arguments; writes only into `out`.
// Never allocates (the caller supplies the buffer) and never recurses.
/** Builds the cyclic pool-traversal order for one (`mode`, `poolSize`).

    ── WHY THE STOCHASTIC MODES ARE TABLES, AND WHY THEY DO NOT USE RngStream ──
    `walk` and `randomNoRepeat` are generated HERE, on the message thread, from
    the pattern's `masterSeed` via `splitmix64` — since Phase 7.1 the shared one
    in `engine/generative/Rng.h`, salted from that header's append-only
    `RngDomain` registry rather than from literals local to the .cpp. Phase 7
    must not "fix" this to draw from its audio-thread `RngStream`:

      * They are different consumers. `RngStream` (§5.2, Phase 7.1) is a STATEFUL
        xoshiro256++ stream pulled per step for probability rolls. `splitmix64` is
        the seed-mixing primitive §5.2 already names by hand ("Effective stream
        seed = splitmix64 (masterSeed ⊕ operatorSeed ⊕ …)"), used here as a pure
        hash. Using it is not pre-empting Phase 12; it is calling the function
        §5.2 specifies.
      * DETERMINISM IS STRICTLY STRONGER AS A TABLE. The table is a pure function
        of (mode, poolSize, seed). A live stream's output would instead depend on
        HOW MANY TIMES IT HAD BEEN PULLED — which is exactly the failure mode that
        breaks buffer-size independence and the §1.2 determinism contract, because
        pull count is a property of the block carving and the gate pattern, not of
        the music.
      * §5.2's LOOP LOCK falls out for free: a table IS a locked loop. Nothing
        extra is needed to make a walk repeat identically every cycle.

    `walk` starts at `n / 2` and steps ±1 on `bit0 (splitmix64 (seed ^ 0x5741 ^ k))`
    for each destination ordinal k, REFLECTING at 0 and n-1 (a brownian walk that
    wrapped would be a jump, not a walk) and never producing a zero-length step
    (§12.3 says "±1"). `randomNoRepeat` rejection-samples
    `splitmix64 (seed ^ 0x9E37 ^ k ^ (attempt << 32))` against the previous entry,
    and additionally against `out[0]` for the final entry so the no-repeat
    invariant survives the LOOP POINT.

    KNOWN AND ACCEPTED: `walk`'s last entry may not be ±1 from its first, so the
    loop point can contain a jump. Constraining it would bias the walk; §12.3 asks
    for a brownian walk, not a closed tour.

    @param mode         Traversal mode (§12.3).
    @param poolSize     Live pool size `n`. `<= 0` returns 0.
    @param seed         Drives `walk` and `randomNoRepeat` ONLY; every other mode
                        ignores it entirely and is a pure function of (mode, n).
    @param out          Receives `period` entries, each a pool index in `[0, n)`.
                        A null pointer returns 0.
    @param outCapacity  Size of `out`. If it is smaller than the mode's period,
                        NOTHING is written and 0 is returned — a truncated
                        traversal would silently change the music, which is worse
                        than a caller-visible failure. Size buffers with
                        `maxTraversalPeriod`.
    @returns the period (entries written), or 0 on any of the failure cases above. */
int buildTraversal (DirectionMode mode, int poolSize, std::uint64_t seed, std::uint8_t* out, int outCapacity) noexcept;

// MESSAGE-THREAD ONLY: pure function; no allocation.
/** The period `buildTraversal` will produce for (`mode`, `poolSize`), so a caller
    can size or validate a buffer without building the table. Returns 0 for
    `poolSize <= 0`. */
int traversalPeriod (DirectionMode mode, int poolSize) noexcept;
} // namespace arpbox::engine::direction
