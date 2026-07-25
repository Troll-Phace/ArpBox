#include "PatternSnapshot.h"

// EngineGuiGuard.h is pulled in FIRST by PatternSnapshot.h (before any JUCE
// include), which is where the tripwire needs to sit; repeating it here would be
// a no-op. Same convention as sequencer/DirectionModes.cpp.
#include "DirectionModes.h"
#include "PatternTypes.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <memory>

namespace arpbox::engine
{
namespace
{
    /** Clamps a stored lane `length` into `[1, maxSteps]`. A 0 length would make
        the polymeter modulus undefined; an over-long one would index past
        `LaneState::values`. Both are impossible through `PatternDocument`, but the
        snapshot is the LAST place a bad datum can be stopped before it reaches the
        audio thread — after this point the RT path assumes validity. */
    constexpr int clampLaneLength (int length) noexcept
    {
        return length < 1 ? 1 : (length > maxSteps ? maxSteps : length);
    }

    /** Clamps a stored lane `division` into `[1, maxLaneDivision]`. */
    constexpr int clampLaneDivision (int division) noexcept
    {
        return division < 1 ? 1 : (division > maxLaneDivision ? maxLaneDivision : division);
    }

    /** Copies one lane into the snapshot with every value forced into its §12.1
        range. The clamp runs over ALL `maxSteps` slots, not just `[0, length)`:
        the inactive tail rides along in the snapshot and would otherwise be the
        one place an out-of-range value could survive a later `setLaneLength`. */
    void buildLane (const LaneState& source, LaneId laneId, LaneState& destination) noexcept
    {
        destination.length = static_cast<std::uint8_t> (clampLaneLength (source.length));
        destination.division = static_cast<std::uint8_t> (clampLaneDivision (source.division));

        for (int step = 0; step < maxSteps; ++step)
            destination.values[static_cast<std::size_t> (step)] =
                clampLaneValue (laneId, source.values[static_cast<std::size_t> (step)]);
    }

    /** Sums the (already clamped) GATE lane into `data`'s exclusive prefix table.

        Walks the lane's FULL cycle — `length * division` base steps — because that
        is the period over which the gated-step count repeats. Within one cycle the
        lane index is a plain `p / division` with no modulus needed, since
        `p < length * division` guarantees `p / division < length`. */
    void buildGatePrefix (PatternData& data) noexcept
    {
        const LaneState& gate = data.lanes[static_cast<std::size_t> (LaneId::gate)];
        const int division = gate.division > 0 ? static_cast<int> (gate.division) : 1;
        const int length = gate.length > 0 ? static_cast<int> (gate.length) : 1;
        const int periodSteps = length * division;

        jassert (periodSteps >= 1 && periodSteps <= maxGatePeriodSteps);

        data.gatePeriodSteps = periodSteps;

        int running = 0;

        for (int p = 0; p < periodSteps; ++p)
        {
            // EXCLUSIVE prefix: write the running total BEFORE counting step p, so
            // the first gated step of the cycle gets ordinal 0.
            data.gatePrefixPulses[static_cast<std::size_t> (p)] = static_cast<std::uint16_t> (running);

            const bool onTick = (p % division) == 0;
            const bool held = gate.values[static_cast<std::size_t> (p / division)] != 0;

            if (onTick && held)
                ++running;
        }

        data.gatePulsesPerLoop = running;
    }

    /** Fills `set`'s traversal tables for EVERY pool size 0..maxPoolSize (see the
        rationale on `TraversalSet`). Pool size 0 stays at period 0 — an empty pool
        emits nothing, and `poolIndexAt` returns -1 for it. */
    void buildTraversalSet (TraversalSet& set, DirectionMode mode, std::uint64_t seed) noexcept
    {
        set.mode = mode;
        set.seed = seed;

        for (int poolSize = 1; poolSize <= maxPoolSize; ++poolSize)
        {
            auto& order = set.order[static_cast<std::size_t> (poolSize)];
            const int written =
                direction::buildTraversal (mode, poolSize, seed, order.data (), direction::maxTraversalPeriod);

            // buildTraversal only returns 0 on inputs this loop cannot produce
            // (null buffer, non-positive pool, undersized capacity).
            jassert (written > 0);

            set.period[static_cast<std::size_t> (poolSize)] = static_cast<std::uint16_t> (written < 0 ? 0 : written);
        }
    }

    /** Finds the existing set matching (`mode`, `seed`), or builds a new one.

        Linear scan over at most 16 entries, once per pattern, on the message
        thread — 128 comparisons worst case, against the ~4,200 table entries each
        avoided build would cost. A map would be slower and would allocate. */
    std::uint16_t internTraversalSet (PatternSnapshot& snapshot, DirectionMode mode, std::uint64_t seed) noexcept
    {
        for (int i = 0; i < snapshot.numTraversalSets; ++i)
        {
            const TraversalSet& existing = snapshot.traversalSets[static_cast<std::size_t> (i)];

            if (existing.mode == mode && existing.seed == seed)
                return static_cast<std::uint16_t> (i);
        }

        // Cannot overflow: at most one new set per pattern, and there are exactly
        // `maxPatterns` slots.
        jassert (snapshot.numTraversalSets < maxPatterns);
        const int index = snapshot.numTraversalSets;

        buildTraversalSet (snapshot.traversalSets[static_cast<std::size_t> (index)], mode, seed);
        snapshot.numTraversalSets = index + 1;

        return static_cast<std::uint16_t> (index);
    }
} // namespace

// MESSAGE-THREAD ONLY:
std::unique_ptr<const PatternSnapshot> buildPatternSnapshot (const PatternSetState& state, std::uint64_t buildCounter)
{
    auto snapshot = std::make_unique<PatternSnapshot> ();

    snapshot->gridStepPpq = state.gridStepPpq > 0.0 ? state.gridStepPpq : 0.25;
    snapshot->startPatternIndex =
        static_cast<std::int32_t> (juce::jlimit (0, maxPatterns - 1, state.startPatternIndex));
    snapshot->outputChannel = static_cast<std::int32_t> (juce::jlimit (1, 16, state.outputChannel));
    snapshot->pool = state.pool;
    snapshot->buildCounter = buildCounter;

    if (snapshot->pool.size > static_cast<std::uint8_t> (maxPoolSize))
        snapshot->pool.size = static_cast<std::uint8_t> (maxPoolSize);

    for (int patternIndex = 0; patternIndex < maxPatterns; ++patternIndex)
    {
        const PatternState& source = state.patterns[static_cast<std::size_t> (patternIndex)];
        PatternData& data = snapshot->patterns[static_cast<std::size_t> (patternIndex)];

        for (int lane = 0; lane < numLanes; ++lane)
        {
            const auto laneId = static_cast<LaneId> (lane);
            buildLane (source.lanes[static_cast<std::size_t> (lane)],
                       laneId,
                       data.lanes[static_cast<std::size_t> (lane)]);
        }

        // An out-of-enum direction would index the traversal switch's default arm;
        // normalise it here so the snapshot is self-consistent with what was built.
        const DirectionMode mode = (source.direction < DirectionMode::count) ? source.direction : DirectionMode::up;

        data.direction = mode;
        data.asPlayedView = direction::usesAsPlayedView (mode);
        data.traversalSetIndex = internTraversalSet (*snapshot, mode, source.masterSeed);

        buildGatePrefix (data);
    }

    // Guarantees `traversalSetFor` always lands on a built set, even for a
    // hypothetical zero-pattern build.
    jassert (snapshot->numTraversalSets > 0);

    return snapshot;
}
} // namespace arpbox::engine
