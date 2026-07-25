#include "Euclid.h"

// EngineGuiGuard.h is pulled in FIRST by Euclid.h (before any JUCE include), which
// is where the tripwire needs to sit; repeating it here would be a no-op. Same
// convention as graph/MasterProcessor.cpp.
#include "PatternTypes.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>

namespace arpbox::engine::euclid
{
namespace
{
    /** Non-negative remainder. C++ `%` keeps the dividend's sign, so `-1 % 8 == -1`;
        the rotation needs `7`. Precondition: `m > 0`. */
    constexpr int positiveMod (int value, int m) noexcept
    {
        const int r = value % m;
        return r < 0 ? r + m : r;
    }
} // namespace

void generate (int steps, int pulses, int rotate, std::uint8_t* out) noexcept
{
    if (out == nullptr || steps <= 0)
        return;

    // A pattern is at most 64 steps (§12.1). `EuclidParams::steps` is a uint8_t and
    // so can carry a larger value than the lane can hold; clamp rather than write
    // past the caller's buffer, which is sized from maxSteps everywhere in-tree.
    jassert (steps <= maxSteps);
    const int n = steps > maxSteps ? maxSteps : steps;

    const int k = juce::jlimit (0, n, pulses);
    const int shift = positiveMod (rotate, n);

    // Bresenham necklace, rotated at the point of writing (one pass, no second
    // buffer). `i * k` peaks at 64 * 64 = 4096 — nowhere near int overflow.
    for (int i = 0; i < n; ++i)
    {
        const std::uint8_t pulse = ((i * k) % n) < k ? std::uint8_t (1) : std::uint8_t (0);

        // Positive `rotate` moves pulses LATER: the necklace entry for step i is
        // deposited at step i + rotate. Writing to the destination (rather than
        // reading from a rotated source) is what makes that direction readable
        // here, which matters because Phase 6.4 freezes it.
        out[static_cast<std::size_t> ((i + shift) % n)] = pulse;
    }
}

void applyToGateLane (const EuclidParams& params, LaneState& lane) noexcept
{
    if (! params.enabled)
        return;

    const int steps = static_cast<int> (params.steps);

    if (steps <= 0)
        return;

    const int n = steps > maxSteps ? maxSteps : steps;

    // Stack buffer, not a heap one: this is message-thread code, but a fixed-size
    // automatic array keeps the function usable from anywhere and costs 64 bytes.
    std::array<std::uint8_t, static_cast<std::size_t> (maxSteps)> gate {};
    generate (n, static_cast<int> (params.pulses), static_cast<int> (params.rotate), gate.data ());

    for (int i = 0; i < n; ++i)
        lane.values[static_cast<std::size_t> (i)] = clampLaneValue (LaneId::gate, gate[static_cast<std::size_t> (i)]);

    // The necklace length IS the lane length — a euclid of 12 steps means the GATE
    // lane loops every 12 steps (this is how euclid participates in polymeter).
    // Steps beyond `n` keep their previous values; they are outside the active
    // length and therefore never read by `laneValueAt`.
    lane.length = static_cast<std::uint8_t> (n);
}
} // namespace arpbox::engine::euclid
