// ─────────────────────────────────────────────────────────────────────────────
// boundary_agreement — issue #54. THE TWO DERIVATIONS OF ONE STEP BOUNDARY MUST
// AGREE, and this file is what makes them fail together or not at all.
//
// ── THE TWO CALL SITES ──────────────────────────────────────────────────────
// `SequencerProcessor` converts a global step index into an absolute sample in
// TWO places, with the same snap-then-floor idiom written out twice:
//
//   A. `SequencerProcessor::stepBoundarySample` (the issue #46 fix)
//          blockStartSample + floor (blockOffsetForPpq (index * stepPpq)
//                                    + sampleOffsetSnapSamples)
//      Deliberately UNCLAMPED, so it can address a boundary belonging to a LATER
//      block. Two callers: `cutoffForSamePitch` (the retrigger lookahead) and the
//      pattern-switch PRE-FLUSH equality test at the end of `processBlock`.
//
//   B. the step-boundary WALK inside `processBlock`
//          offset = clamp (floor (blockOffsetForPpq (index * stepPpq)
//                                 + sampleOffsetSnapSamples), 0, numSamples - 1)
//          onSample = blockStartSample + offset
//      The same arithmetic plus the in-block clamp, which is the ONLY licensed
//      difference (it is the issue #37 snap-boundary window; see below).
//
// They agree today — the Phase 6 review gate traced the ulp-either-side edge
// cases by hand. NOTHING ENFORCED THAT THEY CONTINUE TO, which is issue #54. A
// per-pattern grid (the change #54 names as most likely) that alters one side's
// arithmetic and not the other reintroduces the #36 / #46 / #48 shape — an event
// whose position depends on how the timeline was carved — one level up, where no
// existing test looks.
//
// ── WHY THIS IS NOT "RE-DERIVE THE FORMULA AND COMPARE" ─────────────────────
// A test that recomputes the boundary in test code and checks each site against
// it would be a THIRD derivation. It would catch a single-site perturbation, but
// it pins each site to the test's own arithmetic rather than to the other site —
// so it goes red for a legitimate joint change and, worse, invites whoever makes
// that change to "update the test's formula" one site at a time. #54 asks for
// AGREEMENT, so agreement is what is asserted: nothing here re-derives the
// snap-then-floor idiom.
//
// ── THE INSTRUMENT: put the two derivations of ONE boundary side by side ────
// Configure a single-note pool (so every gated step retriggers the same pitch)
// with LEN at 150 % (so a note is STILL SOUNDING when its own pitch comes round
// again). Then, for every step boundary b:
//
//   • the walk (B) places boundary b's NOTE-ON at `stepBoundarySample_B (b)`;
//   • `cutoffForSamePitch` (A) placed the PREVIOUS note's NOTE-OFF at
//     `stepBoundarySample_A (b) - 1`  — §5.5's one-sample retrigger gap.
//
// Both quantities describe THE SAME boundary b, they are emitted by different
// code, and they land in the stream as adjacent absolute samples. So the whole
// property collapses to one integer relation over the rendered MIDI:
//
//     noteOff[n] == noteOn[n + 1] - 1     for every boundary in the sweep
//
// Perturb A alone and the note-offs move. Perturb B alone and the note-ons move.
// Either way the relation breaks — which is the "must fail if EITHER derivation
// is perturbed independently" clause, and it is verified fails-without twice
// (once per side; see the report accompanying the commit that added this file).
//
// WHY THE RETRIGGER LOOKAHEAD IS THE CHOSEN WINDOW ONTO A. `stepBoundarySample`
// has two callers, and A's OTHER caller — the pattern-switch pre-flush equality
// test — is already pinned, with its own reachability probe, by case D of
// tests/sequencer_offdeterminism.cpp ("the switch flush landing exactly ON a
// block head"). What was missing, and what #54 names, is a check on the shared
// DERIVATION rather than on one consumer of it; that is this file. Between the
// two, both call sites of A are covered.
//
// ── THE ONE LICENSED DIFFERENCE, AND WHY THE SWEEP STAYS OUT OF IT ──────────
// B's clamp fires when a boundary's true position sits inside the issue #37
// snap-boundary window: within `stepIndexSnapSteps` (1e-6 steps, i.e.
// 1e-6 x samplesPerStep SAMPLES) below a block edge, AND more than
// `sampleOffsetSnapSamples` (1e-4 samples) below it. The snapped ceiling then
// claims the boundary for the LATER block, B's raw offset comes back negative,
// and the clamp emits at offset 0 — one sample later than A reports. That is the
// documented exception (SequencerProcessor.h, "THE SNAP-BOUNDARY WINDOW"), not a
// disagreement this test may relax around.
//
// So the window is computed here from first principles for every swept boundary
// and REQUIREd to be EMPTY. If a future sweep edit makes that count non-zero, the
// relation above must be restated against #37 — not the count relaxed.
//
// ── ANTI-VACUITY (the Phase 6 signature failure: observing without failing) ──
//   • The number of boundaries actually compared is asserted as a LITERAL, so a
//     sweep that silently rendered nothing — or one boundary — cannot pass.
//   • Every render is required to be long enough to supply its full quota; short
//     renders are counted and REQUIREd to be zero rather than silently skipped.
//   • The interesting families are counted SEPARATELY and each REQUIREd > 0:
//     boundaries exactly on a block edge, boundaries a sub-sample above one, and
//     boundaries a sub-sample below one. An aggregate count would let all the
//     agreement come from comfortable mid-block boundaries.
//   • The swept step-index range is asserted to span negative and large indices,
//     matching what tests/step_purity.cpp exercises `evaluateStep` over.
//   • No Catch2 macro runs inside a sweep loop: everything aggregates into an
//     `Observation` and is asserted once, after.
//
// ── "AN ULP EITHER SIDE OF A BLOCK EDGE" ────────────────────────────────────
// An ulp-level offset cannot be dialled in directly — you get whatever the float
// arithmetic gives. Two families cover it between them:
//
//   • THE ENGINEERED SWEEP below places a boundary EXACTLY on a block edge by
//     choosing the tempo so `samplesPerStep` is an exact multiple of the block
//     size. Exact coincidence is precisely where the two PPQ expressions for that
//     edge (`blockEndPpq()` of block k vs `blockStartPpq()` of block k+1) differ
//     by an ulp in one direction or the other — the disagreement the header
//     measures at 24.2 % of block edges, and the reason `stepIndexSnapSteps`
//     exists. These are the ulp-either-side cases, reached the only way they can
//     be reached.
//   • The same sweep then dials the boundary a controlled sub-sample distance to
//     each side (±0.05 … ±0.9 samples), all of them outside the #37 window, so
//     both sides of an edge are covered at every distance the clamp can see.
// ─────────────────────────────────────────────────────────────────────────────

#include "support/MidiRenderHarness.h"
#include "support/SequencerRenderRig.h"

#include "engine/graph/EngineCommand.h"
#include "engine/graph/Transport.h"
#include "engine/midi/NotePool.h"
#include "engine/sequencer/PatternTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using arpbox::engine::EngineCommandType;
using arpbox::engine::LaneId;
using arpbox::engine::maxSteps;
using arpbox::engine::PoolSnapshot;
using arpbox::engine::Transport;
using arpbox::testing::engineCommand;
using arpbox::testing::MidiRenderConfig;
using arpbox::testing::MidiRenderResult;
using arpbox::testing::renderSequencer;
using arpbox::testing::ScheduledCommand;
using arpbox::testing::SequencerRig;
using arpbox::testing::TimedMidiEvent;

namespace
{
// ── The musical configuration that makes both derivations observable ─────────

/** LEN as a percentage of the step (§12.1: 1..400). 150 % ⇒ the note is still
    sounding when its own pitch returns one step later, which is the branch
    `cutoffForSamePitch` — and therefore `stepBoundarySample` — actually decides.
    At the default 50 % every note expires first and derivation A never runs. */
constexpr int tiedLenPercent = 150;

/** The single-note pool's only pitch (middle C). One degree ⇒ every gated step
    emits the same note ⇒ every step boundary is a same-pitch retrigger. */
constexpr int poolPitch = 60;

/** Boundaries compared per rendered configuration. Fixed rather than "however
    many the render produced", so the total comparison count is a literal. */
constexpr int boundariesPerRender = 10;

/** Distance from a block edge, in samples, inside which a boundary counts as
    "a sub-sample either side" rather than comfortably mid-block. */
constexpr double subSampleBand = 1.0;

/** `stepIndexSnapSteps` from SequencerProcessor.cpp, in STEPS. Mirrored (not
    included — it is a translation-unit-local constant) purely to compute the #37
    window this sweep asserts it stays out of. It is never used to derive a
    boundary here; see the "not a third derivation" note at the top. */
constexpr double stepIndexSnapStepsMirror = 1.0e-6;

/** `sampleOffsetSnapSamples`, likewise, in SAMPLES: below this distance the snap
    absorbs the offset and the clamp never sees it. */
constexpr double sampleOffsetSnapSamplesMirror = 1.0e-4;

// ── One rendered configuration ───────────────────────────────────────────────

/** One point of the sweep. `firstIndex` is the GLOBAL step index the render
    starts on: the transport is located to exactly `firstIndex * stepPpq`, so
    render sample 0 IS that boundary and boundary n sits at exactly
    `n * samplesPerStep` render samples — no origin bookkeeping. */
struct BoundaryConfig
{
    double bpm = 120.0;
    double sampleRate = 48000.0;
    double stepPpq = 0.25;
    int blockSize = 128;
    std::int64_t firstIndex = 0;
};

/** Samples per step from the MUSICAL definition — used ONLY to classify a
    boundary's position relative to a block edge and to size the render. */
double samplesPerStepOf (const BoundaryConfig& config) noexcept
{
    return config.stepPpq * (60.0 / config.bpm) * config.sampleRate;
}

/** What one sweep accumulated. Catch2 macros are forbidden inside the loops
    (house rule), so every finding lands here and is asserted once at the end. */
struct Observation
{
    std::int64_t configs = 0;      ///< Configurations rendered.
    std::int64_t comparisons = 0;  ///< Boundaries where both derivations were compared.
    std::int64_t mismatches = 0;   ///< Boundaries where they disagreed. MUST be 0.
    std::int64_t shortRenders = 0; ///< Renders that could not supply their quota. MUST be 0.

    /** Renders whose FIRST note-on did not land on render sample 0. MUST be 0: the
        transport is located to exactly `firstIndex * stepPpq`, so boundary
        `firstIndex` IS render sample 0, and every block-edge classification below
        is measured from that origin. Asserted rather than assumed, because a
        silently shifted origin would misclassify every family while leaving the
        agreement property itself green. */
    std::int64_t misalignedRenders = 0;

    // Reachability, by where the boundary sits relative to a block edge.
    std::int64_t onEdge = 0;    ///< Exactly on one (the ulp-either-side family).
    std::int64_t justAbove = 0; ///< Within one sample AFTER an edge.
    std::int64_t justBelow = 0; ///< Within one sample BEFORE an edge.
    std::int64_t midBlock = 0;  ///< Everywhere else.

    /** Boundaries inside the issue #37 window, where B's clamp is licensed to
        differ from A. MUST be 0 — see the window note at the top. */
    std::int64_t snapWindow = 0;

    std::int64_t minIndex = std::numeric_limits<std::int64_t>::max ();
    std::int64_t maxIndex = std::numeric_limits<std::int64_t>::min ();

    /** First few disagreements, spelled out. Bounded so a broken sweep produces a
        readable failure instead of thousands of lines. */
    std::vector<std::string> failures;

    [[nodiscard]] std::string describe () const
    {
        std::string text = "configs " + std::to_string (configs) + ", comparisons " + std::to_string (comparisons) +
                           ", mismatches " + std::to_string (mismatches) + ", short renders " +
                           std::to_string (shortRenders) + ", misaligned renders " +
                           std::to_string (misalignedRenders) + " | on-edge " + std::to_string (onEdge) +
                           ", just-above " + std::to_string (justAbove) + ", just-below " + std::to_string (justBelow) +
                           ", mid-block " + std::to_string (midBlock) + ", #37-window " + std::to_string (snapWindow) +
                           " | step indices " + std::to_string (minIndex) + ".." + std::to_string (maxIndex);

        for (const auto& failure : failures)
            text += "\n    " + failure;

        return text;
    }
};

/** Renders `config` and returns everything the sequencer emitted, at absolute
    render-sample positions. Tempo, then locate, then play — all at sample 0, in
    that order, because `locateToPpq` scales its target by the CURRENT tempo. */
MidiRenderResult renderBoundaries (const BoundaryConfig& config)
{
    SequencerRig rig { config.sampleRate, config.blockSize };

    rig.patternDocument.beginTransaction ();
    rig.patternDocument.setGrid (config.stepPpq);

    PoolSnapshot pool {};
    pool.size = 1;
    pool.sorted[0] = static_cast<std::uint8_t> (poolPitch);
    pool.asPlayed[0] = static_cast<std::uint8_t> (poolPitch);
    rig.patternDocument.setPool (pool);

    for (int step = 0; step < maxSteps; ++step)
        rig.patternDocument.setLaneValue (0, LaneId::len, step, tiedLenPercent);

    rig.patternDocument.endTransaction ();

    const double startPpq = static_cast<double> (config.firstIndex) * config.stepPpq;
    const double samplesPerStep = samplesPerStepOf (config);

    // Quota plus one boundary of slack plus two blocks, so the last compared pair
    // is comfortably inside the render rather than at its ragged end.
    const auto span = static_cast<std::int64_t> (
        std::ceil (static_cast<double> (boundariesPerRender + 1) * samplesPerStep + 2.0 * config.blockSize));

    auto renderConfig = MidiRenderConfig::samples (span, config.sampleRate, config.blockSize);
    renderConfig.numChannels = 1;
    renderConfig.eventReserve = 8192;

    const std::vector<ScheduledCommand> schedule {
        ScheduledCommand { 0, engineCommand (EngineCommandType::setTempoBpm, config.bpm) },
        ScheduledCommand { 0, engineCommand (EngineCommandType::transportLocate, startPpq) },
        ScheduledCommand { 0, engineCommand (EngineCommandType::transportPlay) }
    };

    return renderSequencer (rig, renderConfig, schedule);
}

/** Renders one configuration and folds its boundaries into `observation`.

    THE COMPARISON, restated: `noteOn[n + 1]` is derivation B's answer for step
    boundary `firstIndex + n + 1`; `noteOff[n]` is derivation A's answer for the
    SAME boundary, minus §5.5's one-sample gap. Nothing else in this configuration
    can produce a note-off — one pool degree, no pattern switch, no transport stop
    — so the pairing is structural, not a guess. */
void sweepConfiguration (const BoundaryConfig& config, Observation& observation)
{
    ++observation.configs;

    const auto render = renderBoundaries (config);
    const auto ons = render.select ([] (const TimedMidiEvent& event) { return event.message.isNoteOn (); });
    const auto offs = render.select ([] (const TimedMidiEvent& event) { return event.message.isNoteOff (); });

    if (static_cast<int> (ons.size ()) < boundariesPerRender + 1 ||
        static_cast<int> (offs.size ()) < boundariesPerRender)
    {
        ++observation.shortRenders;
        return;
    }

    if (ons[0].absoluteSample != 0)
    {
        ++observation.misalignedRenders;
        return;
    }

    const double samplesPerStep = samplesPerStepOf (config);
    const double blockSize = static_cast<double> (config.blockSize);

    // The #37 window, in samples, for this configuration (see the note at the top).
    const double windowSamples = stepIndexSnapStepsMirror * samplesPerStep;

    for (int n = 0; n < boundariesPerRender; ++n)
    {
        const std::int64_t index = config.firstIndex + n + 1;
        const std::int64_t fromWalk = ons[static_cast<std::size_t> (n + 1)].absoluteSample;
        const std::int64_t fromBoundarySample = offs[static_cast<std::size_t> (n)].absoluteSample + 1;

        ++observation.comparisons;
        observation.minIndex = std::min (observation.minIndex, index);
        observation.maxIndex = std::max (observation.maxIndex, index);

        // Where this boundary sits relative to the block grid. Render sample 0 is
        // exactly boundary `firstIndex`, so boundary `firstIndex + n + 1` sits at
        // exactly `(n + 1) * samplesPerStep` render samples.
        const double truePosition = static_cast<double> (n + 1) * samplesPerStep;
        const double aboveEdge = truePosition - std::floor (truePosition / blockSize) * blockSize;
        const double belowEdge = blockSize - aboveEdge;

        if (aboveEdge < sampleOffsetSnapSamplesMirror || belowEdge < sampleOffsetSnapSamplesMirror)
            ++observation.onEdge;
        else if (aboveEdge < subSampleBand)
            ++observation.justAbove;
        else if (belowEdge < subSampleBand)
            ++observation.justBelow;
        else
            ++observation.midBlock;

        if (belowEdge > sampleOffsetSnapSamplesMirror && belowEdge <= windowSamples)
            ++observation.snapWindow;

        if (fromWalk != fromBoundarySample)
        {
            ++observation.mismatches;

            if (observation.failures.size () < 8)
                observation.failures.push_back (
                    "step " + std::to_string (index) + " @ " + std::to_string (config.bpm) + " BPM / " +
                    std::to_string (config.sampleRate) + " Hz / stepPpq " + std::to_string (config.stepPpq) +
                    " / block " + std::to_string (config.blockSize) + ": walk placed the note-on at " +
                    std::to_string (fromWalk) + ", stepBoundarySample implied " + std::to_string (fromBoundarySample) +
                    " (note-off at " + std::to_string (fromBoundarySample - 1) + ")");
        }
    }
}

// ── THE BROAD SWEEP: every supported grid x tempo x rate x buffer size ───────

/** §2.1's grid range, 1/32 .. 1/4, each straight / triplet (x2/3) / dotted (x3/2)
    — the full set of `PatternDocument::setGrid` values the product supports, and
    the dimension a per-pattern grid (the change #54 is about) would multiply. */
constexpr double sweptStepPpq[] = {
    0.125, 0.125 * 2.0 / 3.0, 0.125 * 1.5, // 1/32
    0.25,  0.25 * 2.0 / 3.0,  0.25 * 1.5,  // 1/16
    0.5,   0.5 * 2.0 / 3.0,   0.5 * 1.5,   // 1/8
    1.0,   1.0 * 2.0 / 3.0,   1.0 * 1.5    // 1/4
};

/** The tempo axis, spanning the supported range. The endpoints are asserted to BE
    `Transport::minBpm` / `maxBpm` rather than assumed; the interior values are
    deliberately not round, so `samplesPerStep` is irrational-looking and
    boundaries land at every sub-sample phase relative to the block grid. */
constexpr double sweptBpm[] = { 20.0, 47.5, 120.0, 173.3, 300.0 };

/** Both rates the suite runs at (§3.1 targets CoreAudio defaults). */
constexpr double sweptSampleRate[] = { 44100.0, 48000.0 };

/** Buffer sizes: the smallest and largest realistic device buffers plus three in
    between. Agreement is per-block-size because only derivation B knows the block. */
constexpr int sweptBlockSize[] = { 32, 64, 128, 512, 4096 };

/** Step-index origins, rotated across the sweep so the compared indices span the
    same negative-to-large range tests/step_purity.cpp drives `evaluateStep` over
    (-37 .. 260). Negative origins are reachable because `transportLocate` accepts
    a negative PPQ (Transport.cpp deliberately keeps the negative case coherent). */
constexpr std::int64_t sweptFirstIndex[] = { -38, 0, 250 };

constexpr int numSweptGrids = static_cast<int> (std::size (sweptStepPpq));
constexpr int numSweptBpm = static_cast<int> (std::size (sweptBpm));
constexpr int numSweptRates = static_cast<int> (std::size (sweptSampleRate));
constexpr int numSweptBlockSizes = static_cast<int> (std::size (sweptBlockSize));

/** 12 grids x 5 tempos x 2 rates x 5 buffer sizes. */
constexpr int broadSweepConfigs = numSweptGrids * numSweptBpm * numSweptRates * numSweptBlockSizes;

// ── THE ENGINEERED SWEEP: boundaries dialled onto and around a block edge ────

/** The block-edge sample every engineered configuration aims at. 12288 is a
    multiple of every swept block size (32 x 384, 64 x 192, 128 x 96, 512 x 24,
    4096 x 3), so ONE tempo puts a boundary on an edge at all five. */
constexpr double engineeredEdgeSample = 12288.0;

/** Signed distance, in samples, from that edge to the boundary. 0 is the exact
    coincidence — the ulp-either-side case, where the block edge and the boundary
    are the same number only in exact arithmetic. The rest walk both sides at
    sub-sample distances.

    EVERY NEGATIVE VALUE STAYS OUTSIDE THE #37 WINDOW BY CONSTRUCTION: the window
    here is 1e-6 x 12288 = 0.0123 samples, and the closest offset is 0.05. That is
    asserted, not assumed — `Observation::snapWindow` counts any boundary that
    lands inside it and is REQUIREd to be zero. */
constexpr double engineeredOffsetSamples[] = { 0.0, 0.05, 0.25, 0.5, 0.9, -0.05, -0.25, -0.5, -0.9 };

/** Grids used by the engineered sweep: the four straight values. Triplet/dotted
    are excluded here only because `bpm = stepPpq x 60 x rate / samplesPerStep`
    would leave the supported 20..300 range at this edge sample; the broad sweep
    above covers them. */
constexpr double engineeredStepPpq[] = { 0.125, 0.25, 0.5, 1.0 };

constexpr int numEngineeredGrids = static_cast<int> (std::size (engineeredStepPpq));
constexpr int numEngineeredOffsets = static_cast<int> (std::size (engineeredOffsetSamples));

/** 4 grids x 2 rates x 5 buffer sizes x 9 offsets. */
constexpr int engineeredSweepConfigs = numEngineeredGrids * numSweptRates * numSweptBlockSizes * numEngineeredOffsets;

/** The tempo that puts step boundary 1 exactly `offset` samples from
    `engineeredEdgeSample`: `samplesPerStep = edge + offset`, inverted. */
double engineeredBpm (double stepPpq, double sampleRate, double offset) noexcept
{
    return stepPpq * 60.0 * sampleRate / (engineeredEdgeSample + offset);
}
} // namespace

TEST_CASE ("midi-conformance/boundary-agreement: the sweep is wired to the supported parameter ranges",
           "[unit][midi-conformance]")
{
    // ANTI-VACUITY, part one: the sweep's own axes. Every claim the two cases
    // below make about coverage rests on these, and a silent edit to
    // `Transport::minBpm` or to the grid list would otherwise leave them green
    // while covering less than they say.
    REQUIRE (sweptBpm[0] == Transport::minBpm);
    REQUIRE (sweptBpm[numSweptBpm - 1] == Transport::maxBpm);
    REQUIRE (numSweptGrids == 12); // 1/32..1/4 x straight/triplet/dotted (§2.1)
    REQUIRE (broadSweepConfigs == 600);
    REQUIRE (engineeredSweepConfigs == 360);

    // Every engineered tempo must be inside the supported range, or `setTempoBpm`
    // would clamp it and the boundary would land nowhere near the intended edge.
    int inRange = 0;

    for (const double stepPpq : engineeredStepPpq)
        for (const double sampleRate : sweptSampleRate)
            for (const double offset : engineeredOffsetSamples)
            {
                const double bpm = engineeredBpm (stepPpq, sampleRate, offset);

                if (bpm >= Transport::minBpm && bpm <= Transport::maxBpm)
                    ++inRange;
            }

    REQUIRE (inRange == numEngineeredGrids * numSweptRates * numEngineeredOffsets);

    // Every engineered offset is either the exact coincidence or outside the #37
    // window at this edge (1e-6 x 12288 samples), so the walk's clamp can never
    // fire and the two derivations owe each other EXACT agreement.
    const double window = stepIndexSnapStepsMirror * engineeredEdgeSample;
    int outsideWindow = 0;

    for (const double offset : engineeredOffsetSamples)
        if (offset >= 0.0 || -offset > window)
            ++outsideWindow;

    REQUIRE (outsideWindow == numEngineeredOffsets);
}

TEST_CASE ("midi-conformance/boundary-agreement: stepBoundarySample and the step walk place the same boundary on the "
           "same sample (issue #54)",
           "[unit][midi-conformance]")
{
    SECTION ("across every supported grid, tempo, sample rate and buffer size")
    {
        Observation observation;

        for (const double stepPpq : sweptStepPpq)
            for (const double bpm : sweptBpm)
                for (const double sampleRate : sweptSampleRate)
                    for (const int blockSize : sweptBlockSize)
                    {
                        // The origin rotates so the compared indices span negative,
                        // zero-based and large values across the sweep rather than
                        // multiplying its cost by three.
                        const auto origin =
                            sweptFirstIndex[static_cast<std::size_t> (observation.configs % 3)]; // NOLINT

                        sweepConfiguration (BoundaryConfig { bpm, sampleRate, stepPpq, blockSize, origin },
                                            observation);
                    }

        INFO (observation.describe ());

        // THE PROPERTY.
        REQUIRE (observation.mismatches == 0);

        // ANTI-VACUITY: exactly this many boundaries were compared, from first
        // principles — 600 configurations x 10 boundaries each — and not one
        // render fell short of its quota.
        REQUIRE (observation.configs == broadSweepConfigs);
        REQUIRE (observation.shortRenders == 0);
        REQUIRE (observation.misalignedRenders == 0);
        REQUIRE (observation.comparisons == broadSweepConfigs * boundariesPerRender);
        REQUIRE (observation.comparisons == 6000);

        // REACHABILITY, per family rather than in aggregate.
        REQUIRE (observation.onEdge > 0);
        REQUIRE (observation.justAbove > 0);
        REQUIRE (observation.justBelow > 0);
        REQUIRE (observation.midBlock > 0);

        // The sweep stays outside the one licensed difference (issue #37).
        REQUIRE (observation.snapWindow == 0);

        // The index range `evaluateStep` is driven over in tests/step_purity.cpp.
        REQUIRE (observation.minIndex <= -37);
        REQUIRE (observation.maxIndex >= 260);
    }

    SECTION ("with boundaries dialled onto a block edge and a sub-sample either side")
    {
        Observation observation;

        for (const double stepPpq : engineeredStepPpq)
            for (const double sampleRate : sweptSampleRate)
                for (const int blockSize : sweptBlockSize)
                    for (const double offset : engineeredOffsetSamples)
                        sweepConfiguration (BoundaryConfig { engineeredBpm (stepPpq, sampleRate, offset),
                                                             sampleRate,
                                                             stepPpq,
                                                             blockSize,
                                                             0 },
                                            observation);

        INFO (observation.describe ());

        // THE PROPERTY, on the boundaries most able to break it.
        REQUIRE (observation.mismatches == 0);

        REQUIRE (observation.configs == engineeredSweepConfigs);
        REQUIRE (observation.shortRenders == 0);
        REQUIRE (observation.misalignedRenders == 0);
        REQUIRE (observation.comparisons == engineeredSweepConfigs * boundariesPerRender);
        REQUIRE (observation.comparisons == 3600);

        // REACHABILITY. This sweep exists for the first three families; a
        // configuration whose tempo had been clamped, or whose edge arithmetic had
        // drifted, would show up as one of them collapsing to zero.
        REQUIRE (observation.onEdge > 0);
        REQUIRE (observation.justAbove > 0);
        REQUIRE (observation.justBelow > 0);

        REQUIRE (observation.snapWindow == 0);
    }
}
