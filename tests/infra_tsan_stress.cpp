// ─────────────────────────────────────────────────────────────────────────────
// infra_tsan_stress — concurrent cross-thread stress for the two lock-free
// primitives (Phase 2.4 "TSan job clean on FIFO/snapshot stress tests"; success
// criterion "TSan clean on FIFO/snapshot success criterion").
//
// These are the tests the `tsan` preset exercises to prove the SPSC FIFO and the
// snapshot triple buffer are data-race free. Under the default preset they simply
// assert correctness (ordering, reconciliation, no torn reads). Real std::thread
// producers/consumers; start/stop gating via std::atomic and thread joins — NO
// sleeps are used for synchronisation. Iteration counts are high enough to
// provoke races but bounded so the non-TSan run stays fast.
// ─────────────────────────────────────────────────────────────────────────────

#include "engine/graph/EngineSnapshot.h"
#include "engine/graph/EngineSnapshotBuffer.h"
#include "engine/graph/SpscFifo.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <thread>

using arpbox::engine::EngineSnapshot;
using arpbox::engine::EngineSnapshotBuffer;
using arpbox::engine::SpscFifo;

TEST_CASE ("infra/spsc-fifo: concurrent producer/consumer reconciles and stays ordered",
           "[unit]")
{
    // One producer pushes a long, deterministic, monotonically increasing stream;
    // one consumer drains concurrently. SPSC guarantees the consumed values are a
    // strictly increasing subsequence of the produced stream (dropped items leave
    // gaps but never reorder). At the end: consumed + dropped == produced.
    SpscFifo<std::uint32_t, 1024> fifo;

    constexpr std::uint32_t total = 200000;

    std::atomic<bool> go { false };
    std::atomic<bool> producerDone { false };
    std::atomic<std::uint64_t> consumedCount { 0 };
    std::atomic<bool> orderOk { true };

    std::thread consumer ([&] {
        while (! go.load (std::memory_order_acquire))
        {
        } // spin until start (no sleep)

        std::uint64_t localConsumed = 0;
        bool first = true;
        std::uint32_t prev = 0;
        bool ok = true;

        const auto drainOnce = [&] {
            fifo.drain ([&] (const std::uint32_t& v) noexcept {
                if (! first && v <= prev)
                    ok = false; // must be strictly increasing (in-order, no dupes)
                prev = v;
                first = false;
                ++localConsumed;
            });
        };

        // Drain until the producer is done AND the ring is empty.
        while (! producerDone.load (std::memory_order_acquire))
            drainOnce ();
        drainOnce (); // final sweep of whatever remained after producerDone

        consumedCount.store (localConsumed, std::memory_order_relaxed);
        orderOk.store (ok, std::memory_order_relaxed);
    });

    std::thread producer ([&] {
        while (! go.load (std::memory_order_acquire))
        {
        }

        for (std::uint32_t i = 0; i < total; ++i)
            fifo.push (i); // drops when full are counted by the fifo

        producerDone.store (true, std::memory_order_release);
    });

    go.store (true, std::memory_order_release);
    producer.join ();
    consumer.join ();

    REQUIRE (orderOk.load ());
    REQUIRE (consumedCount.load () + fifo.getDroppedCount () == total);
    REQUIRE (fifo.getNumReady () == 0);
}

// REGRESSION GATE (Phase 2.4): this test once caught a real slot-reuse data race in
// EngineSnapshotBuffer under the `tsan` preset. The buffer transfers slot ownership
// in BOTH directions through the `ready` exchange — the writer reclaims the slot the
// reader relinquished, and vice versa — so an earlier release-only `commit()` /
// acquire-only `read()` left the reuse handoff unsynchronised. The fix made both
// `ready.exchange` calls `std::memory_order_acq_rel` (EngineSnapshotBuffer.h), which
// TSan confirms clean. Keep this test as the standing guard: if either exchange is
// ever weakened back below acq_rel, this concurrent run reintroduces the race and
// the tsan job fails here.
TEST_CASE ("infra/snapshot-buffer: concurrent reader never observes a torn value",
           "[unit]")
{
    // Writer commits a monotonically increasing blockCounter and sets peakL to a
    // deterministic function of it (peakL == 2 * blockCounter). A reader spins,
    // reading continuously. Two invariants prove the absence of tearing:
    //   1. Observed blockCounter is non-decreasing and within [0, total].
    //   2. Within every observed snapshot, peakL == 2 * blockCounter — a torn read
    //      that mixed fields from two different commits would break this.
    EngineSnapshotBuffer buffer;

    constexpr std::uint64_t total = 300000;

    std::atomic<bool> go { false };
    std::atomic<bool> writerDone { false };
    std::atomic<bool> readsOk { true };
    std::atomic<std::uint64_t> maxSeen { 0 };

    std::thread reader ([&] {
        while (! go.load (std::memory_order_acquire))
        {
        }

        std::uint64_t last = 0;
        bool ok = true;

        const auto checkOnce = [&] {
            const EngineSnapshot& s = buffer.read ();
            const std::uint64_t bc = s.blockCounter;

            if (bc < last)
                ok = false; // decreased → torn/garbage adoption
            if (bc > total)
                ok = false; // impossible value → tearing
            if (s.peakL != static_cast<float> (bc) * 2.0f)
                ok = false; // cross-field inconsistency → torn read

            last = bc;
        };

        while (! writerDone.load (std::memory_order_acquire))
            checkOnce ();
        checkOnce (); // one more after the writer stops

        maxSeen.store (last, std::memory_order_relaxed);
        readsOk.store (ok, std::memory_order_relaxed);
    });

    std::thread writer ([&] {
        while (! go.load (std::memory_order_acquire))
        {
        }

        for (std::uint64_t i = 1; i <= total; ++i)
        {
            EngineSnapshot& w = buffer.beginWrite ();
            w.blockCounter = i;
            w.peakL = static_cast<float> (i) * 2.0f;
            buffer.commit ();
        }

        writerDone.store (true, std::memory_order_release);
    });

    go.store (true, std::memory_order_release);
    writer.join ();
    reader.join ();

    REQUIRE (readsOk.load ());

    // After both threads join, the latest committed value must be visible.
    const EngineSnapshot& finalSnap = buffer.read ();
    REQUIRE (finalSnap.blockCounter == total);
    REQUIRE (finalSnap.peakL == static_cast<float> (total) * 2.0f);
    REQUIRE (maxSeen.load () <= total);
}
