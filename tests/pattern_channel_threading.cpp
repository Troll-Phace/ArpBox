// ─────────────────────────────────────────────────────────────────────────────
// pattern_channel_threading — ARCHITECTURE §3.4's third cross-thread mechanism
// under two REAL threads (INSTRUCTIONS Phase 6 success criterion "Retired
// snapshots freed on message thread only (TSan clean)").
//
// ── THAT CRITERION IS TWO CLAIMS, AND TSAN PROVES ONLY ONE OF THEM ──────────
// "TSan clean" says the publish/adopt/retire handoff is free of data races. It
// says NOTHING about WHICH THREAD ran the destructor: an implementation that
// deleted retired snapshots on the audio thread under perfect synchronisation
// would be TSan-clean and would still violate code-style.md's hard rule ("Retired
// snapshots are returned to the message thread for deletion — never freed on the
// audio thread"), because the cost of that `free` is a malloc-lock acquisition
// inside a device callback — an audio dropout, not a race.
//
// So this file asserts both halves separately:
//
//   1. RACE-FREEDOM, on the real types. A message thread building and publishing
//      `PatternSnapshot`s against an audio thread adopting and retiring them, run
//      under the `tsan` preset. Anti-vacuity is explicit: the run must observe
//      more than one adoption of DISTINCT pointers and more than zero retirements
//      — a race-free run in which the audio thread never actually swapped would
//      pass trivially and prove nothing.
//
//   2. WHO DELETED IT, by recording `std::this_thread::get_id()` in the
//      destructor (the `Tracked` pattern from infra_snapshot.cpp) and requiring it
//      to equal the reclaiming thread's for EVERY deletion. `PatternSnapshot` is
//      trivially destructible by design and cannot carry that hook, so the probe
//      rides the same `RetirementQueue` template instantiated on a payload that
//      can — the deletion happens in `RetirementQueue::reclaim`, which is the code
//      under test either way. Case 3 closes the loop on the real type from the
//      other side: the audio thread performs zero `free`s at all.
//
// Threads are started and stopped with atomics and joins — no sleep is ever used
// as a synchronisation primitive.
// ─────────────────────────────────────────────────────────────────────────────

#include "support/AllocationSentinel.h"

#include "engine/graph/RetirementQueue.h"
#include "engine/sequencer/PatternChannel.h"
#include "engine/sequencer/PatternDocument.h"
#include "engine/sequencer/PatternSnapshot.h"
#include "engine/sequencer/PatternTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

using arpbox::engine::buildPatternSnapshot;
using arpbox::engine::LaneId;
using arpbox::engine::PatternChannel;
using arpbox::engine::PatternDocument;
using arpbox::engine::PatternSetState;
using arpbox::engine::PatternSnapshot;
using arpbox::engine::RetirementQueue;
using arpbox::test::AllocationSentinel;

namespace
{
/** A stable, comparable stand-in for `std::this_thread::get_id()` that fits an
    `std::atomic` on every platform. */
std::uint64_t currentThreadKey () noexcept
{
    return static_cast<std::uint64_t> (std::hash<std::thread::id> {}(std::this_thread::get_id ()));
}

/** The MOD A lane slot the snapshot-consistency marker is written into. Any lane
    would do; MOD A is stored-but-unread in Phase 6, so nothing else touches it. */
constexpr auto markerLane = static_cast<std::size_t> (LaneId::modA);

/** Destructor-instrumented payload for the "who deleted it" probe. Counts are
    static atomics so both threads can contribute and the test can assert exact
    totals after the joins. */
struct DeleteProbe
{
    ~DeleteProbe () noexcept
    {
        destroyed.fetch_add (1, std::memory_order_relaxed);

        if (currentThreadKey () != expectedThreadKey.load (std::memory_order_relaxed))
            wrongThreadDeletes.fetch_add (1, std::memory_order_relaxed);
    }

    int payload = 0;

    static std::atomic<int> destroyed;                   ///< Lifetime total destroyed.
    static std::atomic<int> wrongThreadDeletes;          ///< Deletions off the expected thread.
    static std::atomic<std::uint64_t> expectedThreadKey; ///< The reclaiming (message) thread.
};

std::atomic<int> DeleteProbe::destroyed { 0 };
std::atomic<int> DeleteProbe::wrongThreadDeletes { 0 };
std::atomic<std::uint64_t> DeleteProbe::expectedThreadKey { 0 };
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// D1. Race-freedom on the real types (this is what the `tsan` preset runs)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/pattern-channel: concurrent publish, adopt and retire is race-free", "[unit]")
{
    // Message thread: build a snapshot stamped with a marker that is a pure
    // function of its `buildCounter`, publish it, then reclaim whatever came back.
    // Audio thread: adopt whatever is offered, check the marker still agrees with
    // the counter (a torn adoption would break that), and retire the displaced one.
    constexpr std::uint64_t total = 400;

    PatternDocument document;
    PatternChannel channel;

    std::atomic<bool> go { false };
    std::atomic<bool> producerDone { false };
    std::atomic<int> adoptions { 0 };
    std::atomic<int> distinctPointers { 0 };
    std::atomic<int> retirements { 0 };
    std::atomic<int> inconsistentReads { 0 };
    std::atomic<std::uint64_t> highestCounterSeen { 0 };

    std::thread audio (
        [&]
        {
            while (! go.load (std::memory_order_acquire))
            {
            } // spin until start (no sleep)

            const PatternSnapshot* held = nullptr;
            const PatternSnapshot* previous = nullptr;
            int localAdoptions = 0;
            int localDistinct = 0;
            int localRetirements = 0;
            int localInconsistent = 0;
            std::uint64_t localHighest = 0;

            const auto adoptOnce = [&]
            {
                if (! channel.adopt (held))
                    return;

                ++localAdoptions;

                if (previous != nullptr)
                    ++localRetirements; // `adopt` handed the previous one back

                if (held != previous)
                    ++localDistinct;
                previous = held;

                // THE torn-read check: the marker is a pure function of the counter,
                // so any mixture of two builds breaks it.
                const auto expectedMarker = static_cast<std::int16_t> (held->buildCounter % 128);
                if (held->patterns[0].lanes[markerLane].values[0] != expectedMarker)
                    ++localInconsistent;

                if (held->buildCounter > localHighest)
                    localHighest = held->buildCounter;
            };

            while (! producerDone.load (std::memory_order_acquire))
                adoptOnce ();
            adoptOnce (); // final sweep of whatever remained after producerDone

            // Teardown: the holder drops its snapshot with no replacement.
            if (held != nullptr)
            {
                channel.retire (held);
                ++localRetirements;
            }

            adoptions.store (localAdoptions, std::memory_order_relaxed);
            distinctPointers.store (localDistinct, std::memory_order_relaxed);
            retirements.store (localRetirements, std::memory_order_relaxed);
            inconsistentReads.store (localInconsistent, std::memory_order_relaxed);
            highestCounterSeen.store (localHighest, std::memory_order_relaxed);
        });

    std::thread message (
        [&]
        {
            while (! go.load (std::memory_order_acquire))
            {
            }

            PatternSetState state = document.state ();

            for (std::uint64_t i = 1; i <= total; ++i)
            {
                state.patterns[0].lanes[markerLane].values[0] = static_cast<std::int16_t> (i % 128);
                channel.publish (buildPatternSnapshot (state, i));
                channel.reclaim (); // drain what the audio thread handed back
            }

            producerDone.store (true, std::memory_order_release);
        });

    go.store (true, std::memory_order_release);
    message.join ();
    audio.join ();

    // Anything the audio thread retired after the producer stopped is still owed.
    channel.reclaim ();

    INFO ("adoptions " << adoptions.load () << ", distinct pointers " << distinctPointers.load () << ", retirements "
                       << retirements.load () << ", highest buildCounter " << highestCounterSeen.load ());

    // ── ANTI-VACUITY ─────────────────────────────────────────────────────────
    // A race-free run in which the audio thread never swapped is not evidence of
    // anything, so the swap has to be shown to have happened — repeatedly, on
    // DISTINCT pointers.
    REQUIRE (adoptions.load () > 1);
    REQUIRE (distinctPointers.load () > 1);
    REQUIRE (retirements.load () > 0);
    REQUIRE (highestCounterSeen.load () > 1);
    REQUIRE (highestCounterSeen.load () <= total);

    // ── THE INVARIANTS ───────────────────────────────────────────────────────
    REQUIRE (inconsistentReads.load () == 0);
    REQUIRE (channel.getNumPendingRetirements () == 0);
    REQUIRE (channel.getDroppedRetirementCount () == 0); // each drop would be a leak
    REQUIRE (channel.peekPending () == nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// D2. WHO deleted it — the half TSan cannot see
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/retirement: every retired object is destroyed on the reclaiming thread", "[unit]")
{
    // The audio thread only ever ENQUEUES a raw pointer; `reclaim` on the message
    // thread is the sole place a `delete` runs. This is that claim, measured rather
    // than reasoned about: the destructor records which thread ran it.
    constexpr int total = 3000;

    DeleteProbe::destroyed.store (0, std::memory_order_relaxed);
    DeleteProbe::wrongThreadDeletes.store (0, std::memory_order_relaxed);
    DeleteProbe::expectedThreadKey.store (currentThreadKey (), std::memory_order_relaxed);

    RetirementQueue<DeleteProbe, 256> queue;

    // Allocated up front on THIS (message) thread — the audio thread must never
    // allocate either.
    std::vector<DeleteProbe*> objects;
    objects.reserve (static_cast<std::size_t> (total));
    for (int i = 0; i < total; ++i)
        objects.push_back (new DeleteProbe { i });

    std::atomic<bool> go { false };
    std::atomic<bool> producerDone { false };
    std::atomic<int> retired { 0 };
    std::atomic<std::uint64_t> backPressureRetries { 0 };

    std::thread audio (
        [&]
        {
            while (! go.load (std::memory_order_acquire))
            {
            }

            int localRetired = 0;
            std::uint64_t localRetries = 0;

            for (auto* object : objects)
            {
                // Spin rather than drop: a dropped retirement is a LEAK by design,
                // and this case is about the destructor's thread, not about
                // overflow (which pattern_model_unit.cpp's capacity case covers).
                //
                // NOTE ON `getDroppedCount()`: `SpscFifo::push` bumps the dropped
                // counter on EVERY refusal, so a retrying producer inflates it —
                // here it counts back-pressure events, not lost objects. That is
                // only a test-harness artefact: production never retries (the audio
                // thread must not spin), so in the real channel one refusal really
                // is one leak. The exact accounting is asserted below so the
                // distinction is stated rather than papered over.
                while (! queue.retire (object))
                    ++localRetries;

                ++localRetired;
            }

            retired.store (localRetired, std::memory_order_relaxed);
            backPressureRetries.store (localRetries, std::memory_order_relaxed);
            producerDone.store (true, std::memory_order_release);
        });

    go.store (true, std::memory_order_release);

    // The message thread reclaims — and is therefore the only thread that deletes.
    while (! producerDone.load (std::memory_order_acquire))
        queue.reclaim ();
    queue.reclaim ();

    audio.join ();
    queue.reclaim (); // final sweep

    INFO ("retired " << retired.load () << ", destroyed " << DeleteProbe::destroyed.load () << ", off-thread "
                     << DeleteProbe::wrongThreadDeletes.load () << ", back-pressure retries "
                     << backPressureRetries.load ());

    REQUIRE (retired.load () == total); // non-vacuous: the run really happened
    REQUIRE (queue.getNumPending () == 0);
    // Every refusal was retried, so nothing was actually lost: the dropped counter
    // accounts for exactly the retries and for nothing else (see the note above).
    REQUIRE (queue.getDroppedCount () == backPressureRetries.load ());
    REQUIRE (DeleteProbe::destroyed.load () == total);

    // THE assertion INSTRUCTIONS' criterion needs and TSan cannot make.
    REQUIRE (DeleteProbe::wrongThreadDeletes.load () == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// D3. …and on the real type: the audio thread frees nothing at all
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/pattern-channel: the adopting thread performs no heap traffic", "[perf-budget]")
{
    // D2 proves the retirement TEMPLATE deletes on the reclaiming thread. This one
    // closes the loop on the real `PatternSnapshot` from the other side: across a
    // full publish/adopt/retire run, the adopting thread's `new` AND `delete`
    // counts are both zero. Between them the two cases leave no room for a free to
    // have happened on the audio side.
    //
    // [perf-budget] for the usual reason (support/AllocationSentinel.h): the
    // allocation counter replaces global operator new/delete and does not compose
    // with ASan's own replacement, so it stays out of the sanitizer `-L unit` runs.
    constexpr std::uint64_t total = 200;

    PatternDocument document;
    PatternChannel channel;

    std::atomic<bool> go { false };
    std::atomic<bool> producerDone { false };
    std::atomic<std::uint64_t> audioAllocations { 0 };
    std::atomic<std::uint64_t> audioDeallocations { 0 };
    std::atomic<int> adoptions { 0 };

    std::thread audio (
        [&]
        {
            while (! go.load (std::memory_order_acquire))
            {
            }

            const PatternSnapshot* held = nullptr;
            int localAdoptions = 0;
            std::uint64_t allocs = 0;
            std::uint64_t frees = 0;

            {
                // No Catch2 macro, no juce::String, nothing pre-allocatable left to
                // allocate: only the channel calls inside the armed region.
                AllocationSentinel sentinel;

                while (! producerDone.load (std::memory_order_acquire))
                    if (channel.adopt (held))
                        ++localAdoptions;

                if (channel.adopt (held))
                    ++localAdoptions;

                if (held != nullptr)
                    channel.retire (held);

                allocs = sentinel.allocations ();
                frees = sentinel.deallocations ();
            }

            adoptions.store (localAdoptions, std::memory_order_relaxed);
            audioAllocations.store (allocs, std::memory_order_relaxed);
            audioDeallocations.store (frees, std::memory_order_relaxed);
        });

    std::thread message (
        [&]
        {
            while (! go.load (std::memory_order_acquire))
            {
            }

            const PatternSetState state = document.state ();
            for (std::uint64_t i = 1; i <= total; ++i)
            {
                channel.publish (buildPatternSnapshot (state, i));
                channel.reclaim ();
            }

            producerDone.store (true, std::memory_order_release);
        });

    go.store (true, std::memory_order_release);
    message.join ();
    audio.join ();
    channel.reclaim ();

    INFO ("adoptions on the audio thread: " << adoptions.load ());

    REQUIRE (adoptions.load () > 0); // non-vacuous
    REQUIRE (audioAllocations.load () == 0);
    REQUIRE (audioDeallocations.load () == 0); // NEVER freed on the audio thread
    REQUIRE (channel.getDroppedRetirementCount () == 0);
}
