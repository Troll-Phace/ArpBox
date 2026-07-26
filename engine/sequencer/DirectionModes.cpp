#include "DirectionModes.h"

// EngineGuiGuard.h is pulled in FIRST by DirectionModes.h (before any JUCE
// include), which is where the tripwire needs to sit; repeating it here would be
// a no-op. Same convention as graph/MasterProcessor.cpp.
#include "../generative/Rng.h"
#include "PatternTypes.h"

#include <juce_core/juce_core.h>

#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// THE PHASE-12 NOTE THAT USED TO LIVE HERE HAS BEEN DISCHARGED (Phase 7.1).
//
// This file used to carry a local `splitmix64` plus two salt literals
// (`walkSalt = 0x5741`, `randomSalt = 0x9E37`) in the anonymous namespace above,
// with a note reading "when the seed engine publishes a shared `splitmix64`,
// DELETE this local copy and call that one". Phase 7.1 published it, so all
// three are gone and the two stochastic modes below now call
// `engine/generative/Rng.h` — the single owner of the primitive and of the
// append-only `RngDomain` registry the salts moved into.
//
// THE MIGRATION IS BIT-IDENTICAL BY CONSTRUCTION, NOT BY MEASUREMENT: the shared
// function body is the old one character-for-character, and the two salts were
// recorded in the registry AT THEIR EXISTING LITERALS. The XOR expression shapes
// at the three call sites below are likewise unchanged. `tests/rng_primitive.cpp`
// pins the exact outputs of those three expressions so a future edit to either
// side cannot quietly move a `walk` or `randomNoRepeat` table.
//
// It is worth knowing WHY that mattered enough to be careful: these two modes
// were deliberately given no golden files (tests/determinism_goldens.cpp:41-55),
// so a drift here would have been invisible to the golden suite.
// ─────────────────────────────────────────────────────────────────────────────

namespace arpbox::engine::direction
{
namespace
{
    /** `converge`'s order (§12.3, OUTSIDE → MIDDLE): 0, n-1, 1, n-2, 2, … For n = 4
        that is 0,3,1,2. Every other symmetric mode is expressed in terms of this one
        so the three of them cannot drift apart. */
    constexpr int convergeIndex (int n, int k) noexcept
    {
        return (k % 2 == 0) ? (k / 2) : (n - 1 - (k / 2));
    }

    /** Hard cap on rejection-sampling retries in `randomNoRepeat`. Unreachable for
        n >= 2 (see the fallback comment at the call site); it exists purely so a
        future change to the exclusion set cannot turn this into a message-thread
        hang. */
    constexpr int maxRejectionAttempts = 64;
} // namespace

int traversalPeriod (DirectionMode mode, int poolSize) noexcept
{
    if (poolSize <= 0)
        return 0;

    // A pool larger than the traversal buffers can describe would silently
    // truncate; clamp so every period stays within `maxTraversalPeriod`.
    jassert (poolSize <= maxPoolSize);
    const int n = poolSize > maxPoolSize ? maxPoolSize : poolSize;

    // A single-note pool has exactly one traversal, whatever the mode says.
    if (n == 1)
        return 1;

    switch (mode)
    {
    case DirectionMode::up:
    case DirectionMode::down:
    case DirectionMode::converge:
    case DirectionMode::diverge:
    case DirectionMode::asPlayed:
        return n;

    case DirectionMode::upDownInclusive:
    case DirectionMode::outsideIn:
        return 2 * n;

    case DirectionMode::upDownExclusive:
        return juce::jmax (1, 2 * n - 2);

    case DirectionMode::spiral:
        return 3 * n;

    case DirectionMode::walk:
    case DirectionMode::randomNoRepeat:
        return 4 * n;

    case DirectionMode::count:
    default:
        jassertfalse; // Unknown mode — behave as `up` rather than fall silent.
        return n;
    }
}

int buildTraversal (DirectionMode mode, int poolSize, std::uint64_t seed, std::uint8_t* out, int outCapacity) noexcept
{
    if (out == nullptr || poolSize <= 0)
        return 0;

    jassert (poolSize <= maxPoolSize);
    const int n = poolSize > maxPoolSize ? maxPoolSize : poolSize;

    const int period = traversalPeriod (mode, n);

    if (period <= 0 || period > outCapacity)
    {
        // Refusing to write is deliberate: a table truncated to fit would be a
        // different (and silently shorter) musical phrase, and the caller would
        // have no way to notice. See the `outCapacity` note in the header.
        jassert (period <= outCapacity);
        return 0;
    }

    if (n == 1)
    {
        out[0] = 0;
        return 1;
    }

    switch (mode)
    {
    case DirectionMode::up:
    // `asPlayed` IS `up` — the arrival ordering lives in the pool array the
    // caller reads, not in this table (see `usesAsPlayedView`).
    case DirectionMode::asPlayed:
    {
        for (int k = 0; k < period; ++k)
            out[k] = static_cast<std::uint8_t> (k);

        break;
    }

    case DirectionMode::down:
    {
        for (int k = 0; k < period; ++k)
            out[k] = static_cast<std::uint8_t> (n - 1 - k);

        break;
    }

    case DirectionMode::upDownInclusive:
    {
        // Endpoints REPEAT at the turnaround: 0,1,2,3,3,2,1,0 for n = 4.
        for (int k = 0; k < period; ++k)
            out[k] = static_cast<std::uint8_t> (k < n ? k : 2 * n - 1 - k);

        break;
    }

    case DirectionMode::upDownExclusive:
    {
        // Endpoints play ONCE: 0,1,2,3,2,1 for n = 4.
        for (int k = 0; k < period; ++k)
            out[k] = static_cast<std::uint8_t> (k < n ? k : 2 * n - 2 - k);

        break;
    }

    case DirectionMode::converge:
    {
        for (int k = 0; k < period; ++k)
            out[k] = static_cast<std::uint8_t> (convergeIndex (n, k));

        break;
    }

    case DirectionMode::diverge:
    {
        // The EXACT reverse of converge — not "converge with the pool
        // flipped", which differs for odd n.
        for (int k = 0; k < period; ++k)
            out[k] = static_cast<std::uint8_t> (convergeIndex (n, n - 1 - k));

        break;
    }

    case DirectionMode::outsideIn:
    {
        // The full round trip: converge (outside → middle) concatenated with
        // diverge (middle → outside). The middle note therefore sounds twice
        // at the turnaround, matching `upDownInclusive`'s endpoint behaviour.
        for (int k = 0; k < n; ++k)
        {
            out[k] = static_cast<std::uint8_t> (convergeIndex (n, k));
            out[n + k] = static_cast<std::uint8_t> (convergeIndex (n, n - 1 - k));
        }

        break;
    }

    case DirectionMode::spiral:
    {
        // Up-2 / back-1: groups of three, each starting one higher than the
        // last — 0,1,2, 1,2,3, 2,3,4, … all modulo n.
        for (int k = 0; k < period; ++k)
            out[k] = static_cast<std::uint8_t> (((k / 3) + (k % 3)) % n);

        break;
    }

    case DirectionMode::walk:
    {
        // Brownian ±1, REFLECTING at both ends. A walk that wrapped from n-1
        // to 0 would be a leap across the whole chord, which is not what
        // §12.3's "walk (±1 brownian)" describes. Never a zero-length step.
        int cur = n / 2;
        out[0] = static_cast<std::uint8_t> (cur);

        for (int k = 1; k < period; ++k)
        {
            const bool stepUp = (rng::splitmix64 (seed ^ rng::domainSalt (rng::RngDomain::directionWalk) ^
                                                  static_cast<std::uint64_t> (k)) &
                                 1ULL) != 0ULL;

            int next = stepUp ? cur + 1 : cur - 1;

            if (next < 0)
                next = 1; // reflect off the bottom
            else if (next > n - 1)
                next = n - 2; // reflect off the top

            cur = next;
            out[k] = static_cast<std::uint8_t> (cur);
        }

        break;
    }

    case DirectionMode::randomNoRepeat:
    {
        // Uniform draw excluding the previous entry. Modulo bias is nil for a
        // power-of-two n and below 2^-58 otherwise, which is far under any
        // audible or testable threshold.
        out[0] = static_cast<std::uint8_t> (
            rng::splitmix64 (seed ^ rng::domainSalt (rng::RngDomain::directionRandomNoRepeat)) %
            static_cast<std::uint64_t> (n));

        for (int k = 1; k < period; ++k)
        {
            const int prev = out[k - 1];
            const bool isLast = (k == period - 1);
            const int firstEntry = out[0];

            int chosen = -1;

            for (int attempt = 0; attempt < maxRejectionAttempts; ++attempt)
            {
                const std::uint64_t hash =
                    rng::splitmix64 (seed ^ rng::domainSalt (rng::RngDomain::directionRandomNoRepeat) ^
                                     static_cast<std::uint64_t> (k) ^ (static_cast<std::uint64_t> (attempt) << 32));
                const int candidate = static_cast<int> (hash % static_cast<std::uint64_t> (n));

                if (candidate == prev)
                    continue;

                // The LAST entry additionally may not equal the first, so the
                // no-repeat invariant survives the loop point.
                //
                // n == 2 does NOT deadlock here: with only two notes the
                // sequence is forced alternation, the period 4n is even, and
                // so out[period-2] == out[0] — the two exclusions coincide and
                // one candidate always remains.
                if (isLast && candidate == firstEntry)
                    continue;

                chosen = candidate;
                break;
            }

            if (chosen < 0)
            {
                // Unreachable for n >= 2 by the argument above; a deterministic
                // scan rather than a hang if that ever stops holding.
                jassertfalse;

                for (int c = 0; c < n && chosen < 0; ++c)
                    if (c != prev && ! (isLast && c == firstEntry))
                        chosen = c;

                if (chosen < 0)
                    chosen = (prev + 1) % n;
            }

            out[k] = static_cast<std::uint8_t> (chosen);
        }

        break;
    }

    case DirectionMode::count:
    default:
    {
        jassertfalse; // Unknown mode — degrade to `up` rather than emit garbage.

        for (int k = 0; k < period; ++k)
            out[k] = static_cast<std::uint8_t> (k % n);

        break;
    }
    }

    return period;
}
} // namespace arpbox::engine::direction
