#pragma once

#include "../EngineGuiGuard.h"

#include <array>
#include <cstdint>

namespace arpbox::engine
{
/** Maximum notes the L0 note pool can hold (ARCHITECTURE §5.1 "L0 — NOTE POOL").

    LIVES HERE, NOT IN PatternTypes.h, ON PURPOSE. `PatternSetState` owns a
    `PoolSnapshot` by value, so `sequencer/PatternTypes.h` includes THIS header;
    if the constant lived there, this header would have to include it back and
    the two would be mutually dependent. The dependency edge runs
    sequencer → midi (matching §3.2's ownership split), so `engine/midi/` is the
    leaf and holds the pool vocabulary. Including `PatternTypes.h` still brings
    `maxPoolSize` into scope — same namespace, one include away.

    32 covers every realistic pool: a 10-finger THRU chord, a SELF-mode
    root+scale+degree stack (§5.1), and the 8-slot chord lane, with headroom. */
inline constexpr int maxPoolSize = 32;

/** An immutable, POD view of the note pool as the audio thread sees it for one
    block (ARCHITECTURE §5.1 L0). Built on the message thread (Phase 8's live
    THRU/SELF pool) or by Phase 6's stub builder, and read — never mutated — by
    the step tick.

    ── THE TWO VIEWS ───────────────────────────────────────────────────────────
    `sorted`   — pool notes in ASCENDING pitch order. Every direction mode in
                 §12.3 except `asPlayed` traverses this array.
    `asPlayed` — the same notes in ARRIVAL order (the order the player struck
                 them). `DirectionMode::asPlayed` traverses this array with the
                 plain `up` traversal order; see `usesAsPlayedView` in
                 sequencer/DirectionModes.h. It is a POOL-VIEW SELECTION, not a
                 distinct permutation, which is why there is no `asPlayed`
                 entry in the traversal-table generator's mode switch.

    IN PHASE 6 THE STUB BUILDER FILLS `sorted == asPlayed`, so `asPlayed`
    degenerates to `up` — which is correct, because a stub pool has no arrival
    history. Phase 8.1's live THRU pool makes the two arrays diverge with ZERO
    Phase 6 code change: the consumer already selects between them via
    `usesAsPlayedView`, so nothing downstream needs to learn a new concept.

    Both arrays hold MIDI note numbers 0..127 and are valid over
    `[0, size)`; entries at or beyond `size` are unspecified (value-initialized
    to 0 by default, but do not read them).

    Trivially copyable by construction, so it travels inside `PatternSetState`
    and any snapshot with no ctor/dtor across the boundary. */
struct PoolSnapshot
{
    std::uint8_t size = 0;                             ///< Live notes, 0..maxPoolSize.
    std::array<std::uint8_t, maxPoolSize> sorted {};   ///< Ascending pitch order.
    std::array<std::uint8_t, maxPoolSize> asPlayed {}; ///< Arrival order (Phase 8).
};

/** Floor division for signed integers — `std::floor (a / double (b))` without the
    float. C++ integer division truncates TOWARD ZERO, so `-1 / 3 == 0`; the
    octave carry below needs `-1` there instead. Precondition: `b > 0`. */
constexpr int floorDivInt (int a, int b) noexcept
{
    const int q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// RT-SAFE: audio thread. Pure arithmetic on caller-owned memory; no allocation,
// no branching on pool contents beyond the empty check.
/** The pool note at signed degree offset `k`, with octave carry — the standard
    arpeggiator degree walk (ARCHITECTURE §12.1 PITCH: "−24..+24 degrees", "Pool
    index offset").

    `k` outside `[0, size)` WRAPS AND TRANSPOSES:

        oct  = floorDivInt (k, size)
        note = notes[k - oct * size] + 12 * oct

    so degree `size` is the first pool note an octave up and degree `-1` is the
    last pool note an octave down. This is what makes the PITCH lane a musical
    control rather than an array index: the pattern keeps working when the held
    chord changes size.

    @param notes  `sorted` or `asPlayed` from a `PoolSnapshot` (see
                  `poolNotes`). Must have at least `size` valid entries.
    @param size   Live pool size. `<= 0` yields -1.
    @param k      Signed degree offset.
    @returns the MIDI note number, or -1 when the pool is empty. THE RESULT IS
             NOT RANGE-CLAMPED — a large `k` can exceed 0..127. Clamping is the
             constraint gate's job (§5.1 "Constraint gate", Phase 12.3), not
             this primitive's; folding here would silently defeat the gate's
             fold-vs-clamp setting. */
constexpr int poolNoteAtDegree (const std::uint8_t* notes, int size, int k) noexcept
{
    if (notes == nullptr || size <= 0)
        return -1;

    const int oct = floorDivInt (k, size);
    const int index = k - oct * size;

    return static_cast<int> (notes[index]) + 12 * oct;
}

// RT-SAFE: audio thread. Pure query.
/** Convenience overload traversing the ASCENDING (`sorted`) view. */
constexpr int poolNoteAtDegree (const PoolSnapshot& pool, int k) noexcept
{
    return poolNoteAtDegree (pool.sorted.data (), static_cast<int> (pool.size), k);
}

// RT-SAFE: audio thread. Pure query.
/** Selects which of the pool's two arrays a traversal should read.

    Resolve this ONCE when a snapshot is adopted, not once per step: the whole
    point of the pre-built traversal table (sequencer/DirectionModes.h) is that
    the per-step path is `notes[order[ordinal % period]]` with no mode branch in
    it. Pass `usesAsPlayedView (mode)` as `asPlayedView`. */
constexpr const std::uint8_t* poolNotes (const PoolSnapshot& pool, bool asPlayedView) noexcept
{
    return asPlayedView ? pool.asPlayed.data () : pool.sorted.data ();
}
} // namespace arpbox::engine
