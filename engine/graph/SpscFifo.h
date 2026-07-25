#pragma once

#include "../EngineGuiGuard.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace arpbox::engine
{
/** Fixed-capacity, lock-free, allocation-free single-producer/single-consumer
    queue of trivially-copyable POD items, built on `juce::AbstractFifo`.

    This is the ONE queue primitive underlying every cross-thread channel in the
    project (ARCHITECTURE §3.4): the UI→engine `EngineCommandQueue`, the
    engine→UI discrete `EngineEventQueue`, and the `RetirementQueue` used to hand
    heap objects back to the message thread. Do NOT invent a fourth channel type;
    reuse this.

    Threading contract (STRICT — SPSC):
      - Exactly ONE producer thread ever calls `push()`.
      - Exactly ONE consumer thread ever calls `drain()`.
      - `getNumReady()` / `getDroppedCount()` are observation-only and safe from
        either side (atomic reads); they are advisory, not synchronisation.
      The two roles may be either (producer = audio, consumer = message) or the
      reverse, depending on the channel — but the pairing is fixed per instance.

    `T` must be trivially copyable (enforced below): pushing copies bytes, so no
    constructor/destructor runs across the boundary and there is nothing to free
    on the audio thread.

    Capacity is a compile-time constant so the backing storage is a single inline
    `std::array` — no heap, ever. `AbstractFifo` treats the ring as holding up to
    `Capacity` in-flight items; size generously (queues here are 256–1024). */
template <typename T, std::size_t Capacity>
class SpscFifo
{
public:
    static_assert (std::is_trivially_copyable_v<T>,
                   "SpscFifo<T>: T must be trivially copyable (POD) — the queue "
                   "copies bytes across threads and never runs ctors/dtors.");
    static_assert (Capacity >= 2, "SpscFifo: capacity must be at least 2.");

    /** Constructs an empty queue with a fixed ring of `Capacity` slots. */
    SpscFifo () = default;

    // RT-SAFE: producer side. Lock-free, allocation-free, never blocks.
    /** Copies one item into the queue.

        @returns true if enqueued; false if the queue was full — in which case the
                 item is dropped and the dropped-count is incremented. Callers on
                 the audio thread must treat a false return as "backpressure", not
                 an error: never retry-spin and never allocate to grow. */
    bool push (const T& item) noexcept
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToWrite (1, start1, size1, start2, size2);

        if (size1 <= 0)
        {
            // Full: no room in either range. Drop and record it.
            droppedCount.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        storage[static_cast<std::size_t> (start1)] = item;
        fifo.finishedWrite (1);
        return true;
    }

    // RT-SAFE: consumer side (RT-safe only when `fn` itself is RT-safe).
    /** Applies `fn(const T&)` to every queued item in FIFO order, then frees the
        consumed slots. Handles `AbstractFifo`'s two-range wrap internally.

        `Fn` is taken by forwarding reference and invoked in place — there is NO
        `std::function` on this path, so no type-erasure allocation. When the
        consumer is the audio thread, `fn` MUST itself be allocation-free and
        lock-free (the queue guarantees nothing about what you do per item). */
    template <class Fn>
    void drain (Fn&& fn) noexcept
    {
        const int ready = fifo.getNumReady ();
        if (ready <= 0)
            return;

        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead (ready, start1, size1, start2, size2);

        for (int i = 0; i < size1; ++i)
            fn (std::as_const (storage[static_cast<std::size_t> (start1) + static_cast<std::size_t> (i)]));

        for (int i = 0; i < size2; ++i)
            fn (std::as_const (storage[static_cast<std::size_t> (start2) + static_cast<std::size_t> (i)]));

        fifo.finishedRead (size1 + size2);
    }

    // RT-SAFE: observation-only, callable from either side.
    /** Number of items currently available to the consumer (advisory snapshot). */
    int getNumReady () const noexcept { return fifo.getNumReady (); }

    // RT-SAFE: observation-only, callable from either side.
    /** Total number of items ever dropped by `push()` because the queue was full.
        Monotonic; useful for diagnostics / an overflow indicator in the UI. */
    std::uint64_t getDroppedCount () const noexcept { return droppedCount.load (std::memory_order_relaxed); }

private:
    juce::AbstractFifo fifo { static_cast<int> (Capacity) };
    std::array<T, Capacity> storage {};
    std::atomic<std::uint64_t> droppedCount { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpscFifo)
};
} // namespace arpbox::engine
