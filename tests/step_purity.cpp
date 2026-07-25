// ─────────────────────────────────────────────────────────────────────────────
// step_purity — the BEHAVIOURAL half of issue #53's enforcement.
//
// ── WHAT IS BEING PROTECTED, AND WHY IT IS NOT OBVIOUS ──────────────────────
// `SequencerProcessor::cutoffForSamePitch` schedules a note-off by PREDICTING what
// steps k+1, k+2, … will emit (the issue #46 fix). A prediction is only sound if
// asking about a step gives the same answer as eventually emitting it — i.e. if
// `evaluateStep (snapshot, patternIndex, stepIndex)` is a PURE function of those
// three arguments. If it ever consults a running RNG cursor, an accumulator, or a
// `PRE`/`NEI` result cached by the emitting walk, then the answer starts depending
// on the ORDER and COUNT of calls, the lookahead diverges from emission, and the
// note-off placement becomes a function of how the timeline was carved into
// blocks. That is exactly the #36 / #46 / #48 failure class, resurrected one level
// up, and Phase 6 spent most of its budget there.
//
// ── HOW THIS FILE DIVIDES THE WORK WITH THE COMPILER ────────────────────────
// The compile-time half is the SIGNATURE: `evaluateStep` is a free function at
// namespace scope, so no member of `SequencerProcessor` — the obvious home for a
// Phase 7.1 RNG cursor — is nameable from inside it. That half is checked by the
// build, needs no test, and cannot be "temporarily" bypassed.
//
// This file covers what a type system cannot see: mutable state parked SOMEWHERE
// ELSE. A file-scope `static` in SequencerProcessor.cpp, a function-local
// `static`, a thread_local, a memo keyed on the snapshot pointer — none of those
// are member state, all of them compile, and every one of them makes the result
// depend on call history. So the strategy here is not "call it and check the
// value" (that would only re-derive the pattern core, which pattern_polymeter.cpp
// already does against a hand-written closed form). It is:
//
//     take an IN-ORDER reference sweep, then demand the identical answers back
//     under call orders no cursor could survive.
//
// Five perturbations, each defeating a different shape of impurity:
//   1. REPEATED     — same index four times. Any per-call advance reddens.
//   2. REVERSED     — descending sweep. A cursor keyed on call count reddens.
//   3. SHUFFLED     — a seeded permutation. Defeats a cursor that happens to be
//                     symmetric under reversal.
//   4. INTERLEAVED  — round-robin across patterns. A cache with one slot (a
//                     plausible `PRE` memo) reddens; a per-pattern one survives 1–3.
//   5. LOOKAHEAD    — literally `cutoffForSamePitch`'s access pattern: peek at
//                     k+1..k+5, THEN evaluate k. This is the production sequence,
//                     so it is the one that must never drift.
// Plus a SECOND, independently built byte-identical snapshot, which reddens a memo
// keyed on the snapshot's address rather than its contents.
//
// ── ANTI-VACUITY ────────────────────────────────────────────────────────────
// A purity test over an all-rest pattern proves nothing: every call returns the
// same empty emission, so every perturbation trivially "agrees". The fixture
// therefore asserts up front that the reference sweep contains gated steps, that
// it contains rests, and that it contains several DISTINCT emissions — and the
// seeded direction modes (`walk`, `randomNoRepeat`) are in the fixture precisely
// because they are the modes most likely to acquire a stream cursor later.
// ─────────────────────────────────────────────────────────────────────────────

#include "engine/sequencer/PatternSnapshot.h"
#include "engine/sequencer/PatternTypes.h"
#include "engine/sequencer/SequencerProcessor.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <tuple>
#include <vector>

using arpbox::engine::buildPatternSnapshot;
using arpbox::engine::DirectionMode;
using arpbox::engine::evaluateStep;
using arpbox::engine::LaneId;
using arpbox::engine::laneOf;
using arpbox::engine::PatternSetState;
using arpbox::engine::PatternSnapshot;
using arpbox::engine::PoolSnapshot;
using arpbox::engine::StepEmission;

namespace
{
/** Patterns exercised. Six is enough to make the INTERLEAVED case meaningful
    without making the sweep slow. */
constexpr int numTestPatterns = 6;

/** Step indices swept per pattern: `[firstStep, lastStep]`, deliberately spanning
    NEGATIVE indices (the polymeter floor-mod path) and several pattern loops (the
    gated-ordinal path, which is where a counter would be tempting). */
constexpr std::int64_t firstStep = -37;
constexpr std::int64_t lastStep = 260;

/** One (pattern, step) query and the answer it must always give. */
struct Sample
{
    int patternIndex = 0;
    std::int64_t stepIndex = 0;
    StepEmission emission {};
};

bool sameEmission (const StepEmission& a, const StepEmission& b) noexcept
{
    return a.gate == b.gate && a.channel == b.channel && a.note == b.note && a.velocity == b.velocity &&
           a.gateFractionOfStep == b.gateFractionOfStep;
}

/** A deterministic permutation of `[0, n)` — a 64-bit LCG walked over an index
    array. Seeded and self-contained: no `std::random_device`, no `std::shuffle`
    whose implementation is free to differ between standard libraries (a test that
    permutes differently on another toolchain still passes, but it should permute
    the SAME way when someone re-runs it to debug a failure). */
std::vector<std::size_t> seededPermutation (std::size_t n, std::uint64_t seed)
{
    std::vector<std::size_t> order (n);

    for (std::size_t i = 0; i < n; ++i)
        order[i] = i;

    std::uint64_t s = seed;

    for (std::size_t i = n; i > 1; --i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::size_t j = static_cast<std::size_t> ((s >> 33) % i);
        const std::size_t tmp = order[i - 1];
        order[i - 1] = order[j];
        order[j] = tmp;
    }

    return order;
}

/** THE FIXTURE. Six patterns that between them light up every input `evaluateStep`
    reads: unequal lane lengths and divisions (polymeter), gate rests, non-zero
    PITCH / OCT / VEL / LEN lanes, five direction modes including BOTH seeded ones,
    and a pool whose size is not the default. Written as one function so the two
    snapshot builds below are provably from identical state. */
PatternSetState purityFixtureState ()
{
    PatternSetState state {};

    state.gridStepPpq = 0.25;
    state.outputChannel = 3;

    PoolSnapshot pool {};
    pool.size = 5;
    const std::uint8_t pitches[5] = { 60, 63, 67, 70, 74 };

    for (std::size_t i = 0; i < 5; ++i)
    {
        pool.sorted[i] = pitches[i];
        // A DIFFERENT arrival order, so `asPlayed` vs `sorted` is an observable
        // difference and the `asPlayed` pattern below is not a duplicate of `up`.
        pool.asPlayed[i] = pitches[(i * 3 + 2) % 5];
    }

    state.pool = pool;

    const DirectionMode modes[numTestPatterns] = {
        DirectionMode::up,
        DirectionMode::converge,
        DirectionMode::spiral,
        DirectionMode::asPlayed,
        DirectionMode::walk,           // seeded — the likeliest future cursor site
        DirectionMode::randomNoRepeat, // seeded — likewise
    };

    // Lane lengths that are mutually coprime-ish, so the lanes phase against each
    // other across the swept range instead of repeating in lockstep.
    const int gateLengths[numTestPatterns] = { 16, 5, 7, 12, 9, 13 };
    const int pitchLengths[numTestPatterns] = { 3, 16, 5, 7, 11, 4 };
    const int gateDivisions[numTestPatterns] = { 1, 1, 2, 1, 3, 1 };

    for (int p = 0; p < numTestPatterns; ++p)
    {
        auto& pattern = state.patterns[static_cast<std::size_t> (p)];

        pattern.direction = modes[p];
        pattern.masterSeed = 0x9E3779B97F4A7C15ULL + static_cast<std::uint64_t> (p) * 0x1234567ULL;

        for (int lane = 0; lane < arpbox::engine::numLanes; ++lane)
        {
            auto& laneState = pattern.lanes[static_cast<std::size_t> (lane)];
            laneState.length = 16;
            laneState.division = 1;

            for (int step = 0; step < arpbox::engine::maxSteps; ++step)
                laneState.values[static_cast<std::size_t> (step)] =
                    arpbox::engine::laneDefault (static_cast<LaneId> (lane));
        }

        auto& gate = laneOf (pattern, LaneId::gate);
        gate.length = static_cast<std::uint8_t> (gateLengths[p]);
        gate.division = static_cast<std::uint8_t> (gateDivisions[p]);

        // RESTS MATTER: the gated-ordinal table is what keeps the pool cursor pure,
        // and it is only exercised when some steps are off.
        for (int step = 0; step < gateLengths[p]; ++step)
            gate.values[static_cast<std::size_t> (step)] = ((step + p) % 4 == 3) ? 0 : 1;

        auto& pitch = laneOf (pattern, LaneId::pitch);
        pitch.length = static_cast<std::uint8_t> (pitchLengths[p]);

        for (int step = 0; step < pitchLengths[p]; ++step)
            pitch.values[static_cast<std::size_t> (step)] = static_cast<std::int16_t> ((step % 5) - 2);

        auto& oct = laneOf (pattern, LaneId::oct);
        oct.length = 4;
        oct.values[1] = 1;
        oct.values[3] = -1;

        auto& vel = laneOf (pattern, LaneId::vel);
        vel.length = 6;
        vel.division = 2;

        for (int step = 0; step < 6; ++step)
            vel.values[static_cast<std::size_t> (step)] = static_cast<std::int16_t> (40 + step * 17);

        auto& len = laneOf (pattern, LaneId::len);
        len.length = 5;

        for (int step = 0; step < 5; ++step)
            len.values[static_cast<std::size_t> (step)] = static_cast<std::int16_t> (25 + step * 60);
    }

    return state;
}

/** The IN-ORDER reference: every (pattern, step) query, ascending, each asked
    exactly once, on a freshly built snapshot. Everything else is compared to this. */
std::vector<Sample> referenceSweep (const PatternSnapshot& snapshot)
{
    std::vector<Sample> samples;
    samples.reserve (static_cast<std::size_t> (numTestPatterns * (lastStep - firstStep + 1)));

    for (int p = 0; p < numTestPatterns; ++p)
        for (std::int64_t step = firstStep; step <= lastStep; ++step)
            samples.push_back ({ p, step, evaluateStep (snapshot, p, step) });

    return samples;
}
} // namespace

TEST_CASE ("sequencer/step-purity: the fixture actually exercises the emission core", "[unit]")
{
    // ANTI-VACUITY. Every assertion in this file is an equality against the
    // reference sweep, so a fixture that emits nothing (or emits one repeated
    // value) would make all of them pass while proving nothing at all. Pin the
    // three properties the perturbation cases depend on, up front and out loud.
    const auto snapshot = buildPatternSnapshot (purityFixtureState (), 1);
    REQUIRE (snapshot != nullptr);

    const auto reference = referenceSweep (*snapshot);
    REQUIRE (reference.size () == static_cast<std::size_t> (numTestPatterns * (lastStep - firstStep + 1)));

    int gated = 0;
    int rests = 0;
    std::set<std::tuple<int, int, int, double>> distinct;

    for (const auto& sample : reference)
    {
        if (sample.emission.gate)
        {
            ++gated;
            distinct.insert ({ sample.emission.channel,
                               sample.emission.note,
                               sample.emission.velocity,
                               sample.emission.gateFractionOfStep });
        }
        else
        {
            ++rests;
        }
    }

    INFO ("gated " << gated << ", rests " << rests << ", distinct emissions " << distinct.size ());

    REQUIRE (gated > 500);           // the core is genuinely producing notes
    REQUIRE (rests > 100);           // …and genuinely producing rests (the ordinal path)
    REQUIRE (distinct.size () > 20); // …and the answers vary, so equality is not trivial
}

TEST_CASE ("sequencer/step-purity: evaluateStep is order- and history-independent (issue #53)", "[unit][determinism]")
{
    const auto snapshot = buildPatternSnapshot (purityFixtureState (), 1);
    REQUIRE (snapshot != nullptr);

    const auto reference = referenceSweep (*snapshot);
    const auto n = reference.size ();

    SECTION ("repeated calls on the same index never drift")
    {
        // Defeats: any per-call state advance — an RNG stream stepped once per
        // evaluation being the canonical Phase 7.1 mistake.
        for (int repeat = 0; repeat < 4; ++repeat)
            for (const auto& sample : reference)
            {
                const auto again = evaluateStep (*snapshot, sample.patternIndex, sample.stepIndex);
                INFO ("repeat " << repeat << " pattern " << sample.patternIndex << " step " << sample.stepIndex);
                REQUIRE (sameEmission (again, sample.emission));
            }
    }

    SECTION ("a descending sweep matches the ascending one")
    {
        // Defeats: state keyed on call count or on "the previous index I was asked
        // about" — e.g. a `PRE` condition result carried forward instead of
        // re-derived, which issue #53 names explicitly.
        for (std::size_t i = n; i > 0; --i)
        {
            const auto& sample = reference[i - 1];
            const auto again = evaluateStep (*snapshot, sample.patternIndex, sample.stepIndex);
            INFO ("descending pattern " << sample.patternIndex << " step " << sample.stepIndex);
            REQUIRE (sameEmission (again, sample.emission));
        }
    }

    SECTION ("a seeded shuffle of the whole sweep matches")
    {
        // Defeats: anything that survives reversal by symmetry.
        for (const auto index : seededPermutation (n, 0xC0FFEEULL))
        {
            const auto& sample = reference[index];
            const auto again = evaluateStep (*snapshot, sample.patternIndex, sample.stepIndex);
            INFO ("shuffled pattern " << sample.patternIndex << " step " << sample.stepIndex);
            REQUIRE (sameEmission (again, sample.emission));
        }
    }

    SECTION ("interleaving the patterns round-robin matches")
    {
        // Defeats: a single-slot cache or accumulator that a per-pattern sweep
        // would never disturb. Also the shape a real pattern switch produces.
        const auto perPattern = static_cast<std::size_t> (lastStep - firstStep + 1);

        for (std::size_t s = 0; s < perPattern; ++s)
            for (int p = 0; p < numTestPatterns; ++p)
            {
                const auto& sample = reference[static_cast<std::size_t> (p) * perPattern + s];
                const auto again = evaluateStep (*snapshot, sample.patternIndex, sample.stepIndex);
                INFO ("interleaved pattern " << sample.patternIndex << " step " << sample.stepIndex);
                REQUIRE (sameEmission (again, sample.emission));
            }
    }

    SECTION ("the retrigger lookahead's own access pattern matches")
    {
        // THE PRODUCTION SEQUENCE, reproduced exactly: `cutoffForSamePitch` peeks at
        // k+1 … k+maxRetriggerLookaheadSteps and the walk then emits k. Both the
        // peeks and the emission must agree with the in-order reference — the peek
        // must not disturb the emission, and the emission must not disturb the next
        // peek. If only one case in this file may exist, it is this one.
        constexpr std::int64_t lookahead = 5; // SequencerProcessor::maxRetriggerLookaheadSteps

        for (int p = 0; p < numTestPatterns; ++p)
        {
            const auto base = static_cast<std::size_t> (p) * static_cast<std::size_t> (lastStep - firstStep + 1);

            for (std::int64_t step = firstStep; step + lookahead <= lastStep; ++step)
            {
                for (std::int64_t ahead = 1; ahead <= lookahead; ++ahead)
                {
                    const auto& peeked = reference[base + static_cast<std::size_t> (step + ahead - firstStep)];
                    const auto again = evaluateStep (*snapshot, p, step + ahead);
                    INFO ("peek pattern " << p << " step " << (step + ahead));
                    REQUIRE (sameEmission (again, peeked.emission));
                }

                const auto& emitted = reference[base + static_cast<std::size_t> (step - firstStep)];
                const auto again = evaluateStep (*snapshot, p, step);
                INFO ("emit-after-peek pattern " << p << " step " << step);
                REQUIRE (sameEmission (again, emitted.emission));
            }
        }
    }

    SECTION ("a second, independently built snapshot of the same state answers identically")
    {
        // Defeats: a memo keyed on the snapshot's ADDRESS rather than its contents,
        // and any state that survives between builds. The build counter differs on
        // purpose — it is diagnostics only and must never reach the emission.
        const auto rebuilt = buildPatternSnapshot (purityFixtureState (), 99);
        REQUIRE (rebuilt != nullptr);
        REQUIRE (rebuilt.get () != snapshot.get ());

        for (const auto& sample : reference)
        {
            const auto again = evaluateStep (*rebuilt, sample.patternIndex, sample.stepIndex);
            INFO ("rebuilt pattern " << sample.patternIndex << " step " << sample.stepIndex);
            REQUIRE (sameEmission (again, sample.emission));
        }
    }
}
