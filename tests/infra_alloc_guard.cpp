// ─────────────────────────────────────────────────────────────────────────────
// infra_alloc_guard — zero-allocation steady-state guard (ARCHITECTURE §11
// "Audio-thread allocations in steady state = 0, asserted by an operator-new
// guard"; Phase 2.4 success criterion "Allocation guard asserts 0 allocations in
// steady-state processBlock").
//
// PRESET: run under the DEFAULT preset only. ASan replaces the global operator
// new with its own instrumented version and will not compose with the override in
// AllocationCounter.cpp, so the strict "== 0" assertion is only meaningful without
// a sanitizer. This case is tagged [perf-budget], which is excluded from the
// `-L unit` sanitizer runs — it will not execute under the tsan/asan presets'
// unit filter.
//
// Two things are proven here:
//   1. The sentinel actually counts (self-test: a deliberate allocation inside an
//      armed region registers; an empty armed region registers zero).
//   2. The real EngineGraph processBlock path — command drain + full signal chain
//      + snapshot publish — performs ZERO heap allocations in steady state, after
//      warmup blocks absorb JUCE's one-time lazy allocations.
// ─────────────────────────────────────────────────────────────────────────────

#include "support/AllocationSentinel.h"

#include "engine/graph/EngineCommand.h"
#include "engine/graph/EngineGraph.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <new>

using arpbox::engine::EngineCommand;
using arpbox::engine::EngineCommandType;
using arpbox::engine::EngineGraph;
using arpbox::test::AllocationSentinel;

TEST_CASE ("infra/alloc-sentinel: counts allocations only inside an armed region",
           "[perf-budget]")
{
    // Empty armed region → zero allocations.
    std::uint64_t emptyDelta = 0;
    {
        AllocationSentinel sentinel;
        emptyDelta = sentinel.allocations ();
    }
    REQUIRE (emptyDelta == 0);

    // A deliberate allocation inside the armed region must register. Use the raw
    // library calls (not a new-expression) so the compiler cannot elide them, and
    // read the delta before freeing / before any Catch2 macro (which allocates).
    void* leaked = nullptr;
    std::uint64_t oneDelta = 0;
    {
        AllocationSentinel sentinel;
        leaked = ::operator new (64);
        oneDelta = sentinel.allocations ();
    }
    ::operator delete (leaked);

    REQUIRE (oneDelta >= 1);

    // Over-aligned allocations must be counted too (C++17 align_val_t overloads),
    // otherwise an over-aligned allocation on a measured path would silently escape
    // the guard. Exercise the aligned new/delete pair explicitly.
    constexpr std::align_val_t overAlign { 64 };
    void* alignedLeaked = nullptr;
    std::uint64_t alignedDelta = 0;
    {
        AllocationSentinel sentinel;
        alignedLeaked = ::operator new (128, overAlign);
        alignedDelta = sentinel.allocations ();
    }
    ::operator delete (alignedLeaked, overAlign);

    REQUIRE (alignedDelta >= 1);
}

TEST_CASE ("engine/process-block: zero allocations in steady state", "[perf-budget]")
{
    EngineGraph graph;
    graph.prepareToPlay (48000.0, 128);

    // Pre-allocate everything the measured loop touches BEFORE arming.
    juce::AudioBuffer<float> buffer (2, 128);
    juce::MidiBuffer midi;

    EngineCommand enableTone {};
    enableTone.type = EngineCommandType::setTestToneEnabled;
    enableTone.value.i = 1;
    REQUIRE (graph.commands ().push (enableTone));

    // Warmup: JUCE allocates render-sequence buffers / scratch lazily on the first
    // blocks. Run the FULL signal path (tone enabled) so nothing allocates later.
    constexpr int warmupBlocks = 64;
    for (int i = 0; i < warmupBlocks; ++i)
    {
        buffer.clear ();
        graph.getProcessor ().processBlock (buffer, midi);
    }

    // Steady state: arm on THIS thread (the one running processBlock) and measure.
    // Each block also pushes a command (exercises the SPSC drain path) and the
    // master writes a snapshot — none of which may allocate.
    constexpr int measuredBlocks = 256;
    std::uint64_t allocations = 0;
    {
        AllocationSentinel sentinel;
        for (int i = 0; i < measuredBlocks; ++i)
        {
            EngineCommand freq {};
            freq.type = EngineCommandType::setTestToneFrequency;
            freq.value.f = (i % 2 == 0) ? 440.0f : 441.0f;
            graph.commands ().push (freq);

            graph.getProcessor ().processBlock (buffer, midi);
        }
        allocations = sentinel.allocations ();
    }

    // The load-bearing assertion: the steady-state audio path is allocation-free.
    INFO ("steady-state allocations across " << measuredBlocks << " processBlock calls");
    REQUIRE (allocations == 0);
}
