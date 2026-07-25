// ─────────────────────────────────────────────────────────────────────────────
// pattern_directions — Phase 6.4: the pool-traversal order tables for the 11
// direction modes (ARCHITECTURE §12.3, engine/sequencer/DirectionModes.h).
//
// WHY [unit] AND NOT [determinism]. Same convention split as pattern_euclid.cpp:
// [determinism] is equality against a frozen byte stream in tests/golden/,
// [midi-conformance] is a MIDI-semantic invariant, [unit] is a value checked
// against a HAND-DERIVED expectation. Every literal here was written out from the
// "ORDERS" table in DirectionModes.h and can be re-derived with a pencil. Phase
// 6.4 then freezes these traversals into golden MIDI — so this file is what says
// the goldens were baked from the right ORDER, not merely from whatever the
// switch statement happened to emit.
//
// ── THE CENTRAL ANTI-VACUITY PROBLEM ────────────────────────────────────────
// Nine deterministic modes, nine `REQUIRE (order == {...})` lines — and a switch
// that fell through to `up` for eight of them would look exactly as green, because
// nothing in nine independent assertions says the nine ANSWERS DIFFER. So the
// pinned sequences are collected into one table and compared ALL-PAIRS. See the
// "up and asPlayed are equal BY DESIGN" note on that test: exactly one pair
// legitimately coincides, and the test pins that pair rather than exempting it.
//
// The stochastic modes (`walk`, `randomNoRepeat`) get PROPERTY tests only. They
// are built on the message thread from `splitmix64` — deliberately NOT Phase 7's
// audio-thread `RngStream`, see the rationale in DirectionModes.h — and Phase 6
// bakes no goldens for them.
// ─────────────────────────────────────────────────────────────────────────────

#include "engine/sequencer/DirectionModes.h"
#include "engine/sequencer/PatternTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

using arpbox::engine::DirectionMode;
using arpbox::engine::maxPoolSize;
using arpbox::engine::numDirectionModes;
using arpbox::engine::direction::buildTraversal;
using arpbox::engine::direction::maxTraversalPeriod;
using arpbox::engine::direction::traversalPeriod;
using arpbox::engine::direction::usesAsPlayedView;

namespace
{
/** A generously oversized traversal buffer pre-filled with a sentinel, so "wrote
    nothing" is distinguishable from "wrote zeros". */
constexpr std::uint8_t sentinel = 0xEE;
constexpr int bufferSize = maxTraversalPeriod + 16;

using Order = std::vector<int>;

/** `buildTraversal` into a vector of exactly `period` entries. Returns empty when
    the builder refuses (period 0). */
Order traversal (DirectionMode mode, int poolSize, std::uint64_t seed = 0)
{
    std::array<std::uint8_t, bufferSize> raw {};
    raw.fill (sentinel);

    const int period = buildTraversal (mode, poolSize, seed, raw.data (), bufferSize);

    Order out;
    out.reserve (static_cast<std::size_t> (period));

    for (int k = 0; k < period; ++k)
        out.push_back (static_cast<int> (raw[static_cast<std::size_t> (k)]));

    return out;
}

std::string describe (const Order& order)
{
    std::string out;

    for (std::size_t i = 0; i < order.size (); ++i)
    {
        if (i != 0)
            out += ',';

        out += std::to_string (order[i]);
    }

    return out;
}

const char* modeName (DirectionMode mode)
{
    switch (mode)
    {
    case DirectionMode::up:
        return "up";
    case DirectionMode::down:
        return "down";
    case DirectionMode::upDownInclusive:
        return "upDownInclusive";
    case DirectionMode::upDownExclusive:
        return "upDownExclusive";
    case DirectionMode::converge:
        return "converge";
    case DirectionMode::diverge:
        return "diverge";
    case DirectionMode::outsideIn:
        return "outsideIn";
    case DirectionMode::asPlayed:
        return "asPlayed";
    case DirectionMode::walk:
        return "walk";
    case DirectionMode::randomNoRepeat:
        return "randomNoRepeat";
    case DirectionMode::spiral:
        return "spiral";
    case DirectionMode::count:
    default:
        return "?";
    }
}

DirectionMode modeAt (int ordinal)
{
    return static_cast<DirectionMode> (ordinal);
}

/** The nine modes that are a pure function of (mode, poolSize) — everything
    except `walk` and `randomNoRepeat`. */
constexpr std::array<DirectionMode, 9> deterministicModes {
    DirectionMode::up,       DirectionMode::down,    DirectionMode::upDownInclusive, DirectionMode::upDownExclusive,
    DirectionMode::converge, DirectionMode::diverge, DirectionMode::outsideIn,       DirectionMode::asPlayed,
    DirectionMode::spiral
};

bool isStochastic (DirectionMode mode)
{
    return mode == DirectionMode::walk || mode == DirectionMode::randomNoRepeat;
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/directions: the nine deterministic modes produce their pinned orders", "[unit]")
{
    // n = 4 is the smallest pool that separates every one of these: it is even (so
    // converge/diverge have a real middle pair), larger than 3 (so spiral's up-2/
    // back-1 groups do not collapse onto `up`), and short enough to write out.
    constexpr int n = 4;

    // Each expectation below is derived from the ORDERS table in DirectionModes.h:
    //
    //   up               k                                  → 0,1,2,3
    //   down             n-1-k                              → 3,2,1,0
    //   upDownInclusive  k<n ? k : 2n-1-k   (period 2n)      → 0,1,2,3,3,2,1,0
    //   upDownExclusive  k<n ? k : 2n-2-k   (period 2n-2)    → 0,1,2,3,2,1
    //   converge         k even → k/2 ; k odd → n-1-k/2      → 0,3,1,2   OUTSIDE→MIDDLE
    //   diverge          converge[n-1-k]                     → 2,1,3,0   the exact reverse
    //   outsideIn        converge ++ diverge (period 2n)     → 0,3,1,2,2,1,3,0
    //   asPlayed         k — a POOL-VIEW selection            → 0,1,2,3
    //   spiral           ((k/3)+(k%3)) % n  (period 3n)      → 0,1,2, 1,2,3, 2,3,0, 3,0,1
    const std::vector<std::pair<DirectionMode, Order>> pinned {
        { DirectionMode::up, { 0, 1, 2, 3 } },
        { DirectionMode::down, { 3, 2, 1, 0 } },
        { DirectionMode::upDownInclusive, { 0, 1, 2, 3, 3, 2, 1, 0 } },
        { DirectionMode::upDownExclusive, { 0, 1, 2, 3, 2, 1 } },
        { DirectionMode::converge, { 0, 3, 1, 2 } },
        { DirectionMode::diverge, { 2, 1, 3, 0 } },
        // OUTSIDE-IN DOUBLES A NOTE AT BOTH ENDS, DELIBERATELY. It is literally
        // converge followed by diverge, so the MIDDLE note sounds twice at the
        // turnaround (…1,2,2,1…) and the OUTER note sounds twice across the loop
        // point (0 last, 0 first) — the same shape upDownInclusive already has.
        // A future "outside-in without the doubles" is an APPENDED enum entry, not
        // an edit to this one: changing it here regenerates goldens (§1.2).
        { DirectionMode::outsideIn, { 0, 3, 1, 2, 2, 1, 3, 0 } },
        { DirectionMode::asPlayed, { 0, 1, 2, 3 } },
        { DirectionMode::spiral, { 0, 1, 2, 1, 2, 3, 2, 3, 0, 3, 0, 1 } },
    };

    REQUIRE (pinned.size () == deterministicModes.size ());

    SECTION ("each mode's order is exactly its pinned literal")
    {
        for (const auto& [mode, expected] : pinned)
        {
            const auto got = traversal (mode, n);
            INFO (modeName (mode) << " at n=" << n << " → " << describe (got) << ", expected " << describe (expected));
            REQUIRE (got == expected);
        }
    }

    SECTION ("the pinned orders are pairwise distinct — except up == asPlayed, which is by design")
    {
        // THE LOAD-BEARING ASSERTION OF THIS FILE. Without it, a `switch` that fell
        // through to `up` for eight of the nine modes would still turn nine
        // `REQUIRE (got == expected)` lines green, because each of those lines only
        // says what ONE mode returns and none of them says the answers differ.
        //
        // ONE PAIR LEGITIMATELY COINCIDES, AND IT IS PINNED RATHER THAN EXEMPTED.
        // `asPlayed` is not an ordering, it is a POOL-VIEW selection: its table IS
        // the plain ascending `up` table, and what makes it "as played" is that the
        // caller walks PoolSnapshot::asPlayed instead of PoolSnapshot::sorted (see
        // `usesAsPlayedView` in DirectionModes.h and the two-views note in
        // NotePool.h). So the two tables are equal at EVERY pool size, by design —
        // asserting `equalPairs == 1` and naming which pair is stronger than
        // skipping the comparison, because it fails if a tenth mode ever collapses
        // onto another AND if asPlayed ever stops being `up`.
        int equalPairs = 0;
        std::string collisions;

        for (std::size_t i = 0; i < pinned.size (); ++i)
        {
            for (std::size_t j = i + 1; j < pinned.size (); ++j)
            {
                if (pinned[i].second == pinned[j].second)
                {
                    ++equalPairs;
                    collisions += std::string (modeName (pinned[i].first)) + "==" + modeName (pinned[j].first) + " ";
                }
            }
        }

        INFO ("colliding pairs: " << collisions);
        REQUIRE (equalPairs == 1);
        REQUIRE (collisions == "up==asPlayed ");

        // AND THE SAME ALL-PAIRS COMPARISON OVER LIVE BUILDER OUTPUT. The loop above
        // compares the LITERALS to each other, which catches the failure the brief
        // names (nine expectations that are secretly the same table). This one
        // catches the other direction: someone "repairing" a red literal by pasting
        // in whatever the builder emitted. Both loops must see exactly one collision.
        int builtEqualPairs = 0;
        std::string builtCollisions;

        for (std::size_t i = 0; i < deterministicModes.size (); ++i)
        {
            for (std::size_t j = i + 1; j < deterministicModes.size (); ++j)
            {
                if (traversal (deterministicModes[i], n) == traversal (deterministicModes[j], n))
                {
                    ++builtEqualPairs;
                    builtCollisions +=
                        std::string (modeName (deterministicModes[i])) + "==" + modeName (deterministicModes[j]) + " ";
                }
            }
        }

        INFO ("built colliding pairs: " << builtCollisions);
        REQUIRE (builtEqualPairs == 1);
        REQUIRE (builtCollisions == "up==asPlayed ");

        // …and the collision is checked against the LIVE builder, not only against
        // the literal table above, at several pool sizes.
        for (const int poolSize : { 2, 3, 4, 5, 8, 32 })
        {
            INFO ("pool size " << poolSize);
            REQUIRE (traversal (DirectionMode::up, poolSize) == traversal (DirectionMode::asPlayed, poolSize));
        }
    }

    SECTION ("the literals came from the live builder, not from each other")
    {
        // Negative control for the pairwise test: prove the builder really produced
        // nine tables (so `equalPairs == 1` is not counting nine empty vectors, which
        // would all compare equal and give 36 collisions — but a reader should not
        // have to reason that out).
        std::set<Order> distinct;

        for (const auto& entry : pinned)
        {
            const auto got = traversal (entry.first, n);
            REQUIRE_FALSE (got.empty ());
            distinct.insert (got);
        }

        REQUIRE (distinct.size () == 8); // 9 modes, one designed collision
    }
}

TEST_CASE ("sequencer/directions: converge, diverge and outsideIn stay in lockstep", "[unit]")
{
    // The three symmetric modes are expressed in terms of one `convergeIndex` helper
    // precisely so they cannot drift apart. These are the relationships that says so,
    // asserted across pool sizes rather than only at n = 4 — including ODD pool sizes,
    // where "diverge == converge with the pool flipped" would be WRONG and only
    // "diverge == the exact reverse of converge" is right.
    for (const int n : { 2, 3, 4, 5, 6, 7, 8, 15, 16, 31, 32 })
    {
        const auto conv = traversal (DirectionMode::converge, n);
        const auto div = traversal (DirectionMode::diverge, n);
        const auto oi = traversal (DirectionMode::outsideIn, n);

        INFO ("n=" << n << " converge " << describe (conv) << " · diverge " << describe (div) << " · outsideIn "
                   << describe (oi));

        REQUIRE (static_cast<int> (conv.size ()) == n);
        REQUIRE (static_cast<int> (div.size ()) == n);
        REQUIRE (static_cast<int> (oi.size ()) == 2 * n);

        // diverge is the EXACT reverse of converge.
        Order reversed (conv.rbegin (), conv.rend ());
        REQUIRE (div == reversed);

        // outsideIn is converge concatenated with diverge — the full round trip.
        Order roundTrip = conv;
        roundTrip.insert (roundTrip.end (), div.begin (), div.end ());
        REQUIRE (oi == roundTrip);

        // Each half is a PERMUTATION of the pool (every note played exactly once),
        // which is what "outside → middle" means and what a broken convergeIndex
        // would break by repeating or skipping an index.
        std::set<int> seen (conv.begin (), conv.end ());
        REQUIRE (static_cast<int> (seen.size ()) == n);

        // It starts at the OUTSIDE (index 0) and ends at the MIDDLE. For even n the
        // last converge entry is n/2 (n=4 → 0,3,1,2); for odd n it is (n-1)/2
        // (n=5 → 0,4,1,3,2) — and integer division makes both `n / 2`.
        REQUIRE (conv.front () == 0);
        REQUIRE (conv.back () == n / 2);
    }
}

TEST_CASE ("sequencer/directions: asPlayed is the only pool-view mode", "[unit]")
{
    // `usesAsPlayedView` is the ENTIRE difference between asPlayed and up — the
    // tables are identical (see the pairwise test), so if this predicate were wrong
    // for any mode, the two modes would be musically indistinguishable and nothing
    // else in the suite would notice.
    int trueCount = 0;

    for (int ordinal = 0; ordinal < numDirectionModes; ++ordinal)
    {
        const auto mode = modeAt (ordinal);
        const bool expected = (mode == DirectionMode::asPlayed);

        INFO (modeName (mode));
        REQUIRE (usesAsPlayedView (mode) == expected);

        if (usesAsPlayedView (mode))
            ++trueCount;
    }

    REQUIRE (trueCount == 1); // non-vacuous: exactly one mode, not zero
    REQUIRE (numDirectionModes == 11);
    REQUIRE (maxTraversalPeriod == 128);
}

TEST_CASE ("sequencer/directions: buildTraversal and traversalPeriod agree for every mode and pool size", "[unit]")
{
    // THE STRUCTURAL CONTRACT. A caller sizes a buffer from `traversalPeriod` and
    // fills it with `buildTraversal`; if the two ever disagree the caller either
    // overruns its buffer or reads uninitialised tail entries as pool indices. They
    // are separate functions with separate switch statements, so nothing but this
    // test keeps them in step.
    //
    // The same sweep carries the range invariant (every emitted index in [0, n)),
    // the period bound, and the sentinel check that nothing is written past `period`.
    int periodMismatches = 0;
    int outOfRangeIndices = 0;
    int overLongPeriods = 0;
    int tailWrites = 0;
    int combinations = 0;
    int totalEntries = 0;
    std::string firstOffender;

    for (int ordinal = 0; ordinal < numDirectionModes; ++ordinal)
    {
        const auto mode = modeAt (ordinal);

        // A stochastic mode's table depends on the seed, so sweep a few of them; the
        // deterministic ones ignore the seed entirely and one pass is the same work.
        for (const std::uint64_t seed : { std::uint64_t (0), std::uint64_t (1), std::uint64_t (0xDECAFBAD) })
        {
            for (int poolSize = 0; poolSize <= maxPoolSize; ++poolSize)
            {
                ++combinations;

                // No Catch2 macros in this loop — aggregated after, house idiom
                // (see the 564-combination matrix in tests/transport_timing.cpp).
                std::array<std::uint8_t, bufferSize> raw {};
                raw.fill (sentinel);

                const int declared = traversalPeriod (mode, poolSize);
                const int written = buildTraversal (mode, poolSize, seed, raw.data (), bufferSize);

                bool bad = false;

                if (declared != written)
                {
                    ++periodMismatches;
                    bad = true;
                }

                if (written > maxTraversalPeriod || written > bufferSize)
                {
                    ++overLongPeriods;
                    bad = true;
                }

                for (int k = 0; k < written; ++k)
                {
                    const int index = static_cast<int> (raw[static_cast<std::size_t> (k)]);

                    if (index < 0 || index >= poolSize)
                    {
                        ++outOfRangeIndices;
                        bad = true;
                    }
                }

                for (int k = written; k < bufferSize; ++k)
                {
                    if (raw[static_cast<std::size_t> (k)] != sentinel)
                    {
                        ++tailWrites;
                        bad = true;
                    }
                }

                totalEntries += written;

                if (bad && firstOffender.empty ())
                    firstOffender = std::string (modeName (mode)) + " n=" + std::to_string (poolSize) +
                                    " declared=" + std::to_string (declared) + " written=" + std::to_string (written);
            }
        }
    }

    INFO ("swept " << combinations << " (mode, seed, poolSize) combinations, " << totalEntries << " entries");
    INFO ("first offender: " << (firstOffender.empty () ? "none" : firstOffender));

    REQUIRE (periodMismatches == 0);
    REQUIRE (outOfRangeIndices == 0);
    REQUIRE (overLongPeriods == 0);
    REQUIRE (tailWrites == 0);
    REQUIRE (combinations == 1089); // 11 modes x 3 seeds x 33 pool sizes
    REQUIRE (totalEntries > 20000); // non-vacuous: tables really were built
}

TEST_CASE ("sequencer/directions: degenerate pools and under-capacity buffers fail safe", "[unit]")
{
    SECTION ("an empty pool yields period 0 and writes nothing, for every mode")
    {
        for (int ordinal = 0; ordinal < numDirectionModes; ++ordinal)
        {
            const auto mode = modeAt (ordinal);
            std::array<std::uint8_t, bufferSize> raw {};
            raw.fill (sentinel);

            INFO (modeName (mode));
            REQUIRE (traversalPeriod (mode, 0) == 0);
            REQUIRE (buildTraversal (mode, 0, 12345, raw.data (), bufferSize) == 0);
            REQUIRE (buildTraversal (mode, -7, 12345, raw.data (), bufferSize) == 0);
            REQUIRE (raw[0] == sentinel);
        }
    }

    SECTION ("a one-note pool is period 1, order {0}, for every mode")
    {
        for (int ordinal = 0; ordinal < numDirectionModes; ++ordinal)
        {
            const auto mode = modeAt (ordinal);
            INFO (modeName (mode));
            REQUIRE (traversalPeriod (mode, 1) == 1);
            REQUIRE (traversal (mode, 1, 999) == Order { 0 });
        }
    }

    SECTION ("a null buffer returns 0 and does not crash")
    {
        for (int ordinal = 0; ordinal < numDirectionModes; ++ordinal)
        {
            INFO (modeName (modeAt (ordinal)));
            REQUIRE (buildTraversal (modeAt (ordinal), 8, 1, nullptr, bufferSize) == 0);
        }
    }

    SECTION ("a buffer one entry short of the period writes NOTHING and returns 0")
    {
        // The header pins refusal over truncation: "a truncated traversal would
        // silently change the music, which is worse than a caller-visible failure".
        // This is also the buffer-overrun guard — a builder that filled what it could
        // would scribble past the caller's array.
        int refusals = 0;
        int exactFits = 0;
        int leaks = 0;

        for (int ordinal = 0; ordinal < numDirectionModes; ++ordinal)
        {
            const auto mode = modeAt (ordinal);

            for (int poolSize = 1; poolSize <= maxPoolSize; ++poolSize)
            {
                const int period = traversalPeriod (mode, poolSize);

                std::array<std::uint8_t, bufferSize> raw {};
                raw.fill (sentinel);

                if (buildTraversal (mode, poolSize, 7, raw.data (), period - 1) == 0)
                    ++refusals;

                for (const auto v : raw)
                    if (v != sentinel)
                        ++leaks;

                // A buffer of EXACTLY the period must succeed — otherwise "refuses
                // when short" could be implemented as "always refuses".
                raw.fill (sentinel);

                if (buildTraversal (mode, poolSize, 7, raw.data (), period) == period)
                    ++exactFits;
            }
        }

        const int cases = numDirectionModes * maxPoolSize;
        INFO ("cases " << cases << " refusals " << refusals << " exactFits " << exactFits << " leaked bytes " << leaks);
        REQUIRE (refusals == cases);
        REQUIRE (exactFits == cases);
        REQUIRE (leaks == 0);
        REQUIRE (cases == 352);
    }

    SECTION ("a zero-capacity buffer is refused")
    {
        std::array<std::uint8_t, bufferSize> raw {};
        raw.fill (sentinel);

        REQUIRE (buildTraversal (DirectionMode::up, 4, 0, raw.data (), 0) == 0);
        REQUIRE (raw[0] == sentinel);
    }
}

// ── The two stochastic modes ─────────────────────────────────────────────────
// PROPERTY TESTS ONLY. `walk` and `randomNoRepeat` are built from `splitmix64` on
// the message thread (NOT from Phase 7's audio-thread `RngStream` — DirectionModes.h
// explains why the table form is the strictly stronger determinism guarantee), and
// Phase 6 deliberately bakes no golden for either. Every test below states a
// property from §12.3 or the header, never a captured output.

TEST_CASE ("sequencer/directions: walk steps exactly ±1 and reflects at the pool ends", "[unit]")
{
    // §12.3 says "walk (±1 brownian)". Three things can silently violate that and
    // each is checked separately: a zero-length step (the walk stalls on a note), a
    // multi-step jump, and a WRAP from n-1 to 0 — a wrap is a leap across the whole
    // chord, not a walk, and it is the failure a naive `(cur + delta + n) % n` gives.
    constexpr std::array<std::uint64_t, 6> seeds { 0, 1, 42, 12345, 0xDECAFBAD, 0xFFFFFFFFFFFFFFFFULL };

    int zeroSteps = 0;
    int jumps = 0;
    int badBottomReflections = 0;
    int badTopReflections = 0;
    int outOfRange = 0;
    int wrongStart = 0;
    int bottomHits = 0;
    int topHits = 0;
    int totalSteps = 0;
    std::string firstOffender;

    for (const auto seed : seeds)
    {
        for (int n = 2; n <= maxPoolSize; ++n)
        {
            const auto order = traversal (DirectionMode::walk, n, seed);

            if (static_cast<int> (order.size ()) != 4 * n)
            {
                ++jumps; // wrong period: counted as a structural failure
                continue;
            }

            // The header pins the starting point: "walk starts at n / 2".
            if (order.front () != n / 2)
                ++wrongStart;

            for (std::size_t k = 0; k < order.size (); ++k)
            {
                const int cur = order[k];

                if (cur < 0 || cur >= n)
                    ++outOfRange;

                if (k == 0)
                    continue;

                const int prev = order[k - 1];
                const int delta = cur - prev;
                ++totalSteps;

                if (delta == 0)
                {
                    ++zeroSteps;

                    if (firstOffender.empty ())
                        firstOffender = "zero step, n=" + std::to_string (n) + " k=" + std::to_string (k);
                }
                else if (delta != 1 && delta != -1)
                {
                    ++jumps;

                    if (firstOffender.empty ())
                        firstOffender = "jump of " + std::to_string (delta) + ", n=" + std::to_string (n) +
                                        " k=" + std::to_string (k);
                }

                // REFLECTION, not wrapping: from the bottom the only legal next
                // index is 1, and from the top it is n-2. (n >= 2 throughout, so
                // those two are always distinct from their origin.)
                if (prev == 0)
                {
                    ++bottomHits;

                    if (cur != 1)
                        ++badBottomReflections;
                }

                if (prev == n - 1)
                {
                    ++topHits;

                    if (cur != n - 2)
                        ++badTopReflections;
                }
            }
        }
    }

    INFO ("first offender: " << (firstOffender.empty () ? "none" : firstOffender));
    INFO ("steps " << totalSteps << ", bottom reflections " << bottomHits << ", top reflections " << topHits);

    REQUIRE (zeroSteps == 0);
    REQUIRE (jumps == 0);
    REQUIRE (outOfRange == 0);
    REQUIRE (wrongStart == 0);
    REQUIRE (badBottomReflections == 0);
    REQUIRE (badTopReflections == 0);

    // ── NON-VACUITY FLOORS — NOT A DISTRIBUTIONAL CLAIM (issue #63) ──────────
    // The reflection assertions above are CONDITIONAL, so a walk that never reached
    // an endpoint would satisfy them trivially. The two thresholds below exist for
    // ONE reason: to say both ends really were hit, repeatedly, across the sweep.
    //
    // WHAT THEY ARE: floors under MEASURED counts for this exact, fixed seed set —
    // 525 bottom and 68 top reflections. They are re-measured numbers, not derived
    // ones.
    //
    // WHAT THEY ARE NOT — READ THIS BEFORE "FIXING" ANYTHING: they are not a
    // uniformity, symmetry or distributional contract on `walk`, and the gap between
    // 525 and 68 is not a defect to be corrected. §12.3 specifies `walk` as "±1
    // brownian", and the properties that ARE the contract — every step exactly ±1,
    // every reflection correct, never a wrap, always in range, always starting at
    // n / 2 — are the unconditional counters asserted above, all of which are green.
    //
    // WHY THE LOPSIDEDNESS: the bias sequence is `splitmix64 (seed ^ 0x5741 ^ k)`
    // evaluated PER STEP INDEX — a hash, not a balanced sequential stream. Low-entropy
    // inputs (small seeds, small k) produce a visibly skewed low bit: seed 0 alone
    // contributes 157 bottom hits against 7 top. A hash makes no equidistribution
    // promise over a handful of near-zero inputs, so this is a property of the chosen
    // constants working as designed.
    //
    // WHEN THESE NUMBERS MUST BE RE-MEASURED: PHASE 12's seed engine. §5.2 composes
    // the effective stream seed as
    // `splitmix64 (masterSeed ^ operatorSeed ^ (loopLock ? 0 : barCounter))`, and
    // DirectionModes.cpp already carries a Phase 12 note about deleting its local
    // `splitmix64` in favour of the shared one. Any change to `walkSalt` (0x5741),
    // to the mixing function, or to how `k` enters the hash SHIFTS THESE COUNTS.
    // The correct response is to re-measure and re-pin the floors — that is expected
    // maintenance, NOT a regression, and not grounds for weakening or deleting the
    // thresholds. What must stay green through such a change is the unconditional
    // block above; only these two floors are seed-set-specific.
    REQUIRE (bottomHits > 100);    // measured 525 — floor, not an expectation
    REQUIRE (topHits > 25);        // measured 68  — floor, not an expectation
    REQUIRE (totalSteps == 12462); // 6 seeds x Σ(n=2..32) (4n-1)
}

TEST_CASE ("sequencer/directions: randomNoRepeat never repeats, including across the loop point", "[unit]")
{
    constexpr std::array<std::uint64_t, 6> seeds { 0, 1, 42, 12345, 0xDECAFBAD, 0xFFFFFFFFFFFFFFFFULL };

    SECTION ("no two consecutive entries are equal, and the last differs from the first")
    {
        // The loop-point clause is the one a naive implementation misses: the table
        // is cyclic, so order[period-1] sitting next to order[0] is a real adjacency
        // that a plain "differs from previous" loop never examines.
        int consecutiveRepeats = 0;
        int loopPointRepeats = 0;
        int outOfRange = 0;
        int wrongPeriod = 0;
        int tables = 0;
        int totalEntries = 0;
        std::string firstOffender;

        for (const auto seed : seeds)
        {
            for (int n = 2; n <= maxPoolSize; ++n)
            {
                const auto order = traversal (DirectionMode::randomNoRepeat, n, seed);
                ++tables;

                if (static_cast<int> (order.size ()) != 4 * n)
                {
                    ++wrongPeriod;
                    continue;
                }

                totalEntries += static_cast<int> (order.size ());

                for (std::size_t k = 0; k < order.size (); ++k)
                {
                    if (order[k] < 0 || order[k] >= n)
                        ++outOfRange;

                    if (k > 0 && order[k] == order[k - 1])
                    {
                        ++consecutiveRepeats;

                        if (firstOffender.empty ())
                            firstOffender = "repeat at n=" + std::to_string (n) + " k=" + std::to_string (k);
                    }
                }

                if (order.back () == order.front ())
                {
                    ++loopPointRepeats;

                    if (firstOffender.empty ())
                        firstOffender = "loop-point repeat at n=" + std::to_string (n);
                }
            }
        }

        INFO ("first offender: " << (firstOffender.empty () ? "none" : firstOffender));
        INFO (tables << " tables, " << totalEntries << " entries");

        REQUIRE (consecutiveRepeats == 0);
        REQUIRE (loopPointRepeats == 0);
        REQUIRE (outOfRange == 0);
        REQUIRE (wrongPeriod == 0);
        REQUIRE (tables == 186);
        REQUIRE (totalEntries > 10000);
    }

    SECTION ("poolSize 2 — the case where termination is nearly false")
    {
        // WHY THIS SECTION EXISTS, AND WHY IT MUST NOT BE DELETED.
        //
        // The builder rejection-samples a candidate that differs from the PREVIOUS
        // entry, and for the FINAL entry additionally from order[0]. At n == 2 that
        // is two exclusions over a two-element set — one more exclusion than the set
        // can absorb. It terminates only because the two exclusions COINCIDE:
        //
        //   * n == 2 forces strict alternation (the only value differing from the
        //     previous entry is the other one), so the table is ababab…
        //   * the period is 4n == 8, which is EVEN, so the entry before the last
        //     (index period-2) is at an even offset from index 0 and therefore
        //     EQUALS order[0]. "differ from the previous entry" and "differ from
        //     order[0]" then exclude the same single value, leaving one candidate.
        //
        // If a future change widens the exclusion set (say, "differ from the last
        // TWO entries", or a no-repeat window), or changes the period away from a
        // multiple of 2, n == 2 stops having a legal final entry. The builder's
        // rejection loop is capped at 64 attempts and falls through to a
        // deterministic scan rather than hanging — but the scan would produce a
        // REPEAT, silently breaking the invariant. This section is the tripwire.
        for (const auto seed : seeds)
        {
            const auto order = traversal (DirectionMode::randomNoRepeat, 2, seed);

            INFO ("seed " << seed << " → " << describe (order));
            REQUIRE (order.size () == 8); // 4n

            // A clean alternation over the FULL period, loop point included.
            for (std::size_t k = 0; k < order.size (); ++k)
            {
                INFO ("entry " << k);
                REQUIRE (order[k] == ((k % 2 == 0) ? order[0] : 1 - order[0]));
            }

            REQUIRE (order.back () != order.front ());
            REQUIRE (order.back () == 1 - order.front ());
        }
    }

    SECTION ("the table is not a near-constant — it spreads over the pool")
    {
        // Guards a builder that emits, say, only two alternating indices for a large
        // pool: that would satisfy every no-repeat assertion above while being
        // musically nothing like "uniform".
        //
        // FULL COVERAGE IS *NOT* ASSERTED, AND THAT IS MEASURED, NOT ASSUMED. Over
        // a period of only 4n uniform draws, some pool index missing entirely is the
        // expected outcome, not a defect: P(a given index never drawn) ≈ (1-1/n)^4n
        // → e^-4 ≈ 1.8%, so a 32-note pool misses at least one index a good fraction
        // of the time. Sweeping n = 2..32 over 3000 seeds, the worst observed
        // coverage was 2/3 of the pool. The bound below sits under that with margin
        // while still being far above what any degenerate builder could reach.
        int worstDistinct = maxPoolSize + 1;
        int worstN = 0;
        int belowHalf = 0;

        for (const auto seed : seeds)
        {
            for (int n = 2; n <= maxPoolSize; ++n)
            {
                const auto order = traversal (DirectionMode::randomNoRepeat, n, seed);
                const std::set<int> distinct (order.begin (), order.end ());
                const int count = static_cast<int> (distinct.size ());

                if (count < (n + 1) / 2)
                    ++belowHalf;

                if (count < worstDistinct)
                {
                    worstDistinct = count;
                    worstN = n;
                }
            }
        }

        INFO ("worst distinct-index count " << worstDistinct << " (at n=" << worstN << ")");
        REQUIRE (belowHalf == 0); // never fewer than half the pool's notes
        REQUIRE (worstDistinct >= 2);
    }
}

TEST_CASE ("sequencer/directions: the stochastic tables are pure functions of their seed", "[unit]")
{
    // §5.2's LOOP LOCK falls out of the table form for free — "a table IS a locked
    // loop" — but only if the table is reproducible. Same seed ⇒ same table is the
    // determinism half; different seed ⇒ different table is the anti-vacuity half
    // (a builder that ignored the seed would satisfy the first clause perfectly).
    constexpr std::array<std::uint64_t, 6> seeds { 0, 1, 42, 12345, 0xDECAFBAD, 0xFFFFFFFFFFFFFFFFULL };

    SECTION ("same seed ⇒ byte-identical table")
    {
        for (const auto mode : { DirectionMode::walk, DirectionMode::randomNoRepeat })
        {
            for (const auto seed : seeds)
            {
                for (const int n : { 2, 3, 7, 8, 16, 32 })
                {
                    INFO (modeName (mode) << " seed " << seed << " n=" << n);
                    REQUIRE (traversal (mode, n, seed) == traversal (mode, n, seed));
                }
            }
        }
    }

    SECTION ("different seed ⇒ different table, for pools large enough to have a choice")
    {
        // POOL SIZES 8 AND UP, DELIBERATELY. At n == 2 the walk is FULLY DETERMINED
        // regardless of seed — reflection at both ends of a two-note pool forces the
        // alternation 1,0,1,0,… — and randomNoRepeat at n == 2 differs between seeds
        // only in which of the two notes it starts on. Small pools also collide
        // occasionally at n = 3 and n = 4 (verified by sweeping 40 seeds: 8 colliding
        // walk pairs at n=3, 1 at n=4, 2 colliding randomNoRepeat pairs at n=3, none
        // above n=4). None of that is a defect — it is a two-note pool having only
        // one musically valid walk — so the assertion is made where a difference is
        // actually meaningful.
        int comparisons = 0;
        int collisions = 0;
        std::string firstCollision;

        for (const auto mode : { DirectionMode::walk, DirectionMode::randomNoRepeat })
        {
            for (const int n : { 8, 16, 32 })
            {
                for (std::size_t i = 0; i < seeds.size (); ++i)
                {
                    for (std::size_t j = i + 1; j < seeds.size (); ++j)
                    {
                        ++comparisons;

                        if (traversal (mode, n, seeds[i]) == traversal (mode, n, seeds[j]))
                        {
                            ++collisions;

                            if (firstCollision.empty ())
                                firstCollision = std::string (modeName (mode)) + " n=" + std::to_string (n);
                        }
                    }
                }
            }
        }

        INFO ("first collision: " << (firstCollision.empty () ? "none" : firstCollision));
        REQUIRE (collisions == 0);
        REQUIRE (comparisons == 90); // 2 modes x 3 pool sizes x 15 seed pairs
    }

    SECTION ("the deterministic modes ignore the seed entirely")
    {
        // The complementary half: a builder that let a seed leak into `converge`
        // would break the "pure function of (mode, n)" contract the header states,
        // and every golden built at a different masterSeed would drift.
        for (const auto mode : deterministicModes)
        {
            for (const int n : { 2, 5, 16, 32 })
            {
                INFO (modeName (mode) << " n=" << n);
                const auto reference = traversal (mode, n, 0);

                for (const auto seed : seeds)
                    REQUIRE (traversal (mode, n, seed) == reference);
            }
        }
    }

    SECTION ("the two stochastic modes are not the same table under the same seed")
    {
        // The domain-separation salts (walkSalt / randomSalt) exist so a walk and a
        // randomNoRepeat built from ONE masterSeed are uncorrelated. Identical tables
        // would mean the salts were dropped.
        for (const auto seed : seeds)
        {
            for (const int n : { 8, 16, 32 })
            {
                INFO ("seed " << seed << " n=" << n);
                REQUIRE (traversal (DirectionMode::walk, n, seed) !=
                         traversal (DirectionMode::randomNoRepeat, n, seed));
            }
        }
    }
}

TEST_CASE ("sequencer/directions: every mode is either a permutation-style table or a seeded one", "[unit]")
{
    // A structural cross-check that no mode was left out of the switch: for every
    // deterministic mode, the table must be reproducible AND must cover every pool
    // index at least once over its period (a traversal that never reaches a held note
    // is not a traversal). The stochastic modes are exempted for the reason spelled
    // out in the randomNoRepeat coverage section above.
    int uncoveredModes = 0;
    int checked = 0;
    std::string firstOffender;

    for (const auto mode : deterministicModes)
    {
        REQUIRE_FALSE (isStochastic (mode));

        for (int n = 1; n <= maxPoolSize; ++n)
        {
            ++checked;
            const auto order = traversal (mode, n, 0);
            const std::set<int> distinct (order.begin (), order.end ());

            if (static_cast<int> (distinct.size ()) != n)
            {
                ++uncoveredModes;

                if (firstOffender.empty ())
                    firstOffender = std::string (modeName (mode)) + " n=" + std::to_string (n) + " covered " +
                                    std::to_string (distinct.size ());
            }
        }
    }

    INFO ("first offender: " << (firstOffender.empty () ? "none" : firstOffender));
    REQUIRE (uncoveredModes == 0);
    REQUIRE (checked == 288); // 9 modes x 32 pool sizes
}
