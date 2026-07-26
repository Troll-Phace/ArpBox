#pragma once

#include "../EngineGuiGuard.h"

#include <cstdint>

namespace arpbox::engine::rng
{
// ─────────────────────────────────────────────────────────────────────────────
// Rng.h — THE shared randomness primitive for the whole engine (ARCHITECTURE
// §5.2 "Seeds & reversibility", §3.2 which assigns seeds to engine/generative/).
//
// Everything here is `constexpr`, stateless and header-only. There is no object,
// no cursor and nothing to prepare, which is the entire design:
//
//   RT-SAFE BY CONSTRUCTION. A pure hash cannot allocate, cannot lock, and
//   cannot carry state between calls, so calling it from `processBlock` needs no
//   audit beyond "is it this function".
//
//   DETERMINISM IS STRICTLY STRONGER THAN A STREAM'S. A stream's output depends
//   on HOW MANY TIMES IT HAS BEEN PULLED, and pull count is a property of the
//   block carving and the gate pattern — not of the music. A per-index hash is a
//   pure function of the index, so buffer-size independence and §1.2's
//   "same (pattern, seeds, N bars) ⇒ byte-identical MIDI, forever" fall out for
//   free rather than being defended.
//
// ── WHAT THIS IS *NOT*: PHASE 12'S `RngStream` ──────────────────────────────
// §5.2 also names a versioned xoshiro256++ `RngStream`. That is Phase 12's
// operator-stack tool: an `IStepOperator::process (StepContext&, RngStream&)`
// receives a stream that is created, drawn from a bounded number of times, and
// discarded WITHIN ONE OPERATOR'S CALL. It is never persistent across steps and
// it is never the thing the emission core (probability rolls, condition
// evaluation, direction tables) draws from — those must stay per-index hashes,
// because the retrigger lookahead evaluates FUTURE steps out of order and a
// cursor would make their results depend on evaluation order. Issue #53 made
// that failure class uncompilable for `evaluateStep`; this header is the
// positive half of the same argument.
// ─────────────────────────────────────────────────────────────────────────────

/** splitmix64 used as a PURE HASH (not as a stateful generator): one call in, one
    well-mixed 64-bit word out, no state carried between calls.

    ARCHITECTURE §5.2 names splitmix64 as the seed-mixing primitive by hand
    ("Effective stream seed = splitmix64 (masterSeed ⊕ operatorSeed ⊕ …)"), so
    this is not an invented RNG — the constants are the canonical ones
    (Steele/Lea/Flood).

    THE CONSTANTS AND SHIFT AMOUNTS ARE FROZEN. This body was migrated verbatim
    out of `engine/sequencer/DirectionModes.cpp` in Phase 7.1 precisely so that
    every `walk` / `randomNoRepeat` traversal table stays bit-identical across the
    migration. Changing anything here changes the sound of every saved project
    that uses a stochastic direction mode or a sub-100 % PROB step: that is an
    `rngVersion` bump plus a migration note plus a justified golden update
    (§5.2), never a refactor. */
constexpr std::uint64_t splitmix64 (std::uint64_t x) noexcept
{
    x += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// ─────────────────────────────────────────────────────────────────────────────
// THIS ENUM IS APPEND-ONLY, AND ITS VALUES ARE PART OF THE DETERMINISM CONTRACT.
//
// A domain constant is XORed into the seed so that two consumers driven by the
// SAME `masterSeed` are uncorrelated — a `walk` table and a `randomNoRepeat`
// table built from one seed must not march in step, and a PROB roll must not be
// a function of either. Because the constant is mixed into the seed, editing an
// existing entry's value silently rewrites every note that consumer ever
// produced. So:
//
//   * NEVER change an existing value. Add new domains at the END.
//   * NEVER reuse a retired value; leave the entry in place with a note.
//   * A new domain is free (nothing existing hashes through it); a changed one
//     is an `rngVersion` bump + golden regeneration (§5.2).
//
// The two `direction*` values are recorded at the EXACT literals the anonymous
// namespace of `DirectionModes.cpp` used before Phase 7.1 (`walkSalt = 0x5741`,
// `randomSalt = 0x9E37`), which is what makes the migration bit-identical by
// construction rather than by measurement. `0x5741` is ASCII 'W','A' (WAlk);
// `0x9E37` is the leading half-word of splitmix64's own gamma, chosen in Phase 6
// simply as a value far from the other one. New entries below follow the ASCII
// convention: `0x5052` = 'P','R' (PRobability), `0x5243` = 'R','C' (RatChet).
// ─────────────────────────────────────────────────────────────────────────────

/** Domain-separation salts. See the append-only rules above.

    THE `std::uint64_t` BASE TYPE IS DELIBERATE, and it is why this carries the
    repository's only clang-tidy suppression. `performance-enum-size` is right in
    general — ARPBOX stores enums (`LaneId`, `DirectionMode`, `TrigCondition`) in
    trivially-copyable PODs where every byte is snapshot memory, so the check
    stays ON globally — but it is wrong here on two counts:

      * NO `RngDomain` VALUE EVER EXISTS AT RUNTIME. It is a `constexpr` function
        argument that folds away entirely; there is no object whose size the
        narrower type would shrink.
      * The salt is XORed into a 64-bit seed, and this enum is APPEND-ONLY. A
        `std::uint16_t` base would permanently cap every FUTURE domain at 16 bits
        of separation — and since existing values can never be re-spaced, that
        ceiling could not be lifted later without an `rngVersion` bump. Trading a
        real constraint on future entropy for zero actual bytes is a bad deal.

    Narrow this only if `RngDomain` ever becomes a stored field, at which point
    the append-only rule makes it a versioned change anyway. */
// NOLINTNEXTLINE(performance-enum-size)
enum class RngDomain : std::uint64_t
{
    directionWalk = 0x5741ULL,           ///< §12.3 `walk` reflection bits (Phase 6, frozen literal).
    directionRandomNoRepeat = 0x9E37ULL, ///< §12.3 `randomNoRepeat` draws (Phase 6, frozen literal).
    stepProbability = 0x5052ULL,         ///< §5.1 L2 per-step PROB roll (Phase 7.1).
    ratchetProbability = 0x5243ULL       ///< §5.1 L2 per-ratchet-child probability (Phase 7.2).
};

/** The raw salt behind an `RngDomain`, for call sites that build the seed
    expression by hand (`DirectionModes.cpp` keeps its historical shape). */
constexpr std::uint64_t domainSalt (RngDomain domain) noexcept
{
    return static_cast<std::uint64_t> (domain);
}

/** THE per-step hash: a pure function of (`masterSeed`, `domain`, `stepIndex`).

    ── WHY TWO NESTED CALLS, AND WHY THAT IS NOT PARANOIA ──────────────────────
    The INNER call is literally §5.2's "effective stream seed":

        splitmix64 (masterSeed ⊕ operatorSeed ⊕ (loopLock ? 0 : barCounter))

    with the `operatorSeed` and bar-counter terms ELIDED — because in Phase 7
    both are ZERO, and `x ^ 0 == x`. That identity is the whole point: Phase 12
    can introduce per-operator seeds and LOOP LOCK by restoring the missing XOR
    terms, and every Phase-7 golden (whose operator seed is still 0 and whose
    loop lock still contributes 0) stays bit-identical. Collapsing the two calls
    into one would make the same extension a determinism break.

    The OUTER call is §12.3's per-index-hash idiom — the same construction
    `walk` already uses, and the reason the direction tables are pure functions
    of the ordinal rather than draws from a running stream.

    ── THERE IS DELIBERATELY NO `barCounter` TERM ──────────────────────────────
    `stepIndex` is the GLOBAL step index, so it already carries the loop: step 3
    of loop 0 and step 3 of loop 1 are different indices and therefore hash
    differently. Adding a bar counter here would double-count it. Phase 12's LOOP
    LOCK is the OPPOSITE edit — it folds the index back INTO the loop
    (`stepFloorMod (stepIndex, loopLength)`) so a roll repeats — and it belongs
    at the CALL SITE that chooses the index, not in this function.

    @param masterSeed  The pattern's seed (§8.1 `patterns[].masterSeed`).
    @param domain      Consumer identity; see the append-only note above.
    @param stepIndex   GLOBAL step index. Signed, and negative values are legal
                       (the retrigger lookahead and locate paths sweep below 0);
                       the conversion to `std::uint64_t` is modulo 2^64 and well
                       defined, so index -1 simply hashes as 0xFFFF'FFFF'FFFF'FFFF.
    @returns a well-mixed 64-bit word. Callers take `% n` or `& 1` from it. */
constexpr std::uint64_t stepHash (std::uint64_t masterSeed, RngDomain domain, std::int64_t stepIndex) noexcept
{
    return splitmix64 (splitmix64 (masterSeed ^ domainSalt (domain)) ^ static_cast<std::uint64_t> (stepIndex));
}

/** THE per-SUB-STEP hash: a pure function of (`masterSeed`, `domain`,
    `stepIndex`, `subIndex`) — for consumers that need several independent draws
    WITHIN one step, of which §5.1 L2's per-ratchet probability is the first.

    ── WHY NOT JUST ADVANCE A STREAM PER CHILD ─────────────────────────────────
    Because that is the failure mode this whole header exists to prevent, one
    level down. The retrigger lookahead evaluates whole steps — children included
    — for up to nine surrounding indices per emitted note, and how many of those
    it reaches depends on note lengths and on the block carving. A stream's output
    depends on its pull COUNT, so the lookahead's prediction of which children
    exist would disagree with the eventual emission, and note-off placement would
    become a function of call count: the #36/#46/#48 buffer-size-dependent family
    one level up. A hash of the (step, sub) PAIR has no such dependence.

    ── WHY NOT FOLD `subIndex` INTO `stepIndex` ────────────────────────────────
    `stepHash (seed, domain, stepIndex * 8 + subIndex)` would also be pure, and it
    would hard-code the RATCHET ceiling into the seed composition — so raising
    §12.1's RATCHET range later would re-key every existing pattern's rolls. It
    would also make step k's child 1 hash identically to step k+1's child... no
    fixed offset, but the two index spaces would interleave, which is a
    correlation nobody wants to reason about. The extra `splitmix64` round is one
    multiply-shift chain and buys independence of both index spaces.

    THE OUTER ROUND IS THE SAME CONSTRUCTION `stepHash` USES on its own inner
    call, so the idiom is one idiom throughout.

    @param masterSeed  The pattern's seed (§8.1 `patterns[].masterSeed`).
    @param domain      Consumer identity; see the append-only note above.
    @param stepIndex   GLOBAL step index. Negative values are legal.
    @param subIndex    Position within the step (a ratchet child index).
    @returns a well-mixed 64-bit word. Callers take `% n` or `& 1` from it. */
constexpr std::uint64_t
subStepHash (std::uint64_t masterSeed, RngDomain domain, std::int64_t stepIndex, int subIndex) noexcept
{
    return splitmix64 (stepHash (masterSeed, domain, stepIndex) ^ static_cast<std::uint64_t> (subIndex));
}

/** RNG stream algorithm version, serialized as §8.1's top-level `rngVersion` key
    in `project.json`. A load that finds a LOWER version than this must migrate
    or refuse — never silently reinterpret, because the whole point of the key is
    that a saved project never changes sound (§1.2).

    VERSION HISTORY — append a line, never edit one:
      0  Phases 1-6. No RNG contract existed. The only randomness in the audible
         path was `DirectionModes`' local `splitmix64`, and no golden depended on
         it (`walk`/`randomNoRepeat` were deliberately left unbaked).
      1  Phase 7.1. This header becomes the single source: `splitmix64`, the
         `RngDomain` registry {directionWalk, directionRandomNoRepeat,
         stepProbability, ratchetProbability}, and `stepHash`. `DirectionModes`
         output is UNCHANGED across the 0 -> 1 boundary by construction (same
         function body, same salts), so no Phase-6 golden regenerates.
         Phase 7.2 ADDED `subStepHash` WITHOUT BUMPING THIS, deliberately: it is a
         new consumer of an existing primitive, reached only by ratchet children
         `c >= 1`, which cannot exist in an rngVersion-1 project (RATCHET was
         stored-but-unevaluated when 1 was assigned, so every 7.1 pattern has
         `noteCount == 1`). No existing seed's output moves, so there is nothing
         for a bump to protect. */
inline constexpr int rngVersion = 1;

// ─────────────────────────────────────────────────────────────────────────────
// THE LOAD-BEARING GUARD. Read this before deleting it.
//
// 0xE220A8397B1DCDAF is splitmix64's published first output for seed 0 — the
// canonical known-answer vector for this function. It is asserted HERE, at
// compile time, because NOTHING ELSE IN THE REPOSITORY WOULD CATCH A DRIFT:
//
//   * `walk` and `randomNoRepeat` were deliberately given NO golden files (see
//     the omission note at tests/determinism_goldens.cpp:41-55), so the six
//     baked goldens are entirely insensitive to this function.
//   * `tests/pattern_directions.cpp` covers the two stochastic modes with
//     PROPERTY tests only (period, ±1 steps, reflection, no-repeat across the
//     loop point). Every one of those properties survives a changed constant.
//
// So a typo'd shift or a mistyped multiplier would silently alter the sound of
// every stochastic pattern and every sub-100 % PROB step, and the suite would
// stay green. This assert plus tests/rng_primitive.cpp are the entire defence;
// the test file's fails-without demonstration is what proves that claim.
// ─────────────────────────────────────────────────────────────────────────────
static_assert (splitmix64 (0) == 0xE220A8397B1DCDAFULL,
               "splitmix64 has drifted from its canonical constants. This is a determinism-contract "
               "break, not a build error: walk/randomNoRepeat have no goldens and pattern_directions.cpp "
               "only checks properties, so NOTHING ELSE in the suite would have caught it. Restore the "
               "constants; changing them deliberately requires an rngVersion bump (ARCHITECTURE §5.2).");

static_assert (domainSalt (RngDomain::directionWalk) == 0x5741ULL,
               "Phase-6 walk salt must stay 0x5741 or every walk traversal table changes.");
static_assert (domainSalt (RngDomain::directionRandomNoRepeat) == 0x9E37ULL,
               "Phase-6 randomNoRepeat salt must stay 0x9E37 or every randomNoRepeat table changes.");
} // namespace arpbox::engine::rng
