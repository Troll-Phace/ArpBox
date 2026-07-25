#pragma once

#include "../EngineGuiGuard.h"
#include "../midi/NotePool.h"
#include "DirectionModes.h"
#include "PatternTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace arpbox::engine
{
// ─────────────────────────────────────────────────────────────────────────────
// PatternSnapshot.h — the IMMUTABLE pattern object the audio thread reads
// (ARCHITECTURE §3.4 mechanism 3, §4 step 3, §5.1 L1).
//
// Built on the message thread by `buildPatternSnapshot` from a `PatternSetState`,
// published through `PatternChannel` by atomic pointer swap, adopted by the audio
// thread at a quantize boundary, and handed back for deletion via the retirement
// queue. Nothing here is ever mutated after the build returns.
//
// ── TRIVIALLY COPYABLE, AND NOT POLYMORPHIC ─────────────────────────────────
// No virtuals, no pointers, no owning members — every table is an inline
// `std::array`. Two consequences worth stating out loud:
//
//   * `RetirementQueue<const PatternSnapshot>` deletes through the EXACT type
//     (`delete` on a `const PatternSnapshot*`), so the non-virtual trivial
//     destructor is correct. graph/RetirementQueue.h says `T` "needs a public
//     (typically virtual) destructor" — that parenthetical is about POLYMORPHIC
//     payloads. This one is not, deliberately, and this is the deviation note.
//   * A snapshot can be memcpy'd, stack-copied and compared byte-for-byte, which
//     is what lets the determinism suite treat "same snapshot" as a byte
//     question rather than a semantic one.
//
// ── D1: ONE PUBLISHED OBJECT HOLDS ALL 16 PATTERNS ──────────────────────────
// A pattern SWITCH (§6.1, quantized) is then an `int` change on the audio thread:
// no pointer swap, no retirement, no lifetime question at a switch boundary. A
// pointer swap happens only on a document EDIT. That collapses two independent
// lifetime problems into one, and it is why `PatternSnapshot` is ~100 KB rather
// than ~6 KB — the size buys the absence of a whole class of race.
// ─────────────────────────────────────────────────────────────────────────────

/** Longest GATE cycle in base steps: `length * division` at both maxima. Sizes
    the gate-prefix table. */
inline constexpr int maxGatePeriodSteps = maxSteps * maxLaneDivision;
static_assert (maxGatePeriodSteps == 512, "64 steps at division 8 is the longest GATE cycle.");

/** Pool sizes a traversal set covers: 0 through `maxPoolSize` INCLUSIVE. */
inline constexpr int numPoolSizes = maxPoolSize + 1;

/** One (`mode`, `seed`) pair's pool-traversal tables, precomputed for EVERY pool
    size (ARCHITECTURE §12.3; see sequencer/DirectionModes.h for the orders).

    ── WHY EVERY POOL SIZE, NOT JUST THE CURRENT ONE ───────────────────────────
    Phase 8 insurance, and it is not speculative. In THRU mode the pool size
    changes on the AUDIO THREAD as the player adds and removes held notes; a
    table built only for "the pool size at snapshot-build time" would need to be
    rebuilt from the audio thread the instant a finger moves, which is impossible
    inside the RT rules. Indexing by pool size turns that into a lookup:
    `order[poolSize]`, cost zero, no rebuild, no allocation, no snapshot swap.
    ~4.3 KB per set is the entire price.

    ── AND WHY SETS ARE DEDUPLICATED ACROSS THE 16 PATTERNS ────────────────────
    A traversal set is a pure function of (`mode`, `seed`) — nothing else in a
    pattern touches it. Real projects run 1–3 distinct pairs across all 16
    patterns, so deduplicating turns a ~68 KB / ~68,000-entry build into a
    ~4–13 KB / few-thousand-entry one. That matters because a piano-roll DRAG
    rebuilds the whole snapshot on every mouse move (§4 "Message-thread edit
    flow"), and the traversal tables are otherwise the dominant term. */
struct TraversalSet
{
    DirectionMode mode = DirectionMode::up; ///< The mode this set was built for.
    std::uint64_t seed = 0;                 ///< Seeds `walk` / `randomNoRepeat` only.

    /** `period[n]` = entries valid in `order[n]`, or 0 for an empty pool. */
    std::array<std::uint16_t, numPoolSizes> period {};

    /** `order[n][k]` = pool index for traversal ordinal `k`, `k < period[n]`. */
    std::array<std::array<std::uint8_t, direction::maxTraversalPeriod>, numPoolSizes> order {};
};

/** One pattern as the audio thread reads it: the §12.1 lanes, already range-
    clamped, plus the derived tables that make the step tick O(1) and STATELESS. */
struct PatternData
{
    /** Range-clamped lane data, indexed by `LaneId` (§12.1). */
    std::array<LaneState, numLanes> lanes {};

    /** Pool traversal mode (§12.3), carried for provenance / diagnostics; the RT
        path reads the resolved tables below, never this. */
    DirectionMode direction = DirectionMode::up;

    /** `direction::usesAsPlayedView (direction)`, resolved ONCE at build time so
        the step tick picks a pool array with `poolNotes (pool, asPlayedView)` and
        never branches on the mode. */
    bool asPlayedView = false;

    /** Index into `PatternSnapshot::traversalSets` (see the dedup note there). */
    std::uint16_t traversalSetIndex = 0;

    // ── THE GATE-PREFIX TABLE ────────────────────────────────────────────────
    // This is the mechanism that keeps the pool cursor a PURE FUNCTION of the
    // global step index under the gated-cursor rule (see `gatedOrdinal`).

    /** `gate.length * gate.division` — the GATE lane's full cycle in base steps,
        always >= 1 and <= `maxGatePeriodSteps`. */
    std::int32_t gatePeriodSteps = 1;

    /** Gated base steps in ONE full gate cycle. Zero for an all-off GATE lane. */
    std::int32_t gatePulsesPerLoop = 0;

    /** EXCLUSIVE prefix sum: `gatePrefixPulses[p]` = gated base steps STRICTLY
        BEFORE `p`, for `p` in `[0, gatePeriodSteps)`. Exclusive, so the first
        gated step of the cycle gets ordinal 0 and therefore `order[...][0]`.

        A base step `p` is gated iff
        `p % gate.division == 0 && gate.values[p / gate.division] != 0`. */
    std::array<std::uint16_t, maxGatePeriodSteps> gatePrefixPulses {};
};

/** The immutable pattern set the audio thread reads for one adoption period
    (ARCHITECTURE §3.4 mechanism 3). Build with `buildPatternSnapshot`; publish
    with `PatternChannel`. */
struct PatternSnapshot
{
    /** All 16 patterns (D1 above). */
    std::array<PatternData, maxPatterns> patterns {};

    /** Deduplicated traversal sets; only `[0, numTraversalSets)` are built. */
    std::array<TraversalSet, maxPatterns> traversalSets {};

    /** Live entries in `traversalSets`, 1..`maxPatterns`. */
    std::int32_t numTraversalSets = 0;

    /** One pattern step in quarter notes (§8.1 `transport.grid`). PROJECT-LEVEL,
        not per pattern — see the note on `PatternState` in PatternTypes.h. */
    double gridStepPpq = 0.25;

    /** Pattern active at transport start, 0..`maxPatterns`-1. */
    std::int32_t startPatternIndex = 0;

    /** MIDI channel the engine emits on, 1..16 (§8.1). */
    std::int32_t outputChannel = 1;

    /** The L0 note pool view (§5.1). Stub-filled in Phase 6; live in Phase 8. */
    PoolSnapshot pool {};

    /** Monotonic build id from the producing document — diagnostics and test
        identity ("did the audio thread actually adopt build N?"). Never affects
        the emitted MIDI. */
    std::uint64_t buildCounter = 0;

    // ── AUDIO-THREAD READERS ─────────────────────────────────────────────────
    // Static where they can be, so the audio thread, the offline render pass
    // (§9 MIDI drag-out) and the tests all run ONE implementation. Everything
    // below is a pure function: no state, no allocation, no locks.

    // RT-SAFE: audio thread. Delegates to PatternTypes.h.
    /** THE polymeter index (§5.1 L1). The formula itself lives in
        `arpbox::engine::laneIndex` (PatternTypes.h) and is not duplicated here. */
    static int laneIndex (const LaneState& lane, std::int64_t globalStep) noexcept
    {
        return arpbox::engine::laneIndex (lane, globalStep);
    }

    // RT-SAFE: audio thread. Delegates to PatternTypes.h.
    /** The lane's held value at `globalStep` (value-hold semantics). */
    static std::int16_t laneValueAt (const LaneState& lane, std::int64_t globalStep) noexcept
    {
        return arpbox::engine::laneValueAt (lane, globalStep);
    }

    // RT-SAFE: audio thread. Delegates to PatternTypes.h.
    /** True on the lane's own division tick. Only GATE gates on this — see the
        division-semantics note on `arpbox::engine::isLaneTick`. */
    static bool isLaneTick (const LaneState& lane, std::int64_t globalStep) noexcept
    {
        return arpbox::engine::isLaneTick (lane, globalStep);
    }

    // RT-SAFE: audio thread. Pure indexing, index clamped.
    /** Pattern `index`, clamped into `[0, maxPatterns)`. */
    const PatternData& pattern (int index) const noexcept
    {
        const int clamped = index < 0 ? 0 : (index >= maxPatterns ? maxPatterns - 1 : index);
        return patterns[static_cast<std::size_t> (clamped)];
    }

    // RT-SAFE: audio thread. Pure indexing, index clamped.
    /** The traversal set `data` was built against. */
    const TraversalSet& traversalSetFor (const PatternData& data) const noexcept
    {
        const int index = static_cast<int> (data.traversalSetIndex);
        const int clamped = index < 0 ? 0 : (index >= maxPatterns ? maxPatterns - 1 : index);
        return traversalSets[static_cast<std::size_t> (clamped)];
    }

    // RT-SAFE: audio thread. One divide, one multiply, one table read.
    /** Whether base step `globalStep` fires at all, per the GATE lane's two
        rules: it must land on GATE's division tick, and the held gate value must
        be non-zero. This is the SAME predicate the prefix table was summed from,
        so `isGated` and `gatedOrdinal` cannot drift apart. */
    static bool isGated (const PatternData& data, std::int64_t globalStep) noexcept
    {
        const LaneState& gate = data.lanes[static_cast<std::size_t> (LaneId::gate)];
        return isLaneTick (gate, globalStep) && laneValueAt (gate, globalStep) != 0;
    }

    // RT-SAFE: audio thread. One floor-divide plus one table read. No state.
    /** How many gated steps precede `globalStep` — i.e. WHICH pool note this step
        gets. Zero-based: the first gated step of the timeline returns 0.

        ── WHY THIS IS A TABLE LOOKUP AND NOT A COUNTER ────────────────────────
        The pool cursor advances on GATED STEPS ONLY (hardware-arpeggiator
        semantics: a rest thins the rhythm, it does not transpose everything
        after it). Naively that is a running counter — which is exactly the
        per-step mutable state that would destroy §1.2's determinism contract and
        §9's "offline render is bit-identical to real-time", because a counter's
        value depends on where playback STARTED, not on the music. The GATE lane
        is periodic, so the count is closed-form:

            loop    = floorDiv (g, gatePeriodSteps)
            ordinal = loop * gatePulsesPerLoop + gatePrefixPulses[g - loop * period]

        Jump the transport anywhere, render in any block carving, render offline:
        the same step index always yields the same pool note.

        ── PHASE 7, READ THIS BEFORE YOU "FIX" IT ──────────────────────────────
        THE ORDINAL IS DEFINED BY THE GATE LANE ALONE. When Phase 7.1 adds PROB
        and the §12.2 trig conditions, a step that is gated but probabilistically
        SUPPRESSED still consumes its ordinal — it does not shift the pool
        traversal for every step after it. That is deliberate: it means the
        arpeggio's pitch sequence stays deterministic and loop-stable while its
        rhythm becomes stochastic, which is what §5.2's LOOP LOCK and the golden
        suite both assume. Folding PROB/COND into the ordinal would make the pool
        cursor depend on RNG draw count, i.e. on block carving — the precise
        failure this table exists to prevent. If a "probability also advances the
        arp" mode is ever wanted, it is a new option with its own goldens, not an
        edit to this function.

        Returns 0 for an all-off GATE lane (nothing fires, so nothing reads it). */
    std::int64_t gatedOrdinal (const PatternData& data, std::int64_t globalStep) const noexcept
    {
        const std::int64_t period = data.gatePeriodSteps > 0 ? data.gatePeriodSteps : 1;
        const std::int64_t loop = stepFloorDiv (globalStep, period);
        const std::int64_t phase = globalStep - loop * period;

        return loop * static_cast<std::int64_t> (data.gatePulsesPerLoop) +
               static_cast<std::int64_t> (data.gatePrefixPulses[static_cast<std::size_t> (phase)]);
    }

    // RT-SAFE: audio thread. One floor-modulus plus two table reads.
    /** The pool INDEX (not the note) this pattern plays at `globalStep`, for a
        live pool of `poolSize` notes.

        Combines `gatedOrdinal` with the pre-built traversal table:
        `order[poolSize][floorMod (ordinal, period[poolSize])]`. Feed the result
        to `poolNoteAtDegree (poolNotes (pool, data.asPlayedView), poolSize,
        index + pitchDegrees)` to get the MIDI note.

        @returns the pool index, or -1 when the pool is empty / the traversal is
                 degenerate — in which case the caller MUST NOT emit a note. */
    int poolIndexAt (const PatternData& data, std::int64_t globalStep, int poolSize) const noexcept
    {
        if (poolSize <= 0)
            return -1;

        const int clampedSize = poolSize > maxPoolSize ? maxPoolSize : poolSize;
        const TraversalSet& set = traversalSetFor (data);
        const std::int64_t period = set.period[static_cast<std::size_t> (clampedSize)];

        if (period <= 0)
            return -1;

        const std::int64_t k = stepFloorMod (gatedOrdinal (data, globalStep), period);

        return static_cast<int> (set.order[static_cast<std::size_t> (clampedSize)][static_cast<std::size_t> (k)]);
    }
};

static_assert (std::is_trivially_copyable_v<PatternSnapshot>,
               "PatternSnapshot must be trivially copyable: it is published by raw pointer, "
               "deleted through its exact type by RetirementQueue, and compared byte-for-byte "
               "by the determinism suite.");
static_assert (std::is_trivially_destructible_v<PatternSnapshot>,
               "PatternSnapshot is deleted through its exact (non-polymorphic) type — see the "
               "RetirementQueue deviation note at the top of this header.");

// MESSAGE-THREAD ONLY: allocates the snapshot and runs the table precompute.
// NEVER call this from processBlock.
/** Builds an immutable `PatternSnapshot` from the document's `PatternSetState`.

    What the build does that the audio thread therefore does not:
      * clamps every lane value into its §12.1 range via `clampLaneValue`, and
        every `length` / `division` into `[1, maxSteps]` / `[1, maxLaneDivision]`,
        so no out-of-range datum can reach the RT path;
      * sums each pattern's GATE lane into its exclusive prefix table;
      * builds the deduplicated `(mode, seed)` traversal sets for every pool size.

    @param state         The document's authoritative state (copied, not retained).
    @param buildCounter  Monotonic id stamped onto the snapshot for diagnostics.
    @returns a heap-allocated snapshot, never null. Ownership passes to
             `PatternChannel::publish`. */
std::unique_ptr<const PatternSnapshot> buildPatternSnapshot (const PatternSetState& state, std::uint64_t buildCounter);
} // namespace arpbox::engine
