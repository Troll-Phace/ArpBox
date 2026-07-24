// ─────────────────────────────────────────────────────────────────────────────
// infra_snapshot — EngineSnapshotBuffer (triple buffer) + RetirementQueue
// (ARCHITECTURE §3.4, channel 2 state-half + channel 3; Phase 2.4 "snapshot swap
// adoption at boundaries", "retired snapshots freed on message thread only").
//
// Single-threaded semantics tests: publish/adopt, freshness/stability, and
// blockCounter monotonicity. The concurrent no-torn-read stress is in
// infra_tsan_stress.cpp. RetirementQueue deletion-count semantics are verified
// with a destructor-counting payload.
// ─────────────────────────────────────────────────────────────────────────────

#include "engine/graph/EngineSnapshot.h"
#include "engine/graph/EngineSnapshotBuffer.h"
#include "engine/graph/RetirementQueue.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <vector>

using arpbox::engine::EngineSnapshot;
using arpbox::engine::EngineSnapshotBuffer;
using arpbox::engine::RetirementQueue;

TEST_CASE ("infra/snapshot-buffer: committed write is adopted by the reader", "[unit]")
{
    EngineSnapshotBuffer buffer;

    // A fresh buffer reads a valid, fully-zeroed snapshot (never garbage).
    const EngineSnapshot& initial = buffer.read ();
    REQUIRE (initial.blockCounter == 0);
    REQUIRE (initial.peakL == 0.0f);

    EngineSnapshot& w = buffer.beginWrite ();
    w.blockCounter = 42;
    w.peakL = 0.25f;
    w.peakR = 0.5f;
    w.voiceCount = 3;
    buffer.commit ();

    const EngineSnapshot& r = buffer.read ();
    REQUIRE (r.blockCounter == 42);
    REQUIRE (r.peakL == 0.25f);
    REQUIRE (r.peakR == 0.5f);
    REQUIRE (r.voiceCount == 3);
}

TEST_CASE ("infra/snapshot-buffer: read is stable when nothing new is committed", "[unit]")
{
    EngineSnapshotBuffer buffer;

    EngineSnapshot& w = buffer.beginWrite ();
    w.blockCounter = 7;
    w.rmsL = 0.1f;
    buffer.commit ();

    const EngineSnapshot& first = buffer.read ();
    REQUIRE (first.blockCounter == 7);

    // No intervening commit → repeated reads keep returning the same value, never
    // a torn or default-zero snapshot.
    for (int i = 0; i < 5; ++i)
    {
        const EngineSnapshot& again = buffer.read ();
        REQUIRE (again.blockCounter == 7);
        REQUIRE (again.rmsL == 0.1f);
    }
}

TEST_CASE ("infra/snapshot-buffer: reader adopts the newest of several commits", "[unit]")
{
    EngineSnapshotBuffer buffer;

    // Multiple commits with no read in between: the reader must jump to the LATEST
    // published value, not an intermediate one.
    for (std::uint64_t v = 1; v <= 4; ++v)
    {
        EngineSnapshot& w = buffer.beginWrite ();
        w.blockCounter = v;
        w.seed = static_cast<std::uint32_t> (v * 100);
        buffer.commit ();
    }

    const EngineSnapshot& r = buffer.read ();
    REQUIRE (r.blockCounter == 4);
    REQUIRE (r.seed == 400u);
}

TEST_CASE ("infra/snapshot-buffer: blockCounter is monotonic across many cycles", "[unit]")
{
    EngineSnapshotBuffer buffer;

    std::uint64_t lastSeen = 0;
    constexpr std::uint64_t cycles = 5000;

    for (std::uint64_t i = 1; i <= cycles; ++i)
    {
        EngineSnapshot& w = buffer.beginWrite ();
        w.blockCounter = i;
        w.peakL = static_cast<float> (i);
        buffer.commit ();

        // Interleave reads at an uneven cadence; each observed value must be
        // non-decreasing and consistent within the snapshot (peakL == counter).
        if (i % 3 == 0)
        {
            const EngineSnapshot& r = buffer.read ();
            REQUIRE (r.blockCounter >= lastSeen);
            REQUIRE (r.peakL == static_cast<float> (r.blockCounter));
            lastSeen = r.blockCounter;
        }
    }

    const EngineSnapshot& final = buffer.read ();
    REQUIRE (final.blockCounter == cycles);
    REQUIRE (final.blockCounter >= lastSeen);
}

namespace
{
// Destructor-counting payload for the retirement-queue tests. Counts are static
// atomics so the tests can assert exactly how many objects were freed and when.
struct Tracked
{
    Tracked () noexcept { ++live; }
    ~Tracked () noexcept
    {
        --live;
        ++destroyed;
    }

    int payload = 0;

    static std::atomic<int> live;      ///< Currently-alive instances.
    static std::atomic<int> destroyed; ///< Lifetime total destroyed.
};

std::atomic<int> Tracked::live { 0 };
std::atomic<int> Tracked::destroyed { 0 };
} // namespace

TEST_CASE ("infra/retirement-queue: reclaim frees exactly the retired objects", "[unit]")
{
    Tracked::live = 0;
    Tracked::destroyed = 0;

    RetirementQueue<Tracked> queue; // default capacity 256

    constexpr int k = 32;
    for (int i = 0; i < k; ++i)
    {
        auto* obj = new Tracked ();
        obj->payload = i;
        REQUIRE (queue.retire (obj)); // enqueues pointer only — does NOT delete
    }

    // retire() must not have freed anything: the "audio thread" never deletes.
    REQUIRE (queue.getNumPending () == k);
    REQUIRE (Tracked::destroyed.load () == 0);
    REQUIRE (Tracked::live.load () == k);

    // reclaim() on the "message thread" deletes every pending object, exactly K.
    queue.reclaim ();
    REQUIRE (Tracked::destroyed.load () == k);
    REQUIRE (Tracked::live.load () == 0);
    REQUIRE (queue.getNumPending () == 0);

    // reclaim() on an empty queue is a harmless no-op.
    queue.reclaim ();
    REQUIRE (Tracked::destroyed.load () == k);
}

TEST_CASE ("infra/retirement-queue: overflow drops without deleting on retire", "[unit]")
{
    Tracked::live = 0;
    Tracked::destroyed = 0;

    // Tiny capacity so retire() starts refusing quickly. usable = capacity - 1.
    RetirementQueue<Tracked, 4> queue;

    std::vector<Tracked*> droppedPtrs; // hold refused objects so nothing leaks
    int enqueued = 0;

    for (int i = 0; i < 16; ++i)
    {
        auto* obj = new Tracked ();
        if (queue.retire (obj))
            ++enqueued;
        else
            droppedPtrs.push_back (obj); // NOT freed by retire() — by design
    }

    REQUIRE (enqueued > 0);
    REQUIRE (! droppedPtrs.empty ());
    REQUIRE (queue.getDroppedCount () == static_cast<std::uint64_t> (droppedPtrs.size ()));
    REQUIRE (Tracked::destroyed.load () == 0); // retire() never deletes

    // reclaim() frees ONLY the objects that were actually enqueued.
    queue.reclaim ();
    REQUIRE (Tracked::destroyed.load () == enqueued);

    // Clean up the intentionally-dropped objects on this (message) thread so the
    // test itself leaks nothing — a dropped retirement is a leak by design.
    for (auto* p : droppedPtrs)
        delete p;

    REQUIRE (Tracked::live.load () == 0);
}
