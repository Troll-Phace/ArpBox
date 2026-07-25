#pragma once

#include "../EngineGuiGuard.h"
#include "PatternTypes.h"

#include <cstdint>

namespace arpbox::engine::euclid
{
// ─────────────────────────────────────────────────────────────────────────────
// Euclid.h — the euclidean rhythm generator that writes the GATE lane
// (ARCHITECTURE §5.1 L1: "Euclidean generator (steps/pulses/rotate) writes the
// GATE lane", §8.1 `euclid {steps,pulses,rotate}`).
//
// MESSAGE-THREAD ONLY. This runs when the user edits the euclid controls, and it
// writes into the document's GATE lane; the audio thread only ever reads the
// resulting lane values out of an immutable PatternSnapshot. Nothing here is
// called from processBlock.
//
// ── WHY BRESENHAM AND NOT RECURSIVE BJORKLUND ────────────────────────────────
// Both produce the same maximally-even necklace. Bresenham gets there in one
// O(steps) pass with no allocation and no recursion, and — the reason that
// actually decides it — the ROTATION CONVENTION is one visible line of code
// instead of an emergent property of where a recursion happens to stop. Phase 6.4
// freezes this output into golden MIDI files, so the convention has to be
// something a reader can verify by eye, permanently.
//
// ── FROZEN CONTRACTS (Phase 6.4 turns each of these into a golden file) ──────
//   * Necklace:   out[i] = ((i * pulses) % steps) < pulses
//   * E(3,8)   == x..x..x.   — i.e. {1,0,0,1,0,0,1,0}, A PULSE ON STEP 0.
//     (0*3)%8=0<3 ✓ · (1*3)%8=3 ✗ · (2*3)%8=6 ✗ · (3*3)%8=1 ✓ ·
//     (4*3)%8=4 ✗ · (5*3)%8=7 ✗ · (6*3)%8=2 ✓ · (7*3)%8=5 ✗
//   * E(5,8)   == x.x.xx.x   · E(4,16) == x...x...x...x...
//     NOTE on E(5,8): the textbook Bjorklund answer is usually WRITTEN
//     x.xx.xx. — that is the SAME necklace (inter-onset intervals {2,2,2,1,1})
//     at a different starting rotation. Bresenham's phase, which always puts a
//     pulse on step 0 and is what `rotate` is measured from, is x.x.xx.x. Do not
//     "fix" this to match a paper; use `rotate` if you want the other phase.
//   * ROTATION: positive `rotate` moves pulses LATER in the bar —
//     out[(i + rotate) % steps] = base[i].  E(3,8) rotate 1 == .x..x..x
//   * `pulses` is CLAMPED into [0, steps]; `rotate` is taken modulo `steps` and
//     may be any sign (−1 is equivalent to steps−1).
//   * `pulses == 0`     ⇒ every step off.
//   * `pulses == steps` ⇒ every step on.
// Changing any of the above is a determinism-contract break: it needs an
// `rngVersion`/`schemaVersion` decision and a justified golden regeneration
// (§1.2, §5.2), not a quiet edit.
// ─────────────────────────────────────────────────────────────────────────────

// MESSAGE-THREAD ONLY: writes into caller-owned memory; never allocates.
/** Fills `out[0 .. n)` with the euclidean necklace E(`pulses`, `steps`) rotated by
    `rotate`, where each entry is 1 (pulse) or 0 (rest) — GATE lane values.

    @param steps   Necklace length. Values `<= 0` write nothing; values above
                   `maxSteps` are CLAMPED to `maxSteps` (a pattern cannot be
                   longer than 64 steps, §12.1), so `n == min (steps, maxSteps)`
                   entries are written and no more.
    @param pulses  Onsets, clamped into `[0, n]`.
    @param rotate  Rotation in steps; any sign, taken modulo `n`. Positive moves
                   pulses LATER.
    @param out     Caller-owned buffer with room for at least `n` entries. A null
                   pointer writes nothing. */
void generate (int steps, int pulses, int rotate, std::uint8_t* out) noexcept;

// MESSAGE-THREAD ONLY: writes into caller-owned memory; never allocates.
/** `generate` applied to a `LaneState`'s value array, writing 0/1 into the first
    `min (params.steps, maxSteps)` entries and setting `lane.length` to match.
    Steps beyond the necklace keep their existing values (they are outside the
    lane's active length, so they never play).

    Does nothing when `params.enabled` is false — a disabled euclid must leave a
    hand-edited GATE lane exactly as the user drew it. */
void applyToGateLane (const EuclidParams& params, LaneState& lane) noexcept;
} // namespace arpbox::engine::euclid
