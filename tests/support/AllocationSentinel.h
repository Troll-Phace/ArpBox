#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// AllocationSentinel — the allocation-guard harness (ARCHITECTURE §11: "Audio-
// thread allocations in steady state = 0, asserted by an operator-new guard";
// .claude/rules/testing.md "allocation guards (overridden operator new counter)
// assert zero allocations inside processBlock during steady-state tests").
//
// The global `operator new`/`operator delete` family is REPLACED in the test
// binary only (tests/support/AllocationCounter.cpp — a replacement in the main
// executable wins at link on macOS). NEVER add that override to the shipped
// engine/app libraries. Each replaced `new`/`new[]` bumps a THREAD-LOCAL counter,
// but ONLY while an `armed` flag (also thread-local) is set; `delete` frees and,
// while armed, bumps a free counter too.
//
// CRITICAL USAGE SEMANTICS (read before trusting a result):
//   • The counter is THREAD-LOCAL. Arm it on the SAME thread that runs the
//     measured `processBlock` loop; allocations on any other thread (Catch2's
//     reporter, worker threads, the message thread) are NOT counted.
//   • Arm AFTER warmup blocks. JUCE lazily allocates render buffers / scratch on
//     the first few `prepareToPlay`/`processBlock` calls; those are legitimate
//     one-time setup, not a steady-state leak. Warm up, THEN arm.
//   • Everything the armed region MEASURES must be pre-allocated first: the
//     AudioBuffer, MidiBuffer, and any command payloads are built BEFORE the
//     sentinel is constructed. Do NOT call Catch2 REQUIRE/SECTION or build a
//     juce::String inside the armed region — they allocate and would be counted.
//   • Read the delta (`allocations()`) INSIDE the region into a plain integer,
//     then assert on that integer AFTER the sentinel is destroyed.
//
// Not re-entrant / not for nested use: an inner sentinel's destructor disarms the
// outer one. Use one sentinel per measured region.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>

namespace arpbox::test
{
// Implemented in AllocationCounter.cpp against file-local thread_locals.

/** Arms or disarms allocation counting on the CALLING thread. */
void setAllocationCounterArmed (bool armed) noexcept;

/** True if allocation counting is currently armed on the calling thread. */
bool isAllocationCounterArmed () noexcept;

/** Monotonic count of `new`/`new[]` calls made on the calling thread WHILE armed
    (never decreases; only advances during armed regions). */
std::uint64_t allocationCount () noexcept;

/** Monotonic count of `delete`/`delete[]` calls made on the calling thread WHILE
    armed. Advisory — the steady-state contract is about allocations, not frees. */
std::uint64_t deallocationCount () noexcept;

/** Scoped RAII guard: arms the thread-local allocation counter on construction,
    disarms on destruction, and exposes the allocation/deallocation delta measured
    across its lifetime.

    Typical use (see AllocationSentinel.h header notes for the full rules):
    @code
        // ... warmup blocks, buffers pre-allocated ...
        std::uint64_t allocs = 0;
        {
            AllocationSentinel sentinel;
            for (int i = 0; i < M; ++i)
                graph.getProcessor().processBlock (buffer, midi);
            allocs = sentinel.allocations();
        }
        REQUIRE (allocs == 0);
    @endcode */
class AllocationSentinel
{
public:
    /** Captures the current counts and arms counting on this thread. */
    AllocationSentinel () noexcept
        : startAllocs (allocationCount ())
        , startFrees (deallocationCount ())
    {
        setAllocationCounterArmed (true);
    }

    /** Disarms counting on this thread. */
    ~AllocationSentinel () noexcept { setAllocationCounterArmed (false); }

    AllocationSentinel (const AllocationSentinel&) = delete;
    AllocationSentinel& operator= (const AllocationSentinel&) = delete;

    /** Number of `new`/`new[]` calls on this thread since construction. */
    std::uint64_t allocations () const noexcept { return allocationCount () - startAllocs; }

    /** Number of `delete`/`delete[]` calls on this thread since construction. */
    std::uint64_t deallocations () const noexcept { return deallocationCount () - startFrees; }

private:
    std::uint64_t startAllocs;
    std::uint64_t startFrees;
};
} // namespace arpbox::test
