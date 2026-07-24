#pragma once

#include "../EngineGuiGuard.h"
#include "SpscFifo.h"

#include <cstddef>

namespace arpbox::engine
{
/** SPSC hand-back channel for message-thread reclamation of heap objects the
    audio thread has finished with (ARCHITECTURE §3.4, channel 3; §3.4 threading
    table "Retired snapshots are returned to the message thread for deletion —
    never freed on the audio thread").

    THE tested seam for Phase 6's `PatternSnapshot`: the message thread builds an
    immutable snapshot on the heap, publishes it by atomic pointer swap, and the
    audio thread adopts it at a quantize boundary. When a snapshot is superseded,
    the audio thread `retire()`s the OLD pointer here (RT-safe: just enqueues a
    raw pointer, no `delete`), and the message thread later `reclaim()`s and
    frees them. Freeing on the audio thread is a defect even if it "works"; this
    queue exists to make that impossible by construction.

    Note the deliberate contrast with `EngineSnapshotBuffer`: that triple buffer
    reuses three FIXED inline slots and therefore has NOTHING to retire. Only
    heap-allocated, pointer-swapped objects use this queue.

    Storage: a fixed-capacity `SpscFifo<T*, Capacity>` — enqueuing a retirement
    is allocation-free and lock-free. `Capacity` bounds how many retired objects
    can be in flight before the message thread drains; 256 is ample because the
    producer retires at most one per quantize boundary and the consumer reclaims
    every UI tick. A dropped retirement (queue full) would LEAK rather than
    crash; `getDroppedCount()` surfaces that (should stay 0 in practice).

    Ownership: `retire()` transfers ownership of the pointer to this queue;
    `reclaim()` takes ownership and `delete`s. `T` is deleted through its own
    type, so `T` needs a public (typically virtual) destructor. */
template <typename T, std::size_t Capacity = 256>
class RetirementQueue
{
public:
    /** Constructs an empty queue. */
    RetirementQueue () = default;

    // RT-SAFE: producer (audio thread). Enqueues a pointer only — never deletes.
    /** Hands `ptr` to the message thread for later deletion.

        @returns true if enqueued; false if the queue was full (the object is NOT
                 freed here — that would defeat the purpose — so a false return
                 means a leak, tracked via `getDroppedCount()`). Passing nullptr
                 is allowed and simply enqueues nothing meaningful; prefer not to. */
    bool retire (T* ptr) noexcept
    {
        if (ptr == nullptr)
            return true;
        return fifo.push (ptr);
    }

    // MESSAGE-THREAD ONLY: consumer. Deletes every pending retired object.
    /** Drains all queued pointers and `delete`s them. Call periodically on the
        message thread (e.g. once per UI tick or per topology edit). Safe to call
        when empty (no-op). */
    void reclaim () noexcept
    {
        fifo.drain ([] (T* const& ptr) { delete ptr; });
    }

    // RT-SAFE: observation-only.
    /** Number of retired objects awaiting reclamation (advisory). */
    int getNumPending () const noexcept { return fifo.getNumReady (); }

    // RT-SAFE: observation-only.
    /** Count of retirements dropped because the queue was full — each is a leaked
        object and should never occur in a correctly-sized system. */
    std::uint64_t getDroppedCount () const noexcept { return fifo.getDroppedCount (); }

private:
    SpscFifo<T*, Capacity> fifo;
};
} // namespace arpbox::engine
