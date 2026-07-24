#pragma once

#include "../EngineGuiGuard.h"
#include "EngineSnapshot.h"

#include <array>
#include <atomic>

namespace arpbox::engine
{
/** Lock-free, allocation-free TRIPLE buffer publishing `EngineSnapshot` from the
    audio thread (single writer) to the UI (single reader) — ARCHITECTURE §3.4,
    channel 2, state half.

    Why THREE slots (and not two)?
      With two buffers the writer must wait for, or race with, the reader to reuse
      a slot: if the reader is mid-read of the only spare, the writer either
      blocks (forbidden on the audio thread) or overwrites what the reader is
      looking at (tearing). A third slot breaks that dependency — at every instant
      each of the three slots is owned by exactly ONE party:
        • one the writer is filling            (`writeSlot`, writer-private)
        • one the reader is looking at          (`readSlot`,  reader-private)
        • one "most-recently-published" middle  (encoded in the `ready` atomic)
      The writer can always publish into the middle and reclaim the previous
      middle to fill next, without ever touching the reader's slot. The reader
      picks up the middle only when it is marked fresh. No writer ever waits.

    Single-owner invariant (the reason this is TSan-clean):
      `{ writeSlot, readSlot, indexOf(ready) }` is ALWAYS a permutation of
      {0,1,2}. Ownership of a slot transfers ONLY through the atomic exchange on
      `ready`, which also carries the acquire/release edge that publishes the
      slot's bytes. Because a slot is single-owner between exchanges, the actual
      `EngineSnapshot` memory is never written by one thread while read by the
      other → no data race on the payload.

    NO locks, NO allocation, NO retirement queue: the three slots are fixed inline
    storage reused forever. (The heap-object hand-back mechanism is the separate
    `RetirementQueue`, used by Phase 6's `PatternSnapshot`, NOT here.) */
class EngineSnapshotBuffer
{
public:
    /** Constructs with all three slots zero-initialised and no fresh data yet;
        an early `read()` returns a valid all-zero snapshot. */
    EngineSnapshotBuffer () = default;

    // Non-copyable / non-movable: the atomic control word and the two private
    // slot indices are meaningful only in place (the audio and message threads
    // hold a stable reference to this object).
    EngineSnapshotBuffer (const EngineSnapshotBuffer&) = delete;
    EngineSnapshotBuffer& operator= (const EngineSnapshotBuffer&) = delete;

    // RT-SAFE: writer (audio thread). Lock-free, allocation-free.
    /** Returns the writer-owned slot to fill IN PLACE, avoiding a full-struct
        copy. Mutate the returned reference, then call `commit()` to publish.
        The reference stays valid and writer-exclusive until `commit()`. */
    EngineSnapshot& beginWrite () noexcept
    {
        return slots[static_cast<std::size_t> (writeSlot)];
    }

    // RT-SAFE: writer (audio thread). Lock-free, allocation-free, wait-free.
    /** Atomically publishes the slot last returned by `beginWrite()` as the new
        "most recent", and adopts the previous middle slot as the next write
        target.

        Ordering is `acq_rel` because this exchange transfers ownership in BOTH
        directions in one operation: the RELEASE half publishes the bytes just
        written into `writeSlot` (a later reader that acquires this slot sees
        them), and the ACQUIRE half is required to reclaim the previous middle
        slot — that slot may be one the READER just relinquished, and its release
        of that slot (in `read()` below) must happen-before this writer starts
        overwriting the slot's bytes next block. A plain `release` here would
        order the publish but NOT the reuse, leaving the writer's next write to
        those bytes racing the reader's prior read of them (TSan-confirmed
        slot-reuse race). */
    void commit () noexcept
    {
        const int published = writeSlot | freshBit;
        const int previous = ready.exchange (published, std::memory_order_acq_rel);
        writeSlot = previous & indexMask; // reclaim the old middle to fill next
    }

    // MESSAGE-THREAD ONLY (single reader). Lock-free, allocation-free.
    /** Returns the newest committed snapshot if one has arrived since the last
        `read()`, otherwise the same slot returned last time. Never returns a
        torn/half-written value.

        The adopting exchange is `acq_rel` for the same bidirectional-ownership
        reason as `commit()`: the ACQUIRE half synchronises with the writer's
        release so this thread sees the fully-written bytes of the slot it is
        taking; the RELEASE half publishes that we are RELINQUISHING our old
        `readSlot` — after which the writer may reclaim and overwrite it — so our
        completed reads of that slot happen-before the writer's future writes to
        it. Plain `acquire` would not order the relinquish and would let the
        writer's reuse race our just-finished read. The returned reference is
        reader-exclusive until the next `read()`. */
    const EngineSnapshot& read () noexcept
    {
        // Only swap when the middle slot is flagged fresh; otherwise keep ours.
        if ((ready.load (std::memory_order_acquire) & freshBit) != 0)
        {
            // Hand our stale slot to the middle (clearing fresh) and take the
            // fresh one. exchange returns the current middle atomically, so even
            // if the writer publishes again between the load and here we still
            // adopt the latest published index.
            const int previous = ready.exchange (readSlot, std::memory_order_acq_rel);
            readSlot = previous & indexMask;
        }

        return slots[static_cast<std::size_t> (readSlot)];
    }

private:
    // Low 2 bits hold a slot index (0..2); bit 2 flags the middle slot as fresh
    // (has data the reader has not yet adopted).
    static constexpr int indexMask = 0x3;
    static constexpr int freshBit = 0x4;

    std::array<EngineSnapshot, 3> slots {};

    int writeSlot = 0; ///< Writer-private: slot currently being filled.
    int readSlot = 1;  ///< Reader-private: slot currently being read.

    // Middle slot index (+ fresh flag). Starts at slot 2, not fresh. This is the
    // ONLY shared state; ownership of a slot transfers only via exchange on it.
    std::atomic<int> ready { 2 };
};
} // namespace arpbox::engine
