// ─────────────────────────────────────────────────────────────────────────────
// infra_fifo — SpscFifo + EngineCommandQueue + EngineEventQueue behaviour
// (ARCHITECTURE §3.4, channels 1–3; Phase 2.4 "FIFO round-trip under churn").
//
// Single-threaded, deterministic behaviour tests (no sleeps): round-trip, the
// AbstractFifo two-range wrap boundary, overflow/drop accounting, and empty-drain
// no-op. The concurrent TSan stress lives in infra_tsan_stress.cpp.
//
// NOTE on capacity: juce::AbstractFifo of nominal capacity N reports N-1 usable
// slots (it reserves one to disambiguate full from empty). These tests therefore
// assert against the OBSERVED fill (successful pushes) rather than a hardcoded N,
// so they hold regardless of that off-by-one.
// ─────────────────────────────────────────────────────────────────────────────

#include "engine/graph/EngineCommand.h"
#include "engine/graph/EngineEvent.h"
#include "engine/graph/SpscFifo.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using arpbox::engine::EngineCommand;
using arpbox::engine::EngineCommandQueue;
using arpbox::engine::EngineCommandType;
using arpbox::engine::EngineEvent;
using arpbox::engine::EngineEventQueue;
using arpbox::engine::EngineEventType;
using arpbox::engine::SpscFifo;

TEST_CASE ("infra/spsc-fifo: push then drain preserves order and values", "[unit]")
{
    SpscFifo<int, 64> fifo;

    constexpr int count = 40;
    for (int i = 0; i < count; ++i)
        REQUIRE (fifo.push (i * 7 + 1));

    REQUIRE (fifo.getNumReady () == count);

    // Drain with a NON-ALLOCATING consumer: write into a preallocated array via a
    // captured index (no std::function, no container growth on the drain path).
    std::array<int, count> out {};
    int n = 0;
    fifo.drain ([&out, &n] (const int& v) noexcept { out[static_cast<std::size_t> (n++)] = v; });

    REQUIRE (n == count);
    REQUIRE (fifo.getNumReady () == 0);

    for (int i = 0; i < count; ++i)
        REQUIRE (out[static_cast<std::size_t> (i)] == i * 7 + 1);
}

TEST_CASE ("infra/spsc-fifo: churn across the wrap boundary keeps order", "[unit]")
{
    // Small ring + far more traffic than capacity forces validStart/validEnd to
    // cross the physical end of the backing array repeatedly (AbstractFifo's
    // two-range read/write). Interleaving partial pushes and drains guarantees the
    // write and read cursors wrap at different points.
    SpscFifo<std::uint32_t, 8> fifo;

    std::uint32_t nextToPush = 0;   // monotonic value stream
    std::uint32_t nextExpected = 0; // what the consumer must see next
    std::uint64_t consumed = 0;

    for (int iter = 0; iter < 500; ++iter)
    {
        // Push a small burst (some may be refused when the ring is near-full).
        const int burst = 3 + (iter % 4);
        for (int b = 0; b < burst; ++b)
            if (fifo.push (nextToPush))
                ++nextToPush;

        // Drain a smaller amount than we pushed on average, so occupancy walks up
        // and the cursors wrap. Values must arrive strictly in push order.
        bool ok = true;
        fifo.drain (
            [&] (const std::uint32_t& v) noexcept
            {
                if (v != nextExpected)
                    ok = false;
                ++nextExpected;
                ++consumed;
            });
        REQUIRE (ok);
    }

    // Flush whatever remains and confirm the full ordered sequence was received.
    // Flag-then-assert: a REQUIRE inside a noexcept drain lambda would std::terminate
    // (not report) on mismatch, so capture the result and assert after draining.
    bool tailOrdered = true;
    fifo.drain (
        [&] (const std::uint32_t& v) noexcept
        {
            if (v != nextExpected)
                tailOrdered = false;
            ++nextExpected;
            ++consumed;
        });
    REQUIRE (tailOrdered);

    REQUIRE (consumed == nextToPush);
    REQUIRE (nextExpected == nextToPush);
    REQUIRE (fifo.getNumReady () == 0);
}

TEST_CASE ("infra/spsc-fifo: overflow drops and increments the drop count", "[unit]")
{
    SpscFifo<int, 8> fifo;

    // Fill until push refuses. usable = capacity - 1 for AbstractFifo, but we do
    // not assume that number — we count successes. NOTE: the loop's TERMINATING
    // push (the one that returns false) is itself a drop, so the drop count is
    // already >= 1 once the ring is full — we baseline against it rather than
    // assuming zero.
    int pushed = 0;
    while (fifo.push (pushed))
        ++pushed;

    REQUIRE (pushed > 0);
    REQUIRE (fifo.getNumReady () == pushed); // every accepted item is ready
    REQUIRE (fifo.getNumReady () <= 8);      // never exceeds nominal capacity

    const std::uint64_t baseDrops = fifo.getDroppedCount ();
    REQUIRE (baseDrops >= 1); // the failed push that ended the fill loop counted

    // Every further push is refused and counted, exactly.
    constexpr int overflowAttempts = 25;
    for (int i = 0; i < overflowAttempts; ++i)
        REQUIRE_FALSE (fifo.push (1000 + i));

    REQUIRE (fifo.getDroppedCount () == baseDrops + static_cast<std::uint64_t> (overflowAttempts));
    REQUIRE (fifo.getNumReady () == pushed); // occupancy unchanged by refused pushes

    // After draining, the ring is usable again and the drop count is monotonic
    // (drops are a lifetime total, not reset by draining).
    int drained = 0;
    fifo.drain ([&drained] (const int&) noexcept { ++drained; });
    REQUIRE (drained == pushed);
    REQUIRE (fifo.getDroppedCount () == baseDrops + static_cast<std::uint64_t> (overflowAttempts));
    REQUIRE (fifo.push (42));
}

TEST_CASE ("infra/spsc-fifo: draining an empty queue is a no-op", "[unit]")
{
    SpscFifo<int, 16> fifo;

    int calls = 0;
    fifo.drain ([&calls] (const int&) noexcept { ++calls; });
    REQUIRE (calls == 0);
    REQUIRE (fifo.getNumReady () == 0);
    REQUIRE (fifo.getDroppedCount () == 0);
}

TEST_CASE ("infra/command-queue: POD commands round-trip intact", "[unit]")
{
    EngineCommandQueue queue;

    EngineCommand a {};
    a.type = EngineCommandType::setMasterGainDb;
    a.targetId = 3;
    a.value.f = -6.5f;

    EngineCommand b {};
    b.type = EngineCommandType::setLimiterEnabled;
    b.targetId = 0;
    b.value.i = 1;

    EngineCommand c {};
    c.type = EngineCommandType::setTestToneFrequency;
    c.targetId = 0;
    c.value.f = 220.0f;

    REQUIRE (queue.push (a));
    REQUIRE (queue.push (b));
    REQUIRE (queue.push (c));
    REQUIRE (queue.getNumReady () == 3);

    std::array<EngineCommand, 3> out {};
    int n = 0;
    queue.drain ([&out, &n] (const EngineCommand& cmd) noexcept { out[static_cast<std::size_t> (n++)] = cmd; });

    REQUIRE (n == 3);
    REQUIRE (out[0].type == EngineCommandType::setMasterGainDb);
    REQUIRE (out[0].targetId == 3);
    REQUIRE (out[0].value.f == -6.5f);
    REQUIRE (out[1].type == EngineCommandType::setLimiterEnabled);
    REQUIRE (out[1].value.i == 1);
    REQUIRE (out[2].type == EngineCommandType::setTestToneFrequency);
    REQUIRE (out[2].value.f == 220.0f);
}

TEST_CASE ("infra/event-queue: discrete events round-trip intact", "[unit]")
{
    EngineEventQueue queue;

    EngineEvent sr {};
    sr.type = EngineEventType::sampleRateChanged;
    sr.a = 48000;
    sr.b = 0;

    EngineEvent latency {};
    latency.type = EngineEventType::latencyChanged;
    latency.a = 512;
    latency.b = 0;

    REQUIRE (queue.push (sr));
    REQUIRE (queue.push (latency));

    std::array<EngineEvent, 2> out {};
    int n = 0;
    queue.drain ([&out, &n] (const EngineEvent& e) noexcept { out[static_cast<std::size_t> (n++)] = e; });

    REQUIRE (n == 2);
    REQUIRE (out[0].type == EngineEventType::sampleRateChanged);
    REQUIRE (out[0].a == 48000u);
    REQUIRE (out[1].type == EngineEventType::latencyChanged);
    REQUIRE (out[1].a == 512u);
}
