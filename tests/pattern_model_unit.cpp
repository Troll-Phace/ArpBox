// ─────────────────────────────────────────────────────────────────────────────
// pattern_model_unit — the MESSAGE-THREAD half of Phase 6.1: the snapshot
// builder, the publish/adopt/retire channel, the editable document, and the
// retirement queue's capacity bound (ARCHITECTURE §3.4 mechanism 3, §4
// "Message-thread edit flow", §5.1 L1, §8.1, §12.1).
//
// These are `[unit]` tests — values against hand-derived expectations, not
// equality to a frozen stream. They are what says the object the audio thread
// adopts was built correctly in the first place; the render-level suites can only
// see what survives the build.
//
// ── THE ONE CASE THAT IS NOT OBVIOUS: THE DISPLACED PUBLISH ─────────────────
// `PatternChannel::publish` uses `exchange`, not `store`, and the difference is a
// ~108 KB leak per unadopted publish — one per mouse move during a piano-roll
// drag. Nothing observable distinguishes the two implementations: both leave the
// pending slot holding the newest snapshot, both leave the retirement queue
// empty, and the leaked object is unreachable rather than corrupt. So the
// discriminating assertion has to be about the HEAP, and it is made with the
// allocation counter's free side (support/AllocationSentinel.h) — which is why
// that one case carries the `[perf-budget]` tag the rest of this file does not.
// ─────────────────────────────────────────────────────────────────────────────

#include "support/AllocationSentinel.h"

#include "engine/graph/RetirementQueue.h"
#include "engine/midi/NotePool.h"
#include "engine/sequencer/DirectionModes.h"
#include "engine/sequencer/PatternChannel.h"
#include "engine/sequencer/PatternDocument.h"
#include "engine/sequencer/PatternSnapshot.h"
#include "engine/sequencer/PatternTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

using arpbox::engine::buildPatternSnapshot;
using arpbox::engine::clampLaneValue;
using arpbox::engine::DirectionMode;
using arpbox::engine::LaneId;
using arpbox::engine::laneOf;
using arpbox::engine::LaneRange;
using arpbox::engine::laneRange;
using arpbox::engine::LaneState;
using arpbox::engine::maxLaneDivision;
using arpbox::engine::maxPatterns;
using arpbox::engine::maxPoolSize;
using arpbox::engine::maxSteps;
using arpbox::engine::numLanes;
using arpbox::engine::numPoolSizes;
using arpbox::engine::PatternChannel;
using arpbox::engine::PatternDocument;
using arpbox::engine::PatternSetState;
using arpbox::engine::PatternSnapshot;
using arpbox::engine::PatternState;
using arpbox::engine::PoolSnapshot;
using arpbox::engine::RetirementQueue;
using arpbox::test::AllocationSentinel;

namespace
{
/** A state whose every lane holds one extreme value at each end, for the clamp
    test. Built by hand rather than through `PatternDocument`, which clamps on the
    way in — the point here is that the BUILDER is the last line of defence for a
    datum that reached `PatternSetState` some other way (a corrupt project file,
    Phase 12's operator stack, a future deserializer). */
PatternSetState outOfRangeState ()
{
    PatternSetState state {};

    for (auto& pattern : state.patterns)
    {
        for (int lane = 0; lane < numLanes; ++lane)
        {
            LaneState& target = pattern.lanes[static_cast<std::size_t> (lane)];
            target.values[0] = 32767;
            target.values[1] = -32768;
            target.values[maxSteps - 1] = 32767; // the INACTIVE tail is clamped too
        }
    }

    return state;
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// C1. The snapshot builder
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/snapshot-build: the snapshot is a trivially copyable, trivially destructible POD", "[unit]")
{
    // Not decoration: `RetirementQueue<const PatternSnapshot>` deletes through the
    // EXACT type with no virtual destructor, the determinism suite compares
    // snapshots byte-for-byte, and `PatternChannel` moves them by raw pointer. All
    // three are only sound while these hold.
    STATIC_REQUIRE (std::is_trivially_copyable_v<PatternSnapshot>);
    STATIC_REQUIRE (std::is_trivially_destructible_v<PatternSnapshot>);
    STATIC_REQUIRE (! std::is_polymorphic_v<PatternSnapshot>);
    STATIC_REQUIRE (std::is_trivially_copyable_v<arpbox::engine::PatternData>);
    STATIC_REQUIRE (std::is_trivially_copyable_v<arpbox::engine::TraversalSet>);
    STATIC_REQUIRE (std::is_trivially_copyable_v<LaneState>);
}

TEST_CASE ("sequencer/snapshot-build: every lane value is clamped into its §12.1 range", "[unit]")
{
    const auto snapshot = buildPatternSnapshot (outOfRangeState (), 1);
    REQUIRE (snapshot != nullptr);

    for (int lane = 0; lane < numLanes; ++lane)
    {
        const auto laneId = static_cast<LaneId> (lane);
        const LaneRange range = laneRange (laneId);
        const LaneState& built = snapshot->patterns[0].lanes[static_cast<std::size_t> (lane)];

        INFO ("lane ordinal " << lane << " range [" << range.lo << ", " << range.hi << "]");

        REQUIRE (built.values[0] == range.hi);
        REQUIRE (built.values[1] == range.lo);
        // The inactive tail rides along in the snapshot; if it were left unclamped
        // a later `setLaneLength` would expose an out-of-range value to the RT path.
        REQUIRE (built.values[maxSteps - 1] == range.hi);

        // And no slot anywhere escapes the range.
        int outOfRange = 0;
        for (int step = 0; step < maxSteps; ++step)
            if (built.values[static_cast<std::size_t> (step)] < range.lo ||
                built.values[static_cast<std::size_t> (step)] > range.hi)
                ++outOfRange;
        REQUIRE (outOfRange == 0);
    }

    // Spot-check the two widest and the two signed ranges by literal, so a change
    // to `laneRange` cannot make this case agree with itself.
    REQUIRE (snapshot->patterns[0].lanes[static_cast<std::size_t> (LaneId::gate)].values[0] == 1);
    REQUIRE (snapshot->patterns[0].lanes[static_cast<std::size_t> (LaneId::pitch)].values[1] == -24);
    REQUIRE (snapshot->patterns[0].lanes[static_cast<std::size_t> (LaneId::oct)].values[0] == 4);
    REQUIRE (snapshot->patterns[0].lanes[static_cast<std::size_t> (LaneId::vel)].values[1] == 1);
    REQUIRE (snapshot->patterns[0].lanes[static_cast<std::size_t> (LaneId::len)].values[0] == 400);
    REQUIRE (snapshot->patterns[0].lanes[static_cast<std::size_t> (LaneId::micro)].values[1] == -50);
    REQUIRE (snapshot->patterns[0].lanes[static_cast<std::size_t> (LaneId::modB)].values[0] == 127);
}

TEST_CASE ("sequencer/snapshot-build: lane length and division are clamped before the RT path", "[unit]")
{
    PatternSetState state {};

    LaneState& zeroed = laneOf (state.patterns[0], LaneId::gate);
    zeroed.length = 0;   // a 0 length makes the polymeter modulus undefined
    zeroed.division = 0; // …and a 0 division a division by zero

    LaneState& oversized = laneOf (state.patterns[1], LaneId::gate);
    oversized.length = 255; // above maxSteps ⇒ would index past LaneState::values
    oversized.division = 99;

    const auto snapshot = buildPatternSnapshot (state, 1);

    const auto& fixedZero = snapshot->patterns[0].lanes[static_cast<std::size_t> (LaneId::gate)];
    REQUIRE (fixedZero.length == 1);
    REQUIRE (fixedZero.division == 1);

    const auto& fixedOversize = snapshot->patterns[1].lanes[static_cast<std::size_t> (LaneId::gate)];
    REQUIRE (fixedOversize.length == maxSteps);
    REQUIRE (fixedOversize.division == maxLaneDivision);

    // The derived gate period follows the clamped values, never the raw ones.
    REQUIRE (snapshot->patterns[0].gatePeriodSteps == 1);
    REQUIRE (snapshot->patterns[1].gatePeriodSteps == maxSteps * maxLaneDivision);
}

TEST_CASE ("sequencer/snapshot-build: the gate-prefix table matches a hand-computed reference", "[unit]")
{
    // GATE length 4 at division 2, values {1,0,1,1}. The full cycle is
    // length x division = 8 base steps, and a base step p is gated iff
    // `p % 2 == 0 && values[p / 2] != 0`:
    //
    //   p        0  1  2  3  4  5  6  7
    //   on tick  Y  .  Y  .  Y  .  Y  .
    //   held     1  -  0  -  1  -  1  -
    //   gated    Y  .  .  .  Y  .  Y  .
    //
    // The table is an EXCLUSIVE prefix sum — gated steps STRICTLY BEFORE p — so the
    // first gated step of the cycle gets ordinal 0 and therefore traversal entry 0.
    constexpr std::uint16_t expectedPrefix[8] = { 0, 1, 1, 1, 1, 2, 2, 3 };
    constexpr int expectedPulses = 3;

    PatternSetState state {};
    LaneState& gate = laneOf (state.patterns[0], LaneId::gate);
    gate.length = 4;
    gate.division = 2;
    gate.values[0] = 1;
    gate.values[1] = 0;
    gate.values[2] = 1;
    gate.values[3] = 1;

    const auto snapshot = buildPatternSnapshot (state, 1);
    const auto& data = snapshot->pattern (0);

    REQUIRE (data.gatePeriodSteps == 8);
    REQUIRE (data.gatePulsesPerLoop == expectedPulses);

    for (int p = 0; p < 8; ++p)
    {
        INFO ("base step " << p);
        REQUIRE (data.gatePrefixPulses[static_cast<std::size_t> (p)] == expectedPrefix[p]);
    }

    // `isGated` and `gatedOrdinal` are summed from the SAME predicate, so they can
    // never drift — pinned here across two whole cycles, including the negative
    // step region the floor-mod exists for.
    for (int p = 0; p < 16; ++p)
    {
        const bool expectedGate = (p % 2 == 0) && (gate.values[static_cast<std::size_t> ((p / 2) % 4)] != 0);
        INFO ("global step " << p);
        REQUIRE (PatternSnapshot::isGated (data, p) == expectedGate);
        REQUIRE (snapshot->gatedOrdinal (data, p) == (p / 8) * expectedPulses + expectedPrefix[p % 8]);
    }

    // Negative steps floor-wrap rather than aliasing onto a positive slot.
    REQUIRE (snapshot->gatedOrdinal (data, -8) == -expectedPulses);
    REQUIRE (snapshot->gatedOrdinal (data, -1) == -expectedPulses + expectedPrefix[7]);
}

TEST_CASE ("sequencer/snapshot-build: traversal sets are deduplicated across the 16 patterns", "[unit]")
{
    // A traversal set is a pure function of (mode, seed) and costs ~4.3 KB, so the
    // builder interns them. Three distinct pairs across sixteen patterns must
    // produce exactly three sets — and, the part that actually matters, the
    // patterns sharing a pair must share an INDEX.
    PatternSetState state {};

    for (int i = 0; i < maxPatterns; ++i)
    {
        PatternState& pattern = state.patterns[static_cast<std::size_t> (i)];

        if (i < 5)
        {
            pattern.direction = DirectionMode::up;
            pattern.masterSeed = 0;
        }
        else if (i < 10)
        {
            pattern.direction = DirectionMode::down;
            pattern.masterSeed = 0;
        }
        else
        {
            // Same MODE as the first group but a different SEED — the pair, not the
            // mode alone, is the identity.
            pattern.direction = DirectionMode::up;
            pattern.masterSeed = 7;
        }
    }

    const auto snapshot = buildPatternSnapshot (state, 1);

    REQUIRE (snapshot->numTraversalSets == 3);

    const auto indexOf = [&snapshot] (int pattern) { return snapshot->pattern (pattern).traversalSetIndex; };

    for (int i = 1; i < 5; ++i)
        REQUIRE (indexOf (i) == indexOf (0));
    for (int i = 6; i < 10; ++i)
        REQUIRE (indexOf (i) == indexOf (5));
    for (int i = 11; i < maxPatterns; ++i)
        REQUIRE (indexOf (i) == indexOf (10));

    REQUIRE (indexOf (0) != indexOf (5));
    REQUIRE (indexOf (0) != indexOf (10));
    REQUIRE (indexOf (5) != indexOf (10));

    // Every set covers EVERY pool size 0..maxPoolSize, because in THRU mode the
    // pool size changes on the audio thread and a rebuild there is impossible.
    REQUIRE (numPoolSizes == maxPoolSize + 1);
    for (int setIndex = 0; setIndex < snapshot->numTraversalSets; ++setIndex)
    {
        const auto& set = snapshot->traversalSets[static_cast<std::size_t> (setIndex)];
        INFO ("traversal set " << setIndex);

        REQUIRE (set.period[0] == 0); // an empty pool emits nothing

        for (int size = 1; size <= maxPoolSize; ++size)
        {
            INFO ("pool size " << size);
            REQUIRE (set.period[static_cast<std::size_t> (size)] > 0);
            REQUIRE (set.period[static_cast<std::size_t> (size)] <= arpbox::engine::direction::maxTraversalPeriod);

            // Every entry is a legal pool index for THAT size.
            int outOfRange = 0;
            for (int k = 0; k < set.period[static_cast<std::size_t> (size)]; ++k)
                if (set.order[static_cast<std::size_t> (size)][static_cast<std::size_t> (k)] >= size)
                    ++outOfRange;
            REQUIRE (outOfRange == 0);
        }
    }

    // The two modes really do traverse differently — otherwise the dedup count
    // above would be satisfied by a builder that ignored the mode.
    REQUIRE (snapshot->traversalSets[0].mode == DirectionMode::up);
    REQUIRE (snapshot->traversalSets[1].mode == DirectionMode::down);
    REQUIRE (snapshot->traversalSets[2].seed == 7);
    REQUIRE (snapshot->traversalSets[0].order[4][0] == 0);
    REQUIRE (snapshot->traversalSets[1].order[4][0] == 3);

    // An empty pool must never yield a playable index.
    REQUIRE (snapshot->poolIndexAt (snapshot->pattern (0), 0, 0) == -1);
    REQUIRE (snapshot->poolIndexAt (snapshot->pattern (0), 0, -3) == -1);
}

TEST_CASE ("sequencer/snapshot-build: identical documents produce memcmp-identical snapshots", "[unit][determinism]")
{
    // The determinism contract's cheapest possible statement (§1.2): two documents
    // that are equal produce snapshots that are equal BYTE FOR BYTE, padding
    // included. `buildPatternSnapshot` is handed the same `buildCounter` on purpose
    // — that field is diagnostics and is deliberately the one thing allowed to
    // differ between two otherwise identical builds.
    PatternDocument first;
    PatternDocument second;

    const auto a = buildPatternSnapshot (first.state (), 7);
    const auto b = buildPatternSnapshot (second.state (), 7);

    REQUIRE (std::memcmp (a.get (), b.get (), sizeof (PatternSnapshot)) == 0);

    // …and a single lane edit is enough to make them differ, so the comparison is
    // not passing because everything is zero.
    REQUIRE (second.setLaneValue (3, LaneId::vel, 5, 42));
    const auto c = buildPatternSnapshot (second.state (), 7);
    REQUIRE (std::memcmp (a.get (), c.get (), sizeof (PatternSnapshot)) != 0);

    // The build counter is the ONLY difference a re-build of the same state makes.
    const auto d = buildPatternSnapshot (first.state (), 8);
    REQUIRE (d->buildCounter == 8);
    REQUIRE (a->buildCounter == 7);
    REQUIRE (std::memcmp (a.get (), d.get (), sizeof (PatternSnapshot)) != 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// C2. PatternChannel — publish / adopt / retire
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/pattern-channel: publish, adopt and retire round-trip", "[unit]")
{
    PatternDocument document;
    PatternChannel channel;

    REQUIRE (channel.peekPending () == nullptr);
    REQUIRE (channel.getNumPendingRetirements () == 0);

    // Nothing pending ⇒ `adopt` is a no-op and leaves the held pointer alone.
    const PatternSnapshot* held = nullptr;
    REQUIRE (channel.adopt (held) == false);
    REQUIRE (held == nullptr);

    auto first = buildPatternSnapshot (document.state (), 1);
    const PatternSnapshot* const firstRaw = first.get ();
    channel.publish (std::move (first));
    REQUIRE (channel.peekPending () == firstRaw);

    REQUIRE (channel.adopt (held));
    REQUIRE (held == firstRaw);
    REQUIRE (channel.peekPending () == nullptr);
    // The FIRST adoption retires nothing (there was no previous snapshot), and
    // `retire (nullptr)` must not consume a queue slot.
    REQUIRE (channel.getNumPendingRetirements () == 0);

    auto second = buildPatternSnapshot (document.state (), 2);
    const PatternSnapshot* const secondRaw = second.get ();
    channel.publish (std::move (second));

    REQUIRE (channel.adopt (held));
    REQUIRE (held == secondRaw);
    REQUIRE (channel.getNumPendingRetirements () == 1); // the first, handed back

    channel.reclaim ();
    REQUIRE (channel.getNumPendingRetirements () == 0);

    // The teardown path: the holder drops its snapshot with no replacement.
    REQUIRE (channel.retire (held));
    held = nullptr;
    REQUIRE (channel.getNumPendingRetirements () == 1);
    channel.reclaim ();

    REQUIRE (channel.getNumPendingRetirements () == 0);
    REQUIRE (channel.getDroppedRetirementCount () == 0);
}

TEST_CASE ("sequencer/pattern-channel: a displaced publish leaves the newest snapshot pending", "[unit]")
{
    // The structural half of the displaced-publish contract (the heap half is the
    // `[perf-budget]` case below): publishing twice without an adoption in between
    // leaves the NEWEST snapshot in the slot, routes nothing through the retirement
    // queue, and drops nothing. At most one snapshot is ever pending, whatever the
    // edit rate — which is what bounds memory during a piano-roll drag.
    PatternDocument document;
    PatternChannel channel;

    auto first = buildPatternSnapshot (document.state (), 1);
    auto second = buildPatternSnapshot (document.state (), 2);
    const PatternSnapshot* const secondRaw = second.get ();

    channel.publish (std::move (first));
    channel.publish (std::move (second));

    REQUIRE (channel.peekPending () == secondRaw);
    REQUIRE (channel.getNumPendingRetirements () == 0);
    REQUIRE (channel.getDroppedRetirementCount () == 0);

    const PatternSnapshot* held = nullptr;
    REQUIRE (channel.adopt (held));
    REQUIRE (held == secondRaw);
    REQUIRE (held->buildCounter == 2);

    REQUIRE (channel.retire (held));
    channel.reclaim ();
}

TEST_CASE ("sequencer/pattern-channel: a displaced publish frees the old snapshot on the message thread",
           "[perf-budget]")
{
    // THE GUARD ON `exchange` VS `store`, and it has to be a heap assertion because
    // nothing else can tell the two apart (see the header note).
    //
    // With `exchange`: this call takes the unadopted snapshot back and deletes it
    // HERE, on the message thread — exactly one `operator delete` inside the armed
    // region. With `store`: the pointer becomes unreachable, no free happens, and
    // ~108 KB leaks per unadopted publish.
    //
    // Tagged [perf-budget] for the same reason infra_alloc_guard.cpp is: the
    // allocation counter replaces global operator new/delete and does not compose
    // with ASan's own replacement, so it must stay out of the sanitizer `-L unit`
    // runs. AllocationSentinel discipline applies — everything is built before
    // arming, the delta is read inside the region into a plain integer, and no
    // Catch2 macro appears while armed.
    PatternDocument document;
    PatternChannel channel;

    auto first = buildPatternSnapshot (document.state (), 1);
    auto second = buildPatternSnapshot (document.state (), 2);
    const PatternSnapshot* const secondRaw = second.get ();

    channel.publish (std::move (first));

    std::uint64_t freesOnDisplace = 0;
    std::uint64_t allocsOnDisplace = 0;
    {
        AllocationSentinel sentinel;
        channel.publish (std::move (second));
        freesOnDisplace = sentinel.deallocations ();
        allocsOnDisplace = sentinel.allocations ();
    }

    INFO ("frees observed while displacing an unadopted snapshot");
    REQUIRE (freesOnDisplace == 1); // exactly the displaced snapshot
    REQUIRE (allocsOnDisplace == 0);
    REQUIRE (channel.peekPending () == secondRaw);
    REQUIRE (channel.getNumPendingRetirements () == 0);
    REQUIRE (channel.getDroppedRetirementCount () == 0);

    const PatternSnapshot* held = nullptr;
    REQUIRE (channel.adopt (held));
    REQUIRE (channel.retire (held));
    channel.reclaim ();
}

TEST_CASE ("infra/retirement-queue: the usable bound is Capacity-1 and one push past it drops", "[unit]")
{
    // THE BOUND, DOCUMENTED: `RetirementQueue<T, N>` is backed by a
    // `juce::AbstractFifo` of size N, whose free space is `N - 1 - numReady`. So N-1
    // items fit and the Nth is refused — NOT N. That matters because a refused
    // retirement is a LEAKED object, by design (the queue must never free on the
    // audio thread), so the real-world sizing question is "can the producer ever
    // get N-1 ahead of the consumer", and PatternChannel's answer is no: it retires
    // at most one per adoption and reclaims on every publish.
    struct Payload
    {
        int value = 0;
    };

    constexpr std::size_t capacity = 8;
    constexpr int usable = static_cast<int> (capacity) - 1;

    RetirementQueue<Payload, capacity> queue;

    int accepted = 0;
    Payload* refused = nullptr;

    for (std::size_t i = 0; i < capacity; ++i)
    {
        auto* object = new Payload { static_cast<int> (i) };
        if (queue.retire (object))
            ++accepted;
        else
            refused = object; // NOT freed by retire() — that is the whole design
    }

    REQUIRE (accepted == usable);
    REQUIRE (queue.getNumPending () == usable);
    REQUIRE (queue.getDroppedCount () == 1);
    REQUIRE (refused != nullptr);

    // Clean up the deliberately-dropped object here, on this thread, so the test
    // itself leaks nothing.
    delete refused;

    queue.reclaim ();
    REQUIRE (queue.getNumPending () == 0);
    REQUIRE (queue.getDroppedCount () == 1); // monotonic

    // Draining restores the full usable capacity.
    for (int i = 0; i < usable; ++i)
        REQUIRE (queue.retire (new Payload { i }));
    queue.reclaim ();
    REQUIRE (queue.getDroppedCount () == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// C3. PatternDocument — edits, transactions, undo, publishing
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/pattern-document: the default document reproduces the documented defaults", "[unit]")
{
    // The Phase-5 scaffold, now the DEFAULT CONFIGURATION — and the reason ~73 KB of
    // Phase-5 timing tests migrated unchanged. Pinned here so the equivalence is
    // stated once in a test rather than only in a comment.
    PatternDocument document;
    const auto& state = document.state ();

    REQUIRE (state.gridStepPpq == 0.25);
    REQUIRE (state.startPatternIndex == 0);
    REQUIRE (state.outputChannel == 1);

    REQUIRE (state.pool.size == 8);
    constexpr std::uint8_t expectedPool[8] = { 60, 62, 64, 65, 67, 69, 71, 72 };
    for (int i = 0; i < 8; ++i)
    {
        REQUIRE (state.pool.sorted[static_cast<std::size_t> (i)] == expectedPool[i]);
        // A stub pool has no arrival history, so the as-played view degenerates.
        REQUIRE (state.pool.asPlayed[static_cast<std::size_t> (i)] == expectedPool[i]);
    }

    for (int p = 0; p < maxPatterns; ++p)
    {
        INFO ("pattern " << p);
        const PatternState& pattern = state.patterns[static_cast<std::size_t> (p)];
        REQUIRE (pattern.direction == DirectionMode::up);
        REQUIRE (pattern.masterSeed == 0);
        REQUIRE (pattern.euclid.enabled == false);

        for (int lane = 0; lane < numLanes; ++lane)
        {
            const auto laneId = static_cast<LaneId> (lane);
            const LaneState& target = pattern.lanes[static_cast<std::size_t> (lane)];
            REQUIRE (target.length == 16);
            REQUIRE (target.division == 1);

            // Lanes are filled with `laneDefault`, NEVER value-initialised: a
            // zero-filled lane set is storage-valid but musically invalid (VEL 0,
            // LEN 0 and RATCHET 0 are all out of range).
            const std::int16_t expected =
                (p == 0 && laneId == LaneId::gate) ? std::int16_t { 1 } : arpbox::engine::laneDefault (laneId);
            REQUIRE (target.values[0] == expected);
        }
    }

    // Pattern 0 is the audible one; 1..15 keep GATE off and are silent.
    REQUIRE (laneOf (state.patterns[0], LaneId::gate).values[15] == 1);
    REQUIRE (laneOf (state.patterns[1], LaneId::gate).values[0] == 0);
}

TEST_CASE ("sequencer/pattern-document: every edit type validates, clamps and reports", "[unit]")
{
    PatternDocument document;

    SECTION ("lane values clamp rather than reject, and out-of-range coordinates reject")
    {
        REQUIRE (document.setLaneValue (0, LaneId::vel, 0, 500));
        REQUIRE (laneOf (document.state ().patterns[0], LaneId::vel).values[0] == 127);

        REQUIRE (document.setLaneValue (0, LaneId::pitch, 1, -999));
        REQUIRE (laneOf (document.state ().patterns[0], LaneId::pitch).values[1] == -24);

        // Clamping means a second call with a different overshoot is a NO-OP: the
        // stored value already equals the clamp.
        REQUIRE (document.setLaneValue (0, LaneId::vel, 0, 900) == false);

        REQUIRE (document.setLaneValue (-1, LaneId::vel, 0, 64) == false);
        REQUIRE (document.setLaneValue (maxPatterns, LaneId::vel, 0, 64) == false);
        REQUIRE (document.setLaneValue (0, LaneId::count, 0, 64) == false);
        REQUIRE (document.setLaneValue (0, LaneId::vel, -1, 64) == false);
        REQUIRE (document.setLaneValue (0, LaneId::vel, maxSteps, 64) == false);
    }

    SECTION ("lengths and divisions are range-checked")
    {
        REQUIRE (document.setLaneLength (0, LaneId::gate, 5));
        REQUIRE (laneOf (document.state ().patterns[0], LaneId::gate).length == 5);
        REQUIRE (document.setLaneLength (0, LaneId::gate, 5) == false); // no-op
        REQUIRE (document.setLaneLength (0, LaneId::gate, 0) == false);
        REQUIRE (document.setLaneLength (0, LaneId::gate, maxSteps + 1) == false);
        REQUIRE (laneOf (document.state ().patterns[0], LaneId::gate).length == 5);

        REQUIRE (document.setLaneDivision (0, LaneId::vel, maxLaneDivision));
        REQUIRE (document.setLaneDivision (0, LaneId::vel, 0) == false);
        REQUIRE (document.setLaneDivision (0, LaneId::vel, maxLaneDivision + 1) == false);
        REQUIRE (laneOf (document.state ().patterns[0], LaneId::vel).division == maxLaneDivision);
    }

    SECTION ("direction, seed, grid, channel, start pattern and pool")
    {
        REQUIRE (document.setDirection (2, DirectionMode::spiral));
        REQUIRE (document.state ().patterns[2].direction == DirectionMode::spiral);
        REQUIRE (document.setDirection (2, DirectionMode::spiral) == false);
        REQUIRE (document.setDirection (2, DirectionMode::count) == false);

        REQUIRE (document.setMasterSeed (2, 0xDEADBEEFCAFEF00DULL));
        REQUIRE (document.state ().patterns[2].masterSeed == 0xDEADBEEFCAFEF00DULL);
        REQUIRE (document.setMasterSeed (2, 0xDEADBEEFCAFEF00DULL) == false);

        REQUIRE (document.setGrid (0.5));
        REQUIRE (document.state ().gridStepPpq == 0.5);
        REQUIRE (document.setGrid (0.5) == false);
        REQUIRE (document.setGrid (0.0) == false);
        REQUIRE (document.setGrid (-1.0) == false);
        REQUIRE (document.state ().gridStepPpq == 0.5);

        REQUIRE (document.setOutputChannel (10));
        REQUIRE (document.setOutputChannel (0) == false);
        REQUIRE (document.setOutputChannel (17) == false);
        REQUIRE (document.state ().outputChannel == 10);

        REQUIRE (document.setStartPatternIndex (7));
        REQUIRE (document.setStartPatternIndex (maxPatterns) == false);
        REQUIRE (document.state ().startPatternIndex == 7);

        // An over-large pool is clamped, and the live prefix is what "changed" means
        // (entries past `size` are unspecified — NotePool.h).
        PoolSnapshot huge {};
        huge.size = 200;
        for (int i = 0; i < maxPoolSize; ++i)
            huge.sorted[static_cast<std::size_t> (i)] = static_cast<std::uint8_t> (40 + i);
        REQUIRE (document.setPool (huge));
        REQUIRE (document.state ().pool.size == maxPoolSize);
        REQUIRE (document.setPool (huge) == false); // clamped result is unchanged
    }

    SECTION ("applyEuclid normalises rotate into [0, steps) and writes the GATE lane")
    {
        // `EuclidParams::rotate` is an int8 and the generator takes any sign modulo
        // the necklace, so the document reduces before narrowing. -1 on an 8-step
        // necklace is 7.
        REQUIRE (document.applyEuclid (0, 8, 3, -1));
        REQUIRE (document.state ().patterns[0].euclid.rotate == 7);
        REQUIRE (document.state ().patterns[0].euclid.steps == 8);
        REQUIRE (document.state ().patterns[0].euclid.pulses == 3);
        REQUIRE (document.state ().patterns[0].euclid.enabled);
        REQUIRE (laneOf (document.state ().patterns[0], LaneId::gate).length == 8);

        // Rotating by -1 and by +7 are the same request, so they produce the same
        // stored rotation AND the same lane.
        PatternDocument sibling;
        REQUIRE (sibling.applyEuclid (0, 8, 3, 7));
        REQUIRE (sibling.state ().patterns[0].euclid.rotate == 7);
        for (int s = 0; s < 8; ++s)
            REQUIRE (laneOf (document.state ().patterns[0], LaneId::gate).values[static_cast<std::size_t> (s)] ==
                     laneOf (sibling.state ().patterns[0], LaneId::gate).values[static_cast<std::size_t> (s)]);

        // Clamping: pulses above steps means every step on, and a huge rotate
        // reduces rather than overflowing the int8.
        REQUIRE (document.applyEuclid (1, 8, 99, 1000));
        REQUIRE (document.state ().patterns[1].euclid.pulses == 8);
        REQUIRE (document.state ().patterns[1].euclid.rotate == 1000 % 8);

        // Disabling leaves the generated lane exactly as it stands.
        const auto before = laneOf (document.state ().patterns[0], LaneId::gate).values;
        REQUIRE (document.setEuclidEnabled (0, false));
        REQUIRE (document.setEuclidEnabled (0, false) == false);
        REQUIRE (laneOf (document.state ().patterns[0], LaneId::gate).values == before);
    }
}

TEST_CASE ("sequencer/pattern-document: a no-op edit costs neither an undo slot nor a snapshot build", "[unit]")
{
    // ~24 KB of state copy plus a ~108 KB snapshot build per edit is the price of
    // full-state undo; paying it for an edit that changes nothing would make a
    // piano-roll drag over unchanged steps quadratically expensive and would fill
    // the undo stack with entries that undo to themselves.
    PatternDocument document;
    PatternChannel channel;
    document.setPublishTarget (&channel);

    REQUIRE (document.getUndoDepth () == 0);
    REQUIRE (document.getBuildCounter () == 0);
    REQUIRE (document.getRevision () == 0);

    // The value already stored: VEL's default is 100.
    REQUIRE (document.setLaneValue (0, LaneId::vel, 0, 100) == false);
    REQUIRE (document.getUndoDepth () == 0);
    REQUIRE (document.getBuildCounter () == 0);
    REQUIRE (document.getRevision () == 0);
    REQUIRE (channel.peekPending () == nullptr);

    // A real edit does all three.
    REQUIRE (document.setLaneValue (0, LaneId::vel, 0, 101));
    REQUIRE (document.getUndoDepth () == 1);
    REQUIRE (document.getBuildCounter () == 1);
    REQUIRE (document.getRevision () == 1);
    REQUIRE (channel.peekPending () != nullptr);

    // A transaction in which every edit is a no-op commits nothing.
    document.beginTransaction ();
    REQUIRE (document.setLaneValue (0, LaneId::vel, 0, 101) == false);
    REQUIRE (document.setGrid (0.25) == false);
    document.endTransaction ();
    REQUIRE (document.getUndoDepth () == 1);
    REQUIRE (document.getBuildCounter () == 1);

    document.setPublishTarget (nullptr);
}

TEST_CASE ("sequencer/pattern-document: only the outermost transaction commits", "[unit]")
{
    PatternDocument document;
    PatternChannel channel;
    document.setPublishTarget (&channel);

    document.beginTransaction ();
    REQUIRE (document.isTransactionOpen ());
    document.beginTransaction ();
    document.beginTransaction ();

    REQUIRE (document.setLaneValue (0, LaneId::vel, 0, 11));
    REQUIRE (document.setLaneValue (0, LaneId::vel, 1, 22));
    REQUIRE (document.setLaneValue (0, LaneId::vel, 2, 33));

    // Nothing has committed yet: no undo entry, no build, nothing published.
    REQUIRE (document.getUndoDepth () == 0);
    REQUIRE (document.getBuildCounter () == 0);

    document.endTransaction ();
    REQUIRE (document.isTransactionOpen ());
    REQUIRE (document.getUndoDepth () == 0);
    document.endTransaction ();
    REQUIRE (document.isTransactionOpen ());
    REQUIRE (document.getUndoDepth () == 0);

    document.endTransaction (); // the outermost
    REQUIRE (! document.isTransactionOpen ());
    REQUIRE (document.getUndoDepth () == 1); // ONE entry for three edits
    REQUIRE (document.getBuildCounter () == 1);
    REQUIRE (channel.peekPending () != nullptr);

    // …and one undo reverts the whole gesture.
    REQUIRE (document.undo ());
    REQUIRE (laneOf (document.state ().patterns[0], LaneId::vel).values[0] == 100);
    REQUIRE (laneOf (document.state ().patterns[0], LaneId::vel).values[1] == 100);
    REQUIRE (laneOf (document.state ().patterns[0], LaneId::vel).values[2] == 100);

    document.setPublishTarget (nullptr);
}

TEST_CASE ("sequencer/pattern-document: undo and redo walk a linear history", "[unit]")
{
    PatternDocument document;

    REQUIRE (! document.canUndo ());
    REQUIRE (! document.canRedo ());
    REQUIRE (document.undo () == false);
    REQUIRE (document.redo () == false);

    REQUIRE (document.setLaneValue (0, LaneId::vel, 0, 10));
    REQUIRE (document.setLaneValue (0, LaneId::vel, 0, 20));
    REQUIRE (document.setLaneValue (0, LaneId::vel, 0, 30));
    REQUIRE (document.getUndoDepth () == 3);

    REQUIRE (document.undo ());
    REQUIRE (laneOf (document.state ().patterns[0], LaneId::vel).values[0] == 20);
    REQUIRE (document.undo ());
    REQUIRE (laneOf (document.state ().patterns[0], LaneId::vel).values[0] == 10);
    REQUIRE (document.getRedoDepth () == 2);

    REQUIRE (document.redo ());
    REQUIRE (laneOf (document.state ().patterns[0], LaneId::vel).values[0] == 20);

    // A NEW edit invalidates the redo branch — standard linear history.
    REQUIRE (document.setLaneValue (0, LaneId::vel, 0, 99));
    REQUIRE (document.getRedoDepth () == 0);
    REQUIRE (! document.canRedo ());

    // Undo mid-gesture is refused: it would discard half of the open transaction.
    document.beginTransaction ();
    REQUIRE (document.undo () == false);
    REQUIRE (document.redo () == false);
    document.endTransaction ();

    document.clearUndoHistory ();
    REQUIRE (document.getUndoDepth () == 0);
    REQUIRE (document.getRedoDepth () == 0);
    REQUIRE (laneOf (document.state ().patterns[0], LaneId::vel).values[0] == 99); // state kept
}

TEST_CASE ("sequencer/pattern-document: the undo stack is capped at maxUndoDepth", "[unit]")
{
    // §5.2 promises "unlimited undo" for document edits; read that as pragmatically
    // capped at 256 GESTURES, because each entry is a full ~24 KB `PatternSetState`
    // and an uncapped stack would be an unbounded leak during a long session.
    PatternDocument document;

    constexpr int overshoot = 40;
    // 1..127 cycling: consecutive values always differ, so every call is a real
    // edit and none is silently swallowed as a no-op.
    for (int i = 0; i < PatternDocument::maxUndoDepth + overshoot; ++i)
        REQUIRE (document.setLaneValue (0, LaneId::modA, 0, (i % 127) + 1));

    REQUIRE (document.getUndoDepth () == PatternDocument::maxUndoDepth);

    // The cap drops the OLDEST entries, so undoing all the way back lands on the
    // state that was current `maxUndoDepth` edits ago — not on the pristine one.
    int undone = 0;
    while (document.undo ())
        ++undone;

    REQUIRE (undone == PatternDocument::maxUndoDepth);
    REQUIRE (document.getUndoDepth () == 0);
    REQUIRE (laneOf (document.state ().patterns[0], LaneId::modA).values[0] != 0);
}

TEST_CASE ("sequencer/pattern-document: an attached publish target republishes on every committed edit", "[unit]")
{
    PatternDocument document;
    PatternChannel channel;

    // Attaching does NOT publish; the caller primes the channel explicitly.
    document.setPublishTarget (&channel);
    REQUIRE (channel.peekPending () == nullptr);

    document.publishTo (channel);
    REQUIRE (channel.peekPending () != nullptr);
    REQUIRE (document.getBuildCounter () == 1);

    const PatternSnapshot* held = nullptr;
    REQUIRE (channel.adopt (held));
    REQUIRE (held->buildCounter == 1);

    for (int i = 0; i < 5; ++i)
    {
        REQUIRE (document.setLaneValue (0, LaneId::modB, i, 7 + i));
        REQUIRE (channel.peekPending () != nullptr);
        REQUIRE (channel.adopt (held));
        REQUIRE (held->buildCounter == static_cast<std::uint64_t> (2 + i));
    }

    REQUIRE (document.getBuildCounter () == 6);

    // Detaching stops the automatic republish; the document still edits.
    document.setPublishTarget (nullptr);
    channel.reclaim ();
    REQUIRE (document.setLaneValue (0, LaneId::modB, 9, 42));
    REQUIRE (channel.peekPending () == nullptr);
    REQUIRE (document.getBuildCounter () == 6);

    REQUIRE (channel.retire (held));
    channel.reclaim ();
    REQUIRE (channel.getDroppedRetirementCount () == 0);
}
