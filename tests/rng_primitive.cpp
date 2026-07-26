// ─────────────────────────────────────────────────────────────────────────────
// rng_primitive — Phase 7.1: the BIT-IDENTITY PINS for engine/generative/Rng.h
// (ARCHITECTURE §5.2 "Seeds & reversibility", §1.2's determinism contract).
//
// ── WHY THIS FILE EXISTS AT ALL, WHICH IS NOT OBVIOUS ───────────────────────
// `splitmix64` is the only randomness in the audible path today, and the suite
// was BLIND to it before this file. Specifically:
//
//   * The six baked goldens do not touch it. `walk` and `randomNoRepeat` were
//     deliberately given NO golden files — see the omission note at
//     tests/determinism_goldens.cpp:41-55 — precisely so Phase 7.1 would not
//     force a regeneration. Nothing in tests/golden/ hashes through splitmix64.
//   * tests/pattern_directions.cpp covers the two stochastic modes with PROPERTY
//     tests only: period, ±1 steps, reflection at the pool ends, no repeat across
//     the loop point, purity in the seed. EVERY ONE OF THOSE SURVIVES A CHANGED
//     CONSTANT. A different multiplier still yields a valid reflecting walk; it
//     is just a DIFFERENT one, and every saved project using that mode changes
//     sound silently.
//
// So a mistyped shift or multiplier was, until this file, a determinism-contract
// break that the entire 202-test suite reported as green. That is the failure
// class this file closes. Rng.h's `static_assert` is the compile-time half; this
// is the runtime half, and it additionally covers what the assert cannot: the
// three EXACT CALL EXPRESSIONS in DirectionModes.cpp, whose XOR shape and salts
// are as much a part of the frozen output as the hash body is.
//
// ── WHY [unit] AND [determinism] BOTH ───────────────────────────────────────
// [unit] by the file's own convention — every literal below was computed
// independently and can be re-derived with a pencil or a five-line script, not
// read out of a frozen render. [determinism] because a failure here IS a §1.2
// contract break and belongs in the gate CI runs as `ctest -L determinism`.
//
// ── THE INDEPENDENT REFERENCE, AND WHY IT IS NOT CIRCULAR ───────────────────
// `referenceSplitmix64` below is a SECOND writing of the algorithm with its own
// literals, used to drive a local re-derivation of the two stochastic traversal
// tables which is then compared against the production `buildTraversal`. On its
// own that would be near-circular (two copies of one mistake agree), so it is
// paired with hard-coded known-answer vectors — including splitmix64's published
// output for seed 0 — and with two literal traversal tables. The reference
// catches a drift on EITHER side of the migration; the literals catch a drift
// that happens to be copied to both.
// ─────────────────────────────────────────────────────────────────────────────

#include "engine/generative/Rng.h"
#include "engine/sequencer/DirectionModes.h"
#include "engine/sequencer/PatternTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

using arpbox::engine::DirectionMode;
using arpbox::engine::direction::buildTraversal;
using arpbox::engine::direction::maxTraversalPeriod;
using arpbox::engine::rng::domainSalt;
using arpbox::engine::rng::RngDomain;
using arpbox::engine::rng::rngVersion;
using arpbox::engine::rng::splitmix64;
using arpbox::engine::rng::stepHash;

namespace
{
/** A SECOND, independent writing of splitmix64 — its own literals, typed from the
    published algorithm rather than copied from Rng.h. Used only to re-derive the
    traversal tables below; the known-answer vectors are what keep the pair from
    agreeing on a shared mistake. */
constexpr std::uint64_t referenceSplitmix64 (std::uint64_t x) noexcept
{
    std::uint64_t z = x + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/** The salt literals as they stood in DirectionModes.cpp BEFORE the Phase-7.1
    migration, written here as raw numbers. This is the test's whole leverage on
    the registry: `RngDomain` could be edited, but these cannot be, so the two
    must keep agreeing. */
constexpr std::uint64_t historicWalkSalt = 0x5741ULL;
constexpr std::uint64_t historicRandomSalt = 0x9E37ULL;

/** DirectionModes.cpp's `walk` table, re-derived here from the reference hash.
    Mirrors the production algorithm deliberately — its job is to detect a change
    in the HASH or the SALT, not in the reflection rule (pattern_directions.cpp
    owns that). */
std::vector<int> referenceWalk (int n, std::uint64_t seed)
{
    const int period = 4 * n;
    std::vector<int> out;
    out.reserve (static_cast<std::size_t> (period));

    int cur = n / 2;
    out.push_back (cur);

    for (int k = 1; k < period; ++k)
    {
        const bool stepUp =
            (referenceSplitmix64 (seed ^ historicWalkSalt ^ static_cast<std::uint64_t> (k)) & 1ULL) != 0ULL;

        int next = stepUp ? cur + 1 : cur - 1;

        if (next < 0)
            next = 1;
        else if (next > n - 1)
            next = n - 2;

        cur = next;
        out.push_back (cur);
    }

    return out;
}

/** DirectionModes.cpp's `randomNoRepeat` table, re-derived from the reference
    hash — including the `attempt << 32` rejection term and the extra exclusion on
    the final entry, both of which are part of the frozen expression. */
std::vector<int> referenceRandomNoRepeat (int n, std::uint64_t seed)
{
    const int period = 4 * n;
    std::vector<int> out;
    out.reserve (static_cast<std::size_t> (period));

    out.push_back (static_cast<int> (referenceSplitmix64 (seed ^ historicRandomSalt) % static_cast<std::uint64_t> (n)));

    for (int k = 1; k < period; ++k)
    {
        const int prev = out.back ();
        const bool isLast = (k == period - 1);
        const int firstEntry = out.front ();
        int chosen = -1;

        for (int attempt = 0; attempt < 64 && chosen < 0; ++attempt)
        {
            const std::uint64_t hash = referenceSplitmix64 (seed ^ historicRandomSalt ^ static_cast<std::uint64_t> (k) ^
                                                            (static_cast<std::uint64_t> (attempt) << 32));
            const int candidate = static_cast<int> (hash % static_cast<std::uint64_t> (n));

            if (candidate == prev)
                continue;

            if (isLast && candidate == firstEntry)
                continue;

            chosen = candidate;
        }

        out.push_back (chosen);
    }

    return out;
}

/** `buildTraversal` into a plain vector of ints, so it compares against the
    reference builders above without a per-test cast dance. */
std::vector<int> production (DirectionMode mode, int poolSize, std::uint64_t seed)
{
    std::uint8_t buffer[maxTraversalPeriod] = {};
    const int period = buildTraversal (mode, poolSize, seed, buffer, maxTraversalPeriod);

    std::vector<int> out;
    out.reserve (static_cast<std::size_t> (period));

    for (int k = 0; k < period; ++k)
        out.push_back (static_cast<int> (buffer[k]));

    return out;
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1. THE HASH ITSELF
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("rng/splitmix64: known-answer vectors are exact", "[unit][determinism]")
{
    // 0xE220A8397B1DCDAF is splitmix64's published first output for seed 0 — the
    // canonical vector, and the same one Rng.h asserts at compile time. Repeated
    // here on purpose: the static_assert dies with the build, so the RUNNING
    // suite would otherwise have no statement about this value at all.
    REQUIRE (splitmix64 (0ULL) == 0xE220A8397B1DCDAFULL);
    REQUIRE (splitmix64 (1ULL) == 0x910A2DEC89025CC1ULL);
    REQUIRE (splitmix64 (2ULL) == 0x975835DE1C9756CEULL);
    REQUIRE (splitmix64 (3ULL) == 0x1D0B14E4DB018FEDULL);

    // The gamma itself, and the all-ones input: the two arguments most likely to
    // expose a wrong `+=`, a signed shift, or a missing 64-bit truncation.
    REQUIRE (splitmix64 (0x9E3779B97F4A7C15ULL) == 0x6E789E6AA1B965F4ULL);
    REQUIRE (splitmix64 (0xFFFFFFFFFFFFFFFFULL) == 0xE4D971771B652C20ULL);

    // An arbitrary high-entropy input, so the vector set is not all small
    // integers clustered near one point of the gamma sequence.
    REQUIRE (splitmix64 (0xDEADBEEFCAFEF00DULL) == 0x901D4F652FB472CBULL);
}

TEST_CASE ("rng/splitmix64: the shared primitive matches an independent writing", "[unit][determinism]")
{
    // Anti-vacuity for the reference implementation the traversal comparisons
    // below lean on: if `referenceSplitmix64` had itself drifted, those
    // comparisons would fail for a reason that has nothing to do with production
    // code. Sweep a wide, structured input set rather than a handful of points.
    for (std::uint64_t i = 0; i < 4096; ++i)
    {
        REQUIRE (splitmix64 (i) == referenceSplitmix64 (i));
        REQUIRE (splitmix64 (~i) == referenceSplitmix64 (~i));
        REQUIRE (splitmix64 (i << 32) == referenceSplitmix64 (i << 32));
    }
}

TEST_CASE ("rng/splitmix64: distinct inputs give distinct, well-spread outputs", "[unit]")
{
    // A hash that collapsed to a constant would satisfy every property test in
    // pattern_directions.cpp for n == 1 and would still produce a "valid" walk.
    // Two cheap non-degeneracy statements close that.
    std::uint64_t orBits = 0;
    std::uint64_t andBits = ~0ULL;

    for (std::uint64_t i = 0; i < 1024; ++i)
    {
        const std::uint64_t h = splitmix64 (i);
        orBits |= h;
        andBits &= h;

        REQUIRE (h != splitmix64 (i + 1));
    }

    // Every bit position must have been seen both set and clear across the sweep.
    REQUIRE (orBits == ~0ULL);
    REQUIRE (andBits == 0ULL);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. THE DOMAIN REGISTRY
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("rng/domains: the registry values are frozen and mutually distinct", "[unit][determinism]")
{
    // The two direction salts are pinned against the raw literals DirectionModes
    // used before Phase 7.1. Editing `RngDomain` to "tidy" these would rewrite
    // every walk / randomNoRepeat table ever produced.
    REQUIRE (domainSalt (RngDomain::directionWalk) == historicWalkSalt);
    REQUIRE (domainSalt (RngDomain::directionRandomNoRepeat) == historicRandomSalt);

    // The Phase 7 additions, pinned the same way so a later "append" that
    // accidentally renumbers an existing entry reddens here.
    REQUIRE (domainSalt (RngDomain::stepProbability) == 0x5052ULL);
    REQUIRE (domainSalt (RngDomain::ratchetProbability) == 0x5243ULL);

    // Domain separation is the enum's entire purpose: two equal salts would make
    // two consumers driven by one masterSeed produce correlated output, and no
    // test of either consumer alone would notice.
    const RngDomain all[] = { RngDomain::directionWalk,
                              RngDomain::directionRandomNoRepeat,
                              RngDomain::stepProbability,
                              RngDomain::ratchetProbability };

    // Compared BY POSITION, not by enum value: `domainSalt` is the underlying
    // value, so `a != b ⇒ domainSalt(a) != domainSalt(b)` is a tautology and a
    // value-keyed loop would prove nothing. What can actually go wrong is two
    // ENUMERATORS sharing one value, and only an index-keyed pass sees that.
    for (std::size_t i = 0; i < std::size (all); ++i)
        for (std::size_t j = i + 1; j < std::size (all); ++j)
            REQUIRE (domainSalt (all[i]) != domainSalt (all[j]));

    // §8.1's serialized key. Phase 7.1 is version 1; a bump without a migration
    // note is exactly what §5.2 forbids, so pin the value by literal.
    REQUIRE (rngVersion == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. stepHash
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("rng/stepHash: is a pure function of (seed, domain, index)", "[unit][determinism]")
{
    // Purity: same inputs, same answer, every time. The emission core evaluates
    // FUTURE steps out of order through the retrigger lookahead, so anything
    // order-dependent here becomes a buffer-size-dependent note position one
    // level up — the #36/#46/#48 failure class.
    for (std::int64_t i = -8; i <= 8; ++i)
    {
        const auto first = stepHash (0x1234ULL, RngDomain::stepProbability, i);

        for (int repeat = 0; repeat < 4; ++repeat)
            REQUIRE (stepHash (0x1234ULL, RngDomain::stepProbability, i) == first);
    }

    // Exact values, computed independently. Two nested splitmix64 calls:
    //   inner = splitmix64 (masterSeed ^ salt)   <- §5.2's effective stream seed
    //   outer = splitmix64 (inner ^ index)       <- §12.3's per-index hash
    REQUIRE (stepHash (0ULL, RngDomain::stepProbability, 0) == 0xDB82CB8632E94DFBULL);
    REQUIRE (stepHash (0ULL, RngDomain::stepProbability, 1) == 0x1F5D956715E20741ULL);
    REQUIRE (stepHash (0xABCDEF0123456789ULL, RngDomain::stepProbability, 37) == 0xA49BFE51240E1441ULL);

    // NEGATIVE INDICES ARE LEGAL AND MUST NOT WRAP TO A NEIGHBOUR. Locate paths
    // and the lookahead both sweep below zero (step_purity.cpp starts at -37).
    // Index -1 converts to 0xFFFF'FFFF'FFFF'FFFF, which is well defined.
    REQUIRE (stepHash (0ULL, RngDomain::stepProbability, -1) == 0x2D8DE66A53A7F99BULL);
    REQUIRE (stepHash (0x1234ULL, RngDomain::stepProbability, -37) == 0x2C2E95226207B76BULL);
    REQUIRE (stepHash (0ULL, RngDomain::stepProbability, -1) != stepHash (0ULL, RngDomain::stepProbability, 1));

    // The two-call structure spelled out, so a future collapse into one call (an
    // obvious-looking simplification that would break Phase 12's ability to add
    // the operatorSeed / loop-lock XOR terms without moving a Phase-7 bit)
    // reddens here rather than in a golden three phases later.
    REQUIRE (stepHash (0ULL, RngDomain::stepProbability, 5) == splitmix64 (splitmix64 (0ULL ^ 0x5052ULL) ^ 5ULL));
    REQUIRE (stepHash (0ULL, RngDomain::stepProbability, 5) != splitmix64 (0ULL ^ 0x5052ULL ^ 5ULL));
}

TEST_CASE ("rng/stepHash: separates domains, seeds and indices", "[unit][determinism]")
{
    constexpr std::uint64_t seed = 0xABCDEF0123456789ULL;

    // Same seed, same index, different domain ⇒ different word. This is the
    // property that lets one masterSeed drive PROB, ratchet probability and two
    // direction tables without any of them tracking any other.
    for (std::int64_t i = -4; i <= 64; ++i)
    {
        const auto walkHash = stepHash (seed, RngDomain::directionWalk, i);
        const auto randHash = stepHash (seed, RngDomain::directionRandomNoRepeat, i);
        const auto probHash = stepHash (seed, RngDomain::stepProbability, i);
        const auto ratchetHash = stepHash (seed, RngDomain::ratchetProbability, i);

        REQUIRE (walkHash != randHash);
        REQUIRE (walkHash != probHash);
        REQUIRE (walkHash != ratchetHash);
        REQUIRE (randHash != probHash);
        REQUIRE (randHash != ratchetHash);
        REQUIRE (probHash != ratchetHash);
    }

    // Adjacent indices and adjacent seeds must not alias — the inner call exists
    // so that `masterSeed + 1` is a genuinely different stream rather than the
    // same one shifted by one step.
    for (std::int64_t i = 0; i < 64; ++i)
    {
        REQUIRE (stepHash (seed, RngDomain::stepProbability, i) != stepHash (seed, RngDomain::stepProbability, i + 1));
        REQUIRE (stepHash (seed, RngDomain::stepProbability, i) != stepHash (seed + 1, RngDomain::stepProbability, i));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. THE MIGRATION: DirectionModes' THREE CALL EXPRESSIONS
//
// The Phase-7.1 migration deleted DirectionModes.cpp's local `splitmix64` and its
// two salt literals in favour of Rng.h. The claim attached to that commit is that
// the output did not move by one bit. These are the tests that make the claim
// checkable instead of merely asserted — the six goldens cannot, because neither
// stochastic mode is baked into any of them.
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("rng/direction-migration: the three call expressions produce their frozen words", "[unit][determinism]")
{
    // Expression 1 — DirectionModes.cpp, `walk`: the reflection bit for ordinal k.
    //     splitmix64 (seed ^ walkSalt ^ uint64 (k))
    REQUIRE ((splitmix64 (0ULL ^ historicWalkSalt ^ 1ULL)) == 0x43FC369C31928E97ULL);
    REQUIRE ((splitmix64 (0ULL ^ historicWalkSalt ^ 2ULL)) == 0x0B7BFC60E4F2A8DEULL);
    REQUIRE ((splitmix64 (0x1234ULL ^ historicWalkSalt ^ 3ULL)) == 0x38A556C024335897ULL);
    REQUIRE ((splitmix64 (0xABCDEF0123456789ULL ^ historicWalkSalt ^ 7ULL)) == 0xB9C448979296CE07ULL);

    // Expression 2 — `randomNoRepeat`, the FIRST entry (no k, no attempt term).
    //     splitmix64 (seed ^ randomSalt)
    REQUIRE ((splitmix64 (0ULL ^ historicRandomSalt)) == 0x1DE68641D0469743ULL);
    REQUIRE ((splitmix64 (0x1234ULL ^ historicRandomSalt)) == 0xDEAEFE401EFA9584ULL);
    REQUIRE ((splitmix64 (0xABCDEF0123456789ULL ^ historicRandomSalt)) == 0x78EC66A9AC4AD210ULL);

    // Expression 3 — `randomNoRepeat`, the rejection draw. The `attempt << 32`
    // term is what keeps retry 1 from re-drawing retry 0's word, so it is pinned
    // at BOTH attempts: dropping the shift would leave attempt 0 correct and only
    // attempt 1 wrong, which a single-attempt vector would miss.
    REQUIRE ((splitmix64 (0ULL ^ historicRandomSalt ^ 1ULL ^ (0ULL << 32))) == 0x97AA37D82A32A0FDULL);
    REQUIRE ((splitmix64 (0ULL ^ historicRandomSalt ^ 1ULL ^ (1ULL << 32))) == 0x34D9C8FA6EBE16AAULL);
    REQUIRE ((splitmix64 (0x1234ULL ^ historicRandomSalt ^ 5ULL ^ (0ULL << 32))) == 0x88A05C239E98684BULL);
    REQUIRE ((splitmix64 (0x1234ULL ^ historicRandomSalt ^ 5ULL ^ (1ULL << 32))) == 0xFC755084A63C2C74ULL);
}

TEST_CASE ("rng/direction-migration: walk and randomNoRepeat tables are bit-identical", "[unit][determinism]")
{
    // Two literal tables, computed independently and written out here. These are
    // the closest thing walk / randomNoRepeat have to a golden file, and they are
    // what catches a shared mistake between production and `referenceSplitmix64`.
    const std::vector<int> walkPinned = { 4, 3, 2, 3, 2, 3, 2, 1, 0, 1, 2, 1, 2, 1, 0, 1,
                                          2, 1, 0, 1, 2, 1, 0, 1, 0, 1, 2, 3, 4, 3, 4, 3 };
    REQUIRE (production (DirectionMode::walk, 8, 0x1234ULL) == walkPinned);

    const std::vector<int> randomPinned = { 4, 1, 3, 2, 3, 1, 2, 3, 1, 2, 4, 3, 2, 0, 4, 2, 3, 0, 3, 1 };
    REQUIRE (production (DirectionMode::randomNoRepeat, 5, 0xABCDEF0123456789ULL) == randomPinned);

    // Then the same claim across a broad (poolSize, seed) grid against the
    // independent reference, so the two literal tables are not the only coverage
    // and a drift that happens to leave n = 8 / n = 5 alone still reddens.
    const std::uint64_t seeds[] = { 0ULL, 1ULL, 0x1234ULL, 0xABCDEF0123456789ULL, 0xFFFFFFFFFFFFFFFFULL };

    for (int n = 2; n <= 16; ++n)
    {
        for (const auto seed : seeds)
        {
            REQUIRE (production (DirectionMode::walk, n, seed) == referenceWalk (n, seed));
            REQUIRE (production (DirectionMode::randomNoRepeat, n, seed) == referenceRandomNoRepeat (n, seed));
        }
    }
}
