// ─────────────────────────────────────────────────────────────────────────────
// step_probability — the per-step PROB roll (ARCHITECTURE §5.1 L2, §5.2, §12.1;
// engine/sequencer/StepLogic.cpp `probabilityPasses`, engine/generative/Rng.h).
//
// INSTRUCTIONS Phase 7 success criterion: "probability output is seed-exact and
// loop-stable". This file is the seed-exact half plus the determinism reading of
// loop-stable (identical across re-renders, across every swept block size, and
// across a transport locate into the middle of the render — LITERAL per-loop
// repetition is LOOP LOCK, which is Phase 12).
//
// ── THE REFERENCE IMPLEMENTATION IS WRITTEN OUT LONGHAND, ON PURPOSE ────────
// `expectedRoll` below re-types splitmix64's body, the domain salt, the nesting
// of `stepHash`, both short-circuits and the `% 100` comparison. It never calls
// `rng::` or `probabilityPasses`. A test that computes its expectation by calling
// the function under test asserts only that the function is deterministic — which
// is a real property, and one `pattern_determinism.cpp` already covers. What is
// missing without a second implementation is "and the answer is THE RIGHT ONE":
// a mistyped multiplier, a lost XOR, a `<=` for `<`, or a swapped domain salt all
// leave a function that is perfectly self-consistent and audibly different.
//
// ── THE "PROB 100 CONSUMES NO RNG" PROBE IS THE LOAD-BEARING CASE ──────────
// `probabilityPasses` returns `true` for `p >= 100` BEFORE hashing, and
// `laneDefault (LaneId::prob)` is 100 — so every pattern whose PROB lane has
// never been touched draws no randomness at all. That is what makes the six
// Phase-6 goldens provably unmoved by this phase: not "the hash happened to
// agree", but "the hash was never called", and therefore `rngVersion: 0` on those
// files is literally true rather than approximately true.
//
// A comment cannot check that. The BLACK-BOX PROBE can: render the same PROB-100
// pattern under two DIFFERENT `masterSeed`s and require byte-identical MIDI. If
// anything on the audible path ever consults the seed at PROB 100, the two renders
// diverge. Its complement (PROB 50 under the same two seeds MUST diverge) is
// what stops the probe passing for the boring reason that the seed reaches
// nothing at all.
// ─────────────────────────────────────────────────────────────────────────────

#include "support/MidiRenderHarness.h"
#include "support/SequencerRenderRig.h"

#include "engine/graph/EngineCommand.h"
#include "engine/sequencer/PatternDocument.h"
#include "engine/sequencer/PatternSnapshot.h"
#include "engine/sequencer/PatternTypes.h"
#include "engine/sequencer/StepLogic.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using arpbox::engine::buildPatternSnapshot;
using arpbox::engine::EngineCommandType;
using arpbox::engine::LaneId;
using arpbox::engine::laneDefault;
using arpbox::engine::laneOf;
using arpbox::engine::LaneState;
using arpbox::engine::maxSteps;
using arpbox::engine::numLanes;
using arpbox::engine::PatternData;
using arpbox::engine::PatternDocument;
using arpbox::engine::PatternSetState;
using arpbox::engine::PatternSnapshot;
using arpbox::engine::probabilityPasses;
using arpbox::testing::engineCommand;
using arpbox::testing::MidiRenderConfig;
using arpbox::testing::MidiRenderResult;
using arpbox::testing::renderSequencer;
using arpbox::testing::ScheduledCommand;
using arpbox::testing::SequencerRig;

namespace
{
// ─────────────────────────────────────────────────────────────────────────────
// A. The independent reference implementation
// ─────────────────────────────────────────────────────────────────────────────

/** splitmix64, re-typed from the canonical constants (Steele/Lea/Flood) rather
    than included from `engine/generative/Rng.h`. THE POINT IS THE DUPLICATION —
    see the header note. Its known-answer vector is checked below before it is
    trusted as a reference, so a typo HERE fails loudly instead of silently
    excusing a typo there. */
std::uint64_t referenceSplitmix64 (std::uint64_t x) noexcept
{
    x += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/** `RngDomain::stepProbability`, as a literal. 0x5052 is ASCII 'P','R'. The salt
    is XORed into the seed, so a changed value silently rewrites every sub-100 %
    PROB step ever produced — writing it out here is what makes a change to the
    registry visible as a test diff. */
constexpr std::uint64_t referenceProbabilitySalt = 0x5052ULL;

/** `rng::stepHash`, re-derived: `splitmix64 (splitmix64 (seed ^ salt) ^ index)`.
    Two nested calls, not one — the inner is §5.2's effective stream seed with the
    Phase-12 operator/loop-lock terms elided (they XOR with 0 today), the outer is
    §12.3's per-index-hash idiom. Collapsing them would make Phase 12's extension a
    determinism break, so the nesting is part of the contract and is duplicated
    here deliberately. */
std::uint64_t referenceStepHash (std::uint64_t masterSeed, std::int64_t stepIndex) noexcept
{
    return referenceSplitmix64 (referenceSplitmix64 (masterSeed ^ referenceProbabilitySalt) ^
                                static_cast<std::uint64_t> (stepIndex));
}

/** The whole of `probabilityPasses`' contract, written from §5.1 L2 / §12.1:
    `p >= 100` passes WITHOUT hashing, `p <= 0` fails without hashing, otherwise
    `hash % 100 < p`. */
bool expectedRoll (std::uint64_t masterSeed, int percent, std::int64_t stepIndex) noexcept
{
    if (percent >= 100)
        return true;

    if (percent <= 0)
        return false;

    return referenceStepHash (masterSeed, stepIndex) % 100ULL < static_cast<std::uint64_t> (percent);
}

// ─────────────────────────────────────────────────────────────────────────────
// B. Snapshot fixtures (direct `probabilityPasses` calls)
// ─────────────────────────────────────────────────────────────────────────────

/** Pattern 0 with every lane at its §12.1 default, GATE all-on across a 16-step
    cycle, a PROB lane of LENGTH 1 holding `percent`, and the given master seed. */
std::unique_ptr<const PatternSnapshot> probSnapshot (std::uint64_t masterSeed, int percent)
{
    PatternSetState state {};
    auto& pattern = state.patterns[0];
    pattern.masterSeed = masterSeed;

    for (int lane = 0; lane < numLanes; ++lane)
    {
        LaneState& target = pattern.lanes[static_cast<std::size_t> (lane)];
        target.length = 16;
        target.division = 1;

        for (int step = 0; step < maxSteps; ++step)
            target.values[static_cast<std::size_t> (step)] = laneDefault (static_cast<LaneId> (lane));
    }

    LaneState& gate = laneOf (pattern, LaneId::gate);
    for (int step = 0; step < maxSteps; ++step)
        gate.values[static_cast<std::size_t> (step)] = 1;

    LaneState& prob = laneOf (pattern, LaneId::prob);
    prob.length = 1;
    prob.values[0] = static_cast<std::int16_t> (percent);

    return buildPatternSnapshot (state, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// C. Render fixtures (the black-box probes)
// ─────────────────────────────────────────────────────────────────────────────

// 120 BPM @ 48 kHz on the default 1/16 grid ⇒ one step is exactly 6000 samples, so
// every literal below divides cleanly and a reader can check a position by hand.
constexpr double probeSampleRate = 48000.0;
constexpr double probeBpm = 120.0;
constexpr std::int64_t probeSamplesPerStep = 6000;
constexpr std::int64_t probeSpanSamples = 6000 * 16 * 8; // 8 loops of the 16-step cycle

/** The ten block sizes the suite sweeps everywhere else (pattern_switch.cpp,
    sequencer_offdeterminism.cpp). Kept identical so a probability finding can be
    compared against those files without an alignment argument. */
constexpr int sweptBlockSizes[] = { 32, 64, 96, 128, 256, 480, 512, 1024, 2048, 4096 };

/** Sets pattern 0's PROB lane to `percent` on every step and its master seed to
    `masterSeed`. Everything else is the default document (GATE all-on across 16
    steps, stub pool, `up` traversal), which is already the PROB-100 pattern the
    "no RNG" probe needs. */
void configure (PatternDocument& document, std::uint64_t masterSeed, int percent)
{
    document.beginTransaction ();
    document.setMasterSeed (0, masterSeed);

    for (int step = 0; step < maxSteps; ++step)
        document.setLaneValue (0, LaneId::prob, step, percent);

    document.endTransaction ();
}

MidiRenderConfig probeConfig (int blockSize)
{
    auto config = MidiRenderConfig::samples (probeSpanSamples, probeSampleRate, blockSize);
    config.numChannels = 1;
    config.eventReserve = 16384;
    return config;
}

MidiRenderResult renderProbe (std::uint64_t masterSeed, int percent, int blockSize)
{
    SequencerRig rig { probeSampleRate, blockSize };
    configure (rig.patternDocument, masterSeed, percent);

    const std::vector<ScheduledCommand> schedule {
        ScheduledCommand { 0, engineCommand (EngineCommandType::setTempoBpm, probeBpm) },
        ScheduledCommand { 0, engineCommand (EngineCommandType::transportPlay) }
    };

    return renderSequencer (rig, probeConfig (blockSize), schedule);
}

/** Note-on (absolute sample, pitch, velocity) triples — the "what was played"
    projection, used where note-offs would confuse a comparison across renders that
    begin at different transport positions. */
struct NoteOn
{
    std::int64_t sample;
    int pitch;
    int velocity;

    bool operator== (const NoteOn& other) const noexcept
    {
        return sample == other.sample && pitch == other.pitch && velocity == other.velocity;
    }
};

/** A canonical text form of every event STRICTLY BEFORE `endSample`.

    ── WHY NOT `toByteStream()` FOR THE BLOCK-SIZE SWEEP ───────────────────────
    `MidiRenderConfig::samples` rounds the span UP to whole blocks, so a render at
    4096 covers 770048 samples where one at 32 covers exactly 768000 — and the
    longer render legitimately contains the note-on at 768000 that the shorter one
    never reached. That is an artefact of the harness's span rounding, not a
    buffer-size dependence, and `toByteStream()` (which carries the block count and
    total span in its header) cannot express "compare the musical time both renders
    actually covered". The goldens solve this by aligning every span to
    `goldenAlignmentUnit`; here the span is chosen for readable step arithmetic
    (6000 samples/step) instead, so the comparison is trimmed rather than the span
    aligned. Everything inside the common span is still compared BYTE-FOR-BYTE. */
std::string trimmedStream (const MidiRenderResult& render, std::int64_t endSample)
{
    std::string stream;

    for (const auto& event : render.events)
    {
        if (event.absoluteSample >= endSample)
            continue;

        stream += std::to_string (event.absoluteSample);

        for (int i = 0; i < event.numBytes (); ++i)
            stream += ':' + std::to_string (static_cast<int> (event.bytes ()[i]));

        stream += '\n';
    }

    return stream;
}

std::vector<NoteOn> noteOnsOf (const MidiRenderResult& render, std::int64_t shift = 0)
{
    std::vector<NoteOn> notes;

    for (const auto& event : render.events)
        if (event.message.isNoteOn ())
            notes.push_back (
                { event.absoluteSample + shift, event.message.getNoteNumber (), event.message.getVelocity () });

    return notes;
}

constexpr std::uint64_t seedA = 0x0123456789ABCDEFULL;
constexpr std::uint64_t seedB = 0xFEDCBA9876543210ULL;
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1. Seed-exactness against the independent reference
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/step-probability: the roll matches an independently written reference", "[unit][determinism]")
{
    // The reference is checked against splitmix64's published known-answer vector
    // BEFORE it is used as an oracle. Without this, a typo in the reference and a
    // typo in the engine could cancel — and the whole point of a second
    // implementation is that they cannot.
    REQUIRE (referenceSplitmix64 (0) == 0xE220A8397B1DCDAFULL);

    const int percents[] = { 0, 1, 50, 99, 100 };
    const std::uint64_t seeds[] = { 0ULL, seedA, seedB };

    // Index range chosen to straddle zero: NEGATIVE step indices are legal (the
    // retrigger lookahead and the locate paths sweep below 0) and the conversion to
    // uint64 is modulo 2^64, so index -1 hashes as 0xFFFF'FFFF'FFFF'FFFF. A
    // reference that agreed only on the non-negative half would be worthless
    // exactly where the lookahead lives.
    constexpr std::int64_t firstIndex = -2000;
    constexpr std::int64_t lastIndex = 9000;

    int checks = 0;
    int mismatches = 0;
    std::string firstMismatch;

    for (const std::uint64_t seed : seeds)
        for (const int percent : percents)
        {
            const auto snapshot = probSnapshot (seed, percent);
            REQUIRE (snapshot != nullptr);
            const PatternData& data = snapshot->patterns[0];
            REQUIRE (laneOf (data, LaneId::prob).values[0] == percent);

            for (std::int64_t index = firstIndex; index <= lastIndex; ++index)
            {
                const bool actual = probabilityPasses (data, index);
                const bool expected = expectedRoll (seed, percent, index);
                ++checks;

                if (actual != expected && firstMismatch.empty ())
                    firstMismatch = "seed " + std::to_string (seed) + " p " + std::to_string (percent) + " index " +
                                    std::to_string (index) + " expected " + (expected ? "true" : "false") + " got " +
                                    (actual ? "true" : "false");

                if (actual != expected)
                    ++mismatches;
            }
        }

    INFO ("checks " << checks << ", first mismatch: " << firstMismatch);
    REQUIRE (checks == 3 * 5 * static_cast<int> (lastIndex - firstIndex + 1));
    REQUIRE (checks > 10000);
    REQUIRE (mismatches == 0);
}

TEST_CASE ("sequencer/step-probability: the fixed points and the middle are non-vacuous", "[unit][determinism]")
{
    // ANTI-VACUITY for the sweep above. If `probabilityPasses` returned a constant,
    // the reference would have to return the same constant for the sweep to pass —
    // which it would not, but only because of arithmetic nobody reads. State the
    // shape directly: 0 never fires, 100 always fires, and 50 fires roughly half
    // the time with BOTH outcomes actually present.
    constexpr std::int64_t sampleCount = 10000;

    const auto never = probSnapshot (seedA, 0);
    const auto always = probSnapshot (seedA, 100);
    const auto half = probSnapshot (seedA, 50);
    const auto rare = probSnapshot (seedA, 1);
    const auto common = probSnapshot (seedA, 99);

    int halfPassed = 0;
    int rarePassed = 0;
    int commonPassed = 0;

    for (std::int64_t index = 0; index < sampleCount; ++index)
    {
        REQUIRE (! probabilityPasses (never->patterns[0], index));
        REQUIRE (probabilityPasses (always->patterns[0], index));

        halfPassed += probabilityPasses (half->patterns[0], index) ? 1 : 0;
        rarePassed += probabilityPasses (rare->patterns[0], index) ? 1 : 0;
        commonPassed += probabilityPasses (common->patterns[0], index) ? 1 : 0;
    }

    INFO ("p50 " << halfPassed << ", p1 " << rarePassed << ", p99 " << commonPassed << " of " << sampleCount);

    // Loose bounds — this is a NON-VACUITY floor, not a distributional contract.
    // (The `% 100` bias is ~1e-18 and deliberately unfixed; see the note in
    // StepLogic.cpp on why rejection sampling is forbidden here.)
    REQUIRE (halfPassed > 4500);
    REQUIRE (halfPassed < 5500);
    REQUIRE (rarePassed > 0);
    REQUIRE (rarePassed < 300);
    REQUIRE (commonPassed > 9700);
    REQUIRE (commonPassed < sampleCount);
}

TEST_CASE ("sequencer/step-probability: a below-range PROB fails closed rather than always firing",
           "[unit][determinism]")
{
    // THE SIGN GUARD, and the only place it is reachable. `laneValueAt` returns
    // `std::int16_t` and the comparison is against a `std::uint64_t`, so a negative
    // percentage that reached the RT path would convert to ~1.8e19 and every step
    // would fire unconditionally. `clampLaneValue` keeps PROB in [0, 100] on the
    // normal path, so this is poked into a copy of a built snapshot — the same
    // shape a corrupt project blob or a future operator write would take.
    //
    // What holds the line is the `percent <= 0` EARLY EXIT (and the explicit
    // `static_cast<std::uint64_t>` that documents why the comparison is safe once
    // the guard has run). Delete the guard and this case reddens; nothing else in
    // the suite does.
    const auto snapshot = probSnapshot (seedA, 50);
    REQUIRE (snapshot != nullptr);

    // The clamp holds on the normal path.
    {
        const auto clamped = probSnapshot (seedA, -30);
        REQUIRE (laneOf (clamped->patterns[0], LaneId::prob).values[0] == 0);
    }

    for (const int negative : { -1, -30, -100, -32768 })
    {
        PatternData poked = snapshot->patterns[0];
        poked.lanes[static_cast<std::size_t> (LaneId::prob)].values[0] = static_cast<std::int16_t> (negative);

        for (std::int64_t index = -50; index <= 50; ++index)
        {
            INFO ("PROB " << negative << " at index " << index);
            REQUIRE (! probabilityPasses (poked, index));
        }
    }

    // And symmetrically above the range: anything >= 100 fires without hashing.
    for (const int over : { 100, 101, 400, 32767 })
    {
        PatternData poked = snapshot->patterns[0];
        poked.lanes[static_cast<std::size_t> (LaneId::prob)].values[0] = static_cast<std::int16_t> (over);

        for (std::int64_t index = -50; index <= 50; ++index)
        {
            INFO ("PROB " << over << " at index " << index);
            REQUIRE (probabilityPasses (poked, index));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. THE BLACK-BOX PROBE: PROB 100 consumes no RNG
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/step-probability: a PROB-100 pattern renders identically under two master seeds",
           "[determinism]")
{
    // WHAT THIS KEEPS HONEST. The six Phase-6 goldens were baked before any RNG
    // existed and carry `rngVersion: 0`. That header line is a CLAIM — "the audible
    // path of this file consumed no randomness" — and the only thing making it true
    // is `probabilityPasses`' `p >= 100` short-circuit, which fires because
    // `laneDefault (LaneId::prob)` is 100.
    //
    // The short-circuit reads like an optimisation, and the natural "simplification"
    // is to delete it: `hash % 100 < 100` is always true, so nothing audible changes
    // TODAY. Then Phase 12 adds LOOP LOCK or a new `RngDomain`, the stream shifts,
    // and every default-PROB golden moves — with the diff pointing at Phase 12
    // rather than at the deletion. This probe is the tripwire on that path: two
    // different seeds, one PROB-100 pattern, byte-identical MIDI required.
    const auto renderA = renderProbe (seedA, 100, 128);
    const auto renderB = renderProbe (seedB, 100, 128);

    INFO (renderA.describeDifference (renderB));
    REQUIRE (renderA.events.size () == renderB.events.size ());
    REQUIRE (renderA.toByteStream () == renderB.toByteStream ());

    // ANTI-VACUITY: the render has to contain notes, or "identical" is trivial.
    // 8 loops x 16 gated steps = 128 note-ons on the default pattern.
    const auto notes = noteOnsOf (renderA);
    INFO ("note-ons " << notes.size ());
    REQUIRE (notes.size () == 128);
    REQUIRE (notes.front ().sample == 0);
    REQUIRE (notes[1].sample == probeSamplesPerStep);

    // A third seed, so the claim is not "these two seeds happen to collide".
    const auto renderC = renderProbe (0xA5A5A5A5A5A5A5A5ULL, 100, 128);
    REQUIRE (renderC.toByteStream () == renderA.toByteStream ());
}

TEST_CASE ("determinism/step-probability: at PROB 50 the same two seeds DIVERGE", "[determinism]")
{
    // THE COMPLEMENT, and it is not optional. Without it, the probe above passes
    // for the boring reason that `masterSeed` reaches nothing on the audible path
    // at all — which is exactly what would happen if `PatternData::masterSeed` were
    // dropped, or if the PROB check were accidentally short-circuited for every
    // value. This case is what says the seed IS wired through and the probe's green
    // means what it claims.
    const auto renderA = renderProbe (seedA, 50, 128);
    const auto renderB = renderProbe (seedB, 50, 128);

    REQUIRE (renderA.toByteStream () != renderB.toByteStream ());

    const auto notesA = noteOnsOf (renderA);
    const auto notesB = noteOnsOf (renderB);

    INFO ("p50 note-ons: seedA " << notesA.size () << ", seedB " << notesB.size () << " (PROB-100 would be 128)");

    // Both renders are genuinely thinned, and neither collapsed to silence.
    REQUIRE (notesA.size () > 30);
    REQUIRE (notesA.size () < 100);
    REQUIRE (notesB.size () > 30);
    REQUIRE (notesB.size () < 100);
    REQUIRE (notesA != notesB);

    // …and each is a strict subset of the PROB-100 rhythm in POSITION: PROB thins
    // the rhythm, it must never move a note or invent one. (Risk register item 6:
    // folding PROB into the gated ordinal would shift the PITCH sequence too.)
    const auto full = noteOnsOf (renderProbe (seedA, 100, 128));
    std::vector<std::int64_t> fullPositions;
    for (const auto& note : full)
        fullPositions.push_back (note.sample);

    for (const auto& note : notesA)
    {
        INFO ("p50 note at " << note.sample << " must sit on a PROB-100 position");
        REQUIRE (std::find (fullPositions.begin (), fullPositions.end (), note.sample) != fullPositions.end ());
    }
}

TEST_CASE ("determinism/step-probability: PROB thins the rhythm without moving the PITCH sequence", "[determinism]")
{
    // `PatternSnapshot::gatedOrdinal`'s "PHASE 7, READ THIS BEFORE YOU 'FIX' IT",
    // as an assertion. A step suppressed by PROB still consumes its gated ordinal,
    // so the pool traversal is untouched: the note that DOES sound at a given step
    // is the same note it would have been at PROB 100. Folding suppression into the
    // ordinal — the tempting "fix" — would make the arpeggio's pitch sequence a
    // function of RNG draw count, i.e. of block carving.
    const auto full = noteOnsOf (renderProbe (seedA, 100, 128));
    const auto thinned = noteOnsOf (renderProbe (seedA, 50, 128));

    REQUIRE (thinned.size () < full.size ());
    REQUIRE (! thinned.empty ());

    for (const auto& note : thinned)
    {
        const auto match =
            std::find_if (full.begin (), full.end (), [&] (const NoteOn& f) { return f.sample == note.sample; });

        INFO ("thinned note at " << note.sample << " pitch " << note.pitch);
        REQUIRE (match != full.end ());
        // SAME sample, SAME pitch, SAME velocity — only the presence differs.
        REQUIRE (match->pitch == note.pitch);
        REQUIRE (match->velocity == note.velocity);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Loop-stability, in the determinism sense
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/step-probability: a PROB-50 render is identical at all ten block sizes", "[determinism]")
{
    // Buffer-size independence for the stochastic path. This is the assertion a
    // RUNNING RNG CURSOR fails: a stream's output depends on how many times it has
    // been pulled, and the pull count depends on how many steps a block contains
    // and on how many future steps the retrigger lookahead happened to peek at —
    // both properties of the block carving, not of the music. A per-index hash has
    // no such dependence, which is why §5.2 requires one.
    const auto reference = renderProbe (seedA, 50, sweptBlockSizes[0]);
    const auto referenceStream = trimmedStream (reference, probeSpanSamples);

    REQUIRE (static_cast<int> (std::size (sweptBlockSizes)) == 10);
    REQUIRE (reference.events.size () > 60);
    REQUIRE (! referenceStream.empty ());

    int sizesChecked = 0;
    int sizesMatched = 0;
    std::string firstMismatch;

    for (const int blockSize : sweptBlockSizes)
    {
        const auto render = renderProbe (seedA, 50, blockSize);
        ++sizesChecked;

        if (trimmedStream (render, probeSpanSamples) == referenceStream)
            ++sizesMatched;
        else if (firstMismatch.empty ())
            firstMismatch = "block size " + std::to_string (blockSize) + ": " +
                            reference.describeDifference (render).toStdString ();
    }

    INFO (firstMismatch);
    REQUIRE (sizesChecked == 10);
    REQUIRE (sizesMatched == 10);
}

TEST_CASE ("determinism/step-probability: two fresh renders of the same seed are byte-identical", "[determinism]")
{
    // The REPRODUCIBLE half (pattern_determinism.cpp's shape, applied to the roll):
    // a different configuration is rendered BETWEEN the two comparands, so any
    // process-wide state the roll might be carrying has been disturbed before the
    // second render observes it.
    const auto first = renderProbe (seedA, 50, 128);
    const auto decoy = renderProbe (0xDEADBEEFULL, 37, 256);
    REQUIRE (! decoy.events.empty ());
    const auto second = renderProbe (seedA, 50, 128);

    INFO (first.describeDifference (second));
    REQUIRE (first.toByteStream () == second.toByteStream ());
}

TEST_CASE ("determinism/step-probability: locating into the middle reproduces the same rolls", "[determinism]")
{
    // RISK REGISTER ITEM 5, as a test. The roll is keyed on the GLOBAL STEP INDEX,
    // never on a transport bar counter or on "how many steps this engine instance
    // has emitted". The difference is invisible during a straight play-from-zero
    // render and decisive the moment the user drags the playhead: a counter's value
    // depends on where playback STARTED, so the same bar would roll differently
    // after a locate — and §9's offline drag-out, which renders from wherever the
    // pattern happens to begin, would not match what was heard.
    //
    // 120 BPM / 1/16 grid ⇒ 6000 samples per step, so PPQ 8.0 is step 32 and sample
    // 192000 exactly. Compared on NOTE-ONS: the continuous render carries note-offs
    // for notes that began before the locate point, which the located render
    // correctly does not.
    constexpr std::int64_t locateStep = 32;
    constexpr double locatePpq = 8.0;
    constexpr std::int64_t locateSample = locateStep * probeSamplesPerStep;
    REQUIRE (locateSample == 192000);

    const auto continuous = renderProbe (seedA, 50, 128);

    SequencerRig rig { probeSampleRate, 128 };
    configure (rig.patternDocument, seedA, 50);

    const std::vector<ScheduledCommand> schedule {
        ScheduledCommand { 0, engineCommand (EngineCommandType::setTempoBpm, probeBpm) },
        ScheduledCommand { 0, engineCommand (EngineCommandType::transportLocate, locatePpq) },
        ScheduledCommand { 0, engineCommand (EngineCommandType::transportPlay) }
    };

    auto config = probeConfig (128);
    config.numBlocks = MidiRenderConfig::numBlocksForSamples (probeSpanSamples - locateSample, 128);
    const auto located = renderSequencer (rig, config, schedule);

    // Shift the located render onto the continuous timeline and compare note-ons.
    const auto locatedNotes = noteOnsOf (located, locateSample);

    std::vector<NoteOn> continuousTail;
    for (const auto& note : noteOnsOf (continuous))
        if (note.sample >= locateSample)
            continuousTail.push_back (note);

    INFO ("continuous tail " << continuousTail.size () << " notes, located " << locatedNotes.size ());
    REQUIRE (! locatedNotes.empty ());
    REQUIRE (locatedNotes.size () > 20); // non-vacuity: the span really is thinned, not empty
    REQUIRE (locatedNotes == continuousTail);
}
