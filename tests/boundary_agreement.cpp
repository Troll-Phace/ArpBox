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
#include "engine/sequencer/SequencerProcessor.h"

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

// ─────────────────────────────────────────────────────────────────────────────
// PHASE 7.2 STAGE 2: `ownsPpq` IS THE INDEX RANGE, RE-EXPRESSED
//
// The step walk used to decide "does this block emit step n?" with an INDEX test
// (`firstIndex <= n < endIndex`). Stage 2 replaced it with a PPQ test, because once
// MICRO / swing / ratchets displace an onset the index stops describing which block
// contains the event. The replacement is only licensed if the two are the same
// predicate while the displacement is zero — which is the whole reason stage 2 is
// landed on its own, with the six goldens as its proof.
//
// THE ALGEBRA, for integer n and stepPpq > 0:
//     n >= snappedStepCeiling (blockStartPpq, stepPpq)
//   ⟺ n >= blockStartPpq / stepPpq - stepIndexSnapSteps      (integer vs ceil)
//   ⟺ n * stepPpq >= blockStartPpq - stepIndexSnapSteps * stepPpq
// and identically at the upper end (`n < ceil (y) ⟺ n < y` for integer n).
//
// WHY THIS IS ASSERTED RATHER THAN LEFT AS THAT PARAGRAPH: the two sides are equal
// in exact arithmetic and round DIFFERENTLY in floating point — one divides by
// `stepPpq` and ceils, the other multiplies by it and compares. The identity
// survives only because `stepIndexSnapSteps` is ~7 orders of magnitude wider than
// the ulp noise either side. That is a quantitative claim about real block geometry,
// so it is checked against real block geometry.
//
// BOTH PREDICATES ARE MIRRORED HERE, DELIBERATELY, and that is NOT the "third
// derivation" the note at the top of this file forbids. That note is about pinning a
// site to the test's own arithmetic instead of to the other site; the property here
// IS an equivalence between two formulations, so mirroring both and comparing them
// to each other is the only shape it can take. The behavioural half — that
// production actually uses `ownsPpq`, and that changing its tolerance is caught — is
// carried by the goldens and the determinism suite, verified fails-without by
// removing the pattern-switch ownership guard (9 tests red, two of them goldens).

/** `snappedStepCeiling` from SequencerProcessor.cpp — the OLD formulation's half. */
std::int64_t mirroredStepCeiling (double ppq, double stepPpq) noexcept
{
    return static_cast<std::int64_t> (std::ceil (ppq / stepPpq - stepIndexSnapStepsMirror));
}

/** `ownsPpq` from SequencerProcessor.cpp — the NEW formulation. Note the SINGLE
    `snapPpq` expression applied to BOTH ends: that is what makes block k's upper
    test and block k+1's lower test the same test on the same number, i.e. what makes
    the half-open spans tile the timeline with no gap and no overlap. A second or
    different tolerance would leave an event owned by zero blocks (dropped) or two
    (duplicated), which is why this mirror reproduces it verbatim. */
bool mirroredOwnsPpq (const Transport& transport, double ppq, double stepPpq) noexcept
{
    const double snapPpq = stepIndexSnapStepsMirror * stepPpq;

    return ppq >= transport.blockStartPpq () - snapPpq && ppq < transport.blockEndPpq () - snapPpq;
}

/** The stage-2 scan widening (`stepScanBack` / `stepScanForward`). Mirrored so the
    equivalence is checked over exactly the index range the walk now VISITS, not just
    the narrower range it used to. */
constexpr std::int64_t mirroredScanBack = 2;
constexpr std::int64_t mirroredScanForward = 1;

// ── REACHING THE ONLY GEOMETRY THAT CAN BREAK THE EQUIVALENCE ────────────────
// MEASURED, and it is why this file needed a second sweep rather than a second
// SECTION over the existing configurations: across all 600 broad configurations,
// driven for 24 blocks each, ZERO ever place a step boundary on a block edge. The
// two formulations are then separated by ~1e-6 steps of shared tolerance against
// ~1e-13 steps of rounding noise, so they cannot differ and the case would have been
// green by construction — verified: deleting the upper tolerance from
// `mirroredOwnsPpq` left the broad sweep entirely green.
//
// The decisive window is `[blockEdgePpq - snapPpq, blockEdgePpq)`: a boundary in
// THERE is claimed by the later block by the snapped ceiling, and it is the only
// place the shared tolerance — rather than a comfortable margin — decides the
// answer. So these configurations dial a boundary onto a block edge and then a
// controlled distance either side, measured in STEPS so the offsets sit at known
// multiples of the tolerance regardless of grid, tempo or sample rate.

/** Signed distance, in STEPS, from an exact boundary/block-edge coincidence.
    `stepIndexSnapSteps` is 1e-6, so the three values between -1e-6 and 0 land INSIDE
    the tolerance window (where a divergence would live), -2e-6 and -1e-4 land just
    outside it below, and the positive values cover the other side. 0.0 is the exact
    coincidence, where the two PPQ expressions for one edge differ by an ulp in
    whichever direction the arithmetic happens to give. */
constexpr double engineeredEdgeOffsetSteps[] = { 0.0,     -0.25e-6, -0.5e-6, -0.9e-6, -2.0e-6,
                                                 -1.0e-4, 0.5e-6,   2.0e-6,  1.0e-4 };

constexpr int numEdgeOffsets = static_cast<int> (std::size (engineeredEdgeOffsetSteps));

/** How many whole blocks one step spans in the coincidence configurations.

    Chosen as the smallest multiple of `blockSize` that keeps the implied tempo
    inside `Transport`'s supported range, PLUS ONE BLOCK OF HEADROOM. The headroom is
    not cosmetic: without it `edgeSample` can equal the exact `maxBpm` step length,
    and a negative offset then implies a tempo above 300 BPM, which `setTempoBpm`
    CLAMPS — silently moving the boundary away from the edge this sweep exists to
    engineer. The wiring test asserts every derived tempo is in range. */
std::int64_t coincidenceBlocksPerStep (double stepPpq, double sampleRate, int blockSize) noexcept
{
    const double fastestStepSamples = stepPpq * 60.0 * sampleRate / Transport::maxBpm;

    return static_cast<std::int64_t> (std::ceil (fastestStepSamples / static_cast<double> (blockSize))) + 1;
}

/** The tempo that makes one step span exactly `blocksPerStep` blocks, displaced by
    `offsetSteps` steps — so step boundary 1 sits `offsetSteps` steps from the block
    edge at `blocksPerStep * blockSize` samples. */
double coincidenceBpm (double stepPpq,
                       double sampleRate,
                       int blockSize,
                       std::int64_t blocksPerStep,
                       double offsetSteps) noexcept
{
    const double edgeSample = static_cast<double> (blocksPerStep * blockSize);

    return stepPpq * 60.0 * sampleRate / (edgeSample * (1.0 + offsetSteps));
}

/** Blocks driven per coincidence configuration: enough to carry the playhead a
    little past the engineered edge, and no further. */
int coincidenceBlockCount (std::int64_t blocksPerStep) noexcept
{
    return static_cast<int> (blocksPerStep) + 3;
}

/** What the equivalence sweep accumulated. Same house rule as `Observation`: no
    Catch2 macro inside a loop. */
struct OwnershipObservation
{
    std::int64_t configs = 0;     ///< Configurations driven.
    std::int64_t blocks = 0;      ///< Blocks stepped, across all configurations.
    std::int64_t comparisons = 0; ///< (block, index) pairs where both predicates were evaluated.
    std::int64_t mismatches = 0;  ///< Pairs where they disagreed. MUST be 0.

    // Reachability: an equivalence is vacuous if one side never fires.
    std::int64_t owned = 0;        ///< Pairs both predicates ACCEPTED.
    std::int64_t rejectedLow = 0;  ///< Pairs both REJECTED as below this block.
    std::int64_t rejectedHigh = 0; ///< Pairs both REJECTED as at/after this block's end.

    /** Pairs reached ONLY because of the widening — i.e. `index < firstIndex` or
        `index >= endIndex`. Counted per side and each REQUIREd > 0, because the
        widened band is exactly where an index-vs-PPQ disagreement would live. */
    std::int64_t widenedBelow = 0;
    std::int64_t widenedAbove = 0;

    /** THE ONLY GEOMETRY WHERE THE TWO FORMULATIONS CAN DISAGREE, and therefore the
        reachability counters this whole case lives or dies by — see
        `engineeredEdgeOffsetSteps`. A boundary sitting within `snapPpq` BELOW a block
        edge is the one place the shared tolerance decides the answer; anywhere else
        the two are separated by ~1e-6 steps of margin against ~1e-13 of rounding
        noise and cannot possibly differ. Both REQUIREd > 0. */
    std::int64_t insideEndWindow = 0;
    std::int64_t insideStartWindow = 0;

    std::vector<std::string> failures;

    [[nodiscard]] std::string describe () const
    {
        std::string text = "configs " + std::to_string (configs) + ", blocks " + std::to_string (blocks) +
                           ", comparisons " + std::to_string (comparisons) + ", mismatches " +
                           std::to_string (mismatches) + " | owned " + std::to_string (owned) + ", rejected-low " +
                           std::to_string (rejectedLow) + ", rejected-high " + std::to_string (rejectedHigh) +
                           " | widened-below " + std::to_string (widenedBelow) + ", widened-above " +
                           std::to_string (widenedAbove) + " | in-end-window " + std::to_string (insideEndWindow) +
                           ", in-start-window " + std::to_string (insideStartWindow);

        for (const auto& failure : failures)
            text += "\n    " + failure;

        return text;
    }
};

/** Blocks driven per configuration in the BROAD equivalence sweep, so its total is a
    literal. The engineered sweep sizes its own count from the geometry it dials in. */
constexpr int equivalenceBlocksPerConfig = 24;

/** Drives a bare `Transport` for `numBlocks` blocks and, for every block, compares
    the two ownership formulations over the WIDENED index range the stage-2 walk
    visits.

    The transport is production code and is driven exactly as the graph drives it
    (`applyCommand` during the drain, then `beginBlock` once per block), so
    `blockStartPpq()` / `blockEndPpq()` are the real latched values the real walk
    sees — including the ulp-level disagreement between block k's end and block
    k+1's start that `stepIndexSnapSteps` exists to absorb. */
void sweepOwnershipEquivalence (const BoundaryConfig& config, int numBlocks, OwnershipObservation& observation)
{
    ++observation.configs;

    Transport transport;
    transport.prepare (config.sampleRate);
    transport.applyCommand (engineCommand (EngineCommandType::setTempoBpm, config.bpm));
    transport.applyCommand (
        engineCommand (EngineCommandType::transportLocate, static_cast<double> (config.firstIndex) * config.stepPpq));
    transport.applyCommand (engineCommand (EngineCommandType::transportPlay));

    const double snapPpq = stepIndexSnapStepsMirror * config.stepPpq;

    for (int block = 0; block < numBlocks; ++block)
    {
        transport.beginBlock (config.blockSize);
        ++observation.blocks;

        const std::int64_t firstIndex = mirroredStepCeiling (transport.blockStartPpq (), config.stepPpq);
        const std::int64_t endIndex = mirroredStepCeiling (transport.blockEndPpq (), config.stepPpq);

        for (std::int64_t index = firstIndex - mirroredScanBack; index < endIndex + mirroredScanForward; ++index)
        {
            const double ppq = static_cast<double> (index) * config.stepPpq;
            const bool byIndex = index >= firstIndex && index < endIndex;
            const bool byPpq = mirroredOwnsPpq (transport, ppq, config.stepPpq);

            ++observation.comparisons;

            if (ppq >= transport.blockEndPpq () - snapPpq && ppq < transport.blockEndPpq ())
                ++observation.insideEndWindow;

            if (ppq >= transport.blockStartPpq () - snapPpq && ppq < transport.blockStartPpq ())
                ++observation.insideStartWindow;

            if (index < firstIndex)
                ++observation.widenedBelow;
            else if (index >= endIndex)
                ++observation.widenedAbove;

            if (byIndex)
                ++observation.owned;
            else if (index < firstIndex)
                ++observation.rejectedLow;
            else
                ++observation.rejectedHigh;

            if (byIndex != byPpq)
            {
                ++observation.mismatches;

                if (observation.failures.size () < 8)
                    observation.failures.push_back (
                        "step " + std::to_string (index) + " @ " + std::to_string (config.bpm) + " BPM / " +
                        std::to_string (config.sampleRate) + " Hz / stepPpq " + std::to_string (config.stepPpq) +
                        " / block " + std::to_string (config.blockSize) + ": index range said " +
                        (byIndex ? "OWNED" : "not owned") + ", ownsPpq said " + (byPpq ? "OWNED" : "not owned") +
                        " (blockStartPpq " + std::to_string (transport.blockStartPpq ()) + ", blockEndPpq " +
                        std::to_string (transport.blockEndPpq ()) + ")");
            }
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// PHASE 7.3: ISSUE #75's WITNESS — A RENDERED EVENT INSIDE THE #37 WINDOW
//
// ── WHAT #75 IS, AND WHY PHASE 7.2 PUT IT ON THE CRITICAL PATH ──────────────
// The #37 window is the one documented exception to buffer-size independence: an
// event whose TRUE position lies within `stepIndexSnapSteps` BELOW a block edge is
// claimed by the LATER block (that is what the snapped ceiling does, deliberately),
// its `blockOffsetForPpq` result then comes back very slightly NEGATIVE, and the
// walk's surviving lower clamp emits it at offset 0 — up to ONE SAMPLE LATER than a
// carving under which the same event falls mid-block and is placed exactly.
//
// #75 IS THAT NO TEST EVER RENDERED SUCH AN EVENT. Both sweeps above deliberately
// stay out of the window and assert `snapWindow == 0` / `insideEndWindow == 0`; the
// equivalence section reaches the window but only compares PREDICATES against a bare
// `Transport` — nothing was ever emitted from inside it. So the documented "up to one
// sample" was an argument about code, not a measurement.
//
// Phase 7.2 made that gap live rather than theoretical. The window's WIDTH is
// unchanged (it is denominated against the untouched grid), but a step now offers up
// to EIGHT positions to be tested against block edges instead of one, so the hit rate
// rose by about that factor. This section closes the coverage half of #75 by rendering
// a RATCHET CHILD inside the window and pinning the divergence as an EXACT VALUE —
// exactly 1 sample, not "up to one".
//
// ── THE GEOMETRY, DIALLED IN ────────────────────────────────────────────────
// `windowEdgeSample` (12288) is a block head at 4096 and NOT at 480 — the only two
// sizes this section needs. A tempo is chosen so that step 0's ratchet CHILD 1 (half
// a step, at RATCHET 2) lands `windowInsideOffsetSamples` BELOW that edge: 0.006
// samples, which is inside the window (1e-6 x 24575.988 = 0.0246 samples) and two
// orders of magnitude above `sampleOffsetSnapSamples` (1e-4), so the snap cannot
// absorb it and the clamp is genuinely reached.
//
//   at block 4096  12288 is a head ⇒ the child is claimed by the block STARTING there,
//                  its raw offset is -0.006, and the clamp emits it at 12288
//   at block 480   12288 is not a head ⇒ the child is mid-block and is placed exactly,
//                  at floor (12287.994) = 12287
//
// One sample apart, at a position decided by the buffer size. THAT IS THE DOCUMENTED
// BEHAVIOUR, not a defect — and it is now a measured number instead of a bound.
// ═════════════════════════════════════════════════════════════════════════════

/** The block-edge sample the witness aims a displaced child at. 12288 = 2^12 x 3, so
    it IS a head at 4096 and is NOT at 480 (12288 / 480 = 25.6) — the two carvings the
    witness contrasts. */
constexpr double windowEdgeSample = 12288.0;

/** How far BELOW that edge the child is placed, in samples. Inside the window
    (asserted) and far above `sampleOffsetSnapSamples` (also asserted), which is the
    combination that makes the lower clamp — rather than the snap — decide. */
constexpr double windowInsideOffsetSamples = 0.006;

/** Ratchet children the witness uses. TWO, so child 1 sits at exactly half a step and
    the arithmetic that places it is a single exact division. */
constexpr int windowRatchetChildren = 2;

/** The two carvings the witness contrasts: `windowEdgeSample` is a block head at the
    first and strictly mid-block at the second. */
constexpr int windowHeadBlockSize = 4096;
constexpr int windowMidBlockSize = 480;

/** The tempo that puts step 0's child 1 exactly `windowInsideOffsetSamples` below
    `windowEdgeSample`: child 1 is at `samplesPerStep / 2`, so
    `samplesPerStep = 2 x (edge - offset)`, inverted through the grid. */
double windowWitnessBpm (double stepPpq, double sampleRate) noexcept
{
    const double samplesPerStep = 2.0 * (windowEdgeSample - windowInsideOffsetSamples);

    return stepPpq * 60.0 * sampleRate / samplesPerStep;
}

/** What one witness render reported. */
struct WindowWitness
{
    std::int64_t noteOns = 0;
    std::int64_t childOnset = -1;   ///< The emitted sample of step 0's child 1.
    std::int64_t exactPosition = 0; ///< floor of its true position, for the report.
    bool edgeIsBlockHead = false;
};

/** Renders the witness configuration at `blockSize` and reports where step 0's child 1
    actually landed. RATCHET `windowRatchetChildren`, a one-note pool and a short gate:
    the question is purely WHERE the child's note-on is placed. */
WindowWitness renderWindowWitness (int blockSize, double stepPpq, double sampleRate)
{
    const double bpm = windowWitnessBpm (stepPpq, sampleRate);

    SequencerRig rig { sampleRate, blockSize };

    rig.patternDocument.beginTransaction ();
    rig.patternDocument.setGrid (stepPpq);

    PoolSnapshot pool {};
    pool.size = 1;
    pool.sorted[0] = static_cast<std::uint8_t> (poolPitch);
    pool.asPlayed[0] = static_cast<std::uint8_t> (poolPitch);
    rig.patternDocument.setPool (pool);

    for (int step = 0; step < maxSteps; ++step)
    {
        rig.patternDocument.setLaneValue (0, LaneId::ratchet, step, windowRatchetChildren);
        rig.patternDocument.setLaneValue (0, LaneId::len, step, 25);
    }

    rig.patternDocument.endTransaction ();

    // THE SAME ABSOLUTE SPAN AT BOTH CARVINGS, so the two renders cover identical
    // musical time and their note-on counts are directly comparable. Sizing it from
    // `blockSize` instead would have made the anti-vacuity counts differ for a reason
    // that has nothing to do with the window.
    constexpr std::int64_t witnessSpan = static_cast<std::int64_t> (windowEdgeSample) + 4LL * windowHeadBlockSize;
    const auto span = witnessSpan;

    auto renderConfig = MidiRenderConfig::samples (span, sampleRate, blockSize);
    renderConfig.numChannels = 1;
    renderConfig.eventReserve = 4096;

    const std::vector<ScheduledCommand> schedule {
        ScheduledCommand { 0, engineCommand (EngineCommandType::setTempoBpm, bpm) },
        ScheduledCommand { 0, engineCommand (EngineCommandType::transportPlay) }
    };

    const auto render = renderSequencer (rig, renderConfig, schedule);

    WindowWitness witness;
    witness.edgeIsBlockHead = static_cast<std::int64_t> (windowEdgeSample) % blockSize == 0;
    witness.exactPosition = static_cast<std::int64_t> (std::floor (windowEdgeSample - windowInsideOffsetSamples));

    // Step 0's child 1 is the SECOND note-on of the render (child 0 is at sample 0).
    for (const auto& event : render.events)
        if (event.message.isNoteOn ())
        {
            ++witness.noteOns;

            if (witness.noteOns == 2)
                witness.childOnset = event.absoluteSample;
        }

    return witness;
}

// ── THE 8-CHILD CHAIN: THE DENSEST INSTANCE OF THE TWO-DERIVATION PROBLEM ────

/** Ratchet children the chain re-run uses. EIGHT — every child of every step is a
    same-pitch retrigger of its predecessor, so each consecutive pair is one walk
    PLACEMENT against one lookahead PREDICTION of the same position, and a step
    contributes eight of them instead of one. */
constexpr int chainRatchetChildren = 8;

/** LEN for that chain: 400 % ⇒ a child's natural end is far past the next child's
    onset, so EVERY pair is decided by `cutoffForSamePitch` rather than by the gate. */
constexpr int chainLenPercent = 400;

/** What one 8-child chain sweep observed. */
struct ChainObservation
{
    std::int64_t configs = 0;
    std::int64_t pairs = 0;      ///< Consecutive (off, next on) pairs compared.
    std::int64_t mismatches = 0; ///< Pairs where `off + 1 != nextOn`. MUST be 0.
    std::int64_t shortRenders = 0;

    std::vector<std::string> failures;

    [[nodiscard]] std::string describe () const
    {
        std::string text = "configs " + std::to_string (configs) + ", pairs " + std::to_string (pairs) +
                           ", mismatches " + std::to_string (mismatches) + ", short renders " +
                           std::to_string (shortRenders);

        for (const auto& failure : failures)
            text += "\n    " + failure;

        return text;
    }
};

/** Renders `config` with RATCHET 8 over a one-note pool at LEN 400 % and folds the
    whole child chain's `off + 1 == nextOn` relation into `observation`.

    THE SAME TWO DERIVATIONS AS THE REST OF THIS FILE, at eight times the density: the
    note-ON comes from the walk's `ownsPpq` + `sampleForPpq` placement, and the
    note-OFF one sample earlier comes from `cutoffForSamePitch`'s PREDICTION of that
    same position. A single-sided edit to either shows up as a broken pair. */
void sweepChildChain (const BoundaryConfig& config, ChainObservation& observation)
{
    ++observation.configs;

    SequencerRig rig { config.sampleRate, config.blockSize };

    rig.patternDocument.beginTransaction ();
    rig.patternDocument.setGrid (config.stepPpq);

    PoolSnapshot pool {};
    pool.size = 1;
    pool.sorted[0] = static_cast<std::uint8_t> (poolPitch);
    pool.asPlayed[0] = static_cast<std::uint8_t> (poolPitch);
    rig.patternDocument.setPool (pool);

    for (int step = 0; step < maxSteps; ++step)
    {
        rig.patternDocument.setLaneValue (0, LaneId::ratchet, step, chainRatchetChildren);
        rig.patternDocument.setLaneValue (0, LaneId::len, step, chainLenPercent);
    }

    rig.patternDocument.endTransaction ();

    const double samplesPerStep = samplesPerStepOf (config);
    const auto span =
        static_cast<std::int64_t> (std::ceil (4.0 * samplesPerStep + 2.0 * static_cast<double> (config.blockSize)));

    auto renderConfig = MidiRenderConfig::samples (span, config.sampleRate, config.blockSize);
    renderConfig.numChannels = 1;
    renderConfig.eventReserve = 8192;

    const std::vector<ScheduledCommand> schedule {
        ScheduledCommand { 0, engineCommand (EngineCommandType::setTempoBpm, config.bpm) },
        ScheduledCommand { 0,
                           engineCommand (EngineCommandType::transportLocate,
                                          static_cast<double> (config.firstIndex) * config.stepPpq) },
        ScheduledCommand { 0, engineCommand (EngineCommandType::transportPlay) }
    };

    const auto render = renderSequencer (rig, renderConfig, schedule);
    const auto ons = render.select ([] (const TimedMidiEvent& event) { return event.message.isNoteOn (); });
    const auto offs = render.select ([] (const TimedMidiEvent& event) { return event.message.isNoteOff (); });

    // Two whole steps' worth of children, so the chain crosses a step boundary as well
    // as running inside one.
    constexpr int chainPairs = 2 * chainRatchetChildren;

    if (static_cast<int> (ons.size ()) < chainPairs || static_cast<int> (offs.size ()) < chainPairs - 1)
    {
        ++observation.shortRenders;
        return;
    }

    for (int i = 0; i + 1 < chainPairs; ++i)
    {
        const std::int64_t off = offs[static_cast<std::size_t> (i)].absoluteSample;
        const std::int64_t nextOn = ons[static_cast<std::size_t> (i + 1)].absoluteSample;

        ++observation.pairs;

        if (off + 1 != nextOn)
        {
            ++observation.mismatches;

            if (observation.failures.size () < 8)
                observation.failures.push_back (
                    "child " + std::to_string (i) + " @ " + std::to_string (config.bpm) + " BPM / " +
                    std::to_string (config.sampleRate) + " Hz / stepPpq " + std::to_string (config.stepPpq) +
                    " / block " + std::to_string (config.blockSize) + ": note-off at " + std::to_string (off) +
                    " but the next note-on is at " + std::to_string (nextOn) + " (expected " +
                    std::to_string (off + 1) + ")");
        }
    }
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

    // ── THE PHASE 7.2 COINCIDENCE SWEEP'S AXES ───────────────────────────────
    // Every tempo it derives must be inside the supported range. A clamped tempo
    // would move the boundary off the block edge, and the whole point of that sweep
    // is that the boundary IS on the edge — it would go green while covering nothing.
    int coincidenceInRange = 0;
    int coincidenceConfigs = 0;

    for (const double stepPpq : sweptStepPpq)
        for (const double sampleRate : sweptSampleRate)
            for (const int blockSize : sweptBlockSize)
            {
                const auto blocksPerStep = coincidenceBlocksPerStep (stepPpq, sampleRate, blockSize);

                for (const double offsetSteps : engineeredEdgeOffsetSteps)
                {
                    ++coincidenceConfigs;

                    const double bpm = coincidenceBpm (stepPpq, sampleRate, blockSize, blocksPerStep, offsetSteps);

                    if (bpm >= Transport::minBpm && bpm <= Transport::maxBpm)
                        ++coincidenceInRange;
                }
            }

    REQUIRE (coincidenceConfigs == numSweptGrids * numSweptRates * numSweptBlockSizes * numEdgeOffsets);
    REQUIRE (coincidenceConfigs == 1080);
    REQUIRE (coincidenceInRange == coincidenceConfigs);

    // THREE of the nine offsets sit INSIDE the tolerance window (magnitude below
    // `stepIndexSnapSteps`, on the low side) — the family that makes the sweep able
    // to detect a divergence at all. Asserted so an edit to the offset list cannot
    // quietly empty it.
    int insideWindowOffsets = 0;

    for (const double offsetSteps : engineeredEdgeOffsetSteps)
        if (offsetSteps < 0.0 && -offsetSteps < stepIndexSnapStepsMirror)
            ++insideWindowOffsets;

    REQUIRE (insideWindowOffsets == 3);
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

TEST_CASE ("midi-conformance/boundary-agreement: ownsPpq is the step-index range, re-expressed (Phase 7.2 stage 2)",
           "[unit][midi-conformance]")
{
    // WHAT THIS PINS: the step walk's ownership test moved from the INDEX to the
    // PLACED PPQ, and the swap is behaviour-neutral only while the two are the same
    // predicate. See the extended note above `mirroredOwnsPpq` for why both sides are
    // mirrored here and why that is the right shape for an equivalence.

    SECTION ("with a step boundary dialled onto a block edge and either side of the shared tolerance")
    {
        // THE SECTION THE PROPERTY ACTUALLY RESTS ON. Everything else in this case is
        // breadth; this is the only geometry in which the index formulation and the
        // PPQ formulation can possibly return different answers, and it is reached by
        // construction rather than by hoping a broad sweep wanders into it.
        OwnershipObservation observation;
        std::int64_t configs = 0;

        for (const double stepPpq : sweptStepPpq)
            for (const double sampleRate : sweptSampleRate)
                for (const int blockSize : sweptBlockSize)
                {
                    const auto blocksPerStep = coincidenceBlocksPerStep (stepPpq, sampleRate, blockSize);

                    for (const double offsetSteps : engineeredEdgeOffsetSteps)
                    {
                        ++configs;

                        const double bpm = coincidenceBpm (stepPpq, sampleRate, blockSize, blocksPerStep, offsetSteps);

                        sweepOwnershipEquivalence (BoundaryConfig { bpm, sampleRate, stepPpq, blockSize, 0 },
                                                   coincidenceBlockCount (blocksPerStep),
                                                   observation);
                    }
                }

        INFO (observation.describe ());

        // THE PROPERTY, on the only boundaries able to break it.
        REQUIRE (observation.mismatches == 0);

        // ANTI-VACUITY: the sweep's own size. 12 grids x 2 rates x 5 buffer sizes x 9
        // offsets.
        REQUIRE (observation.configs == configs);
        REQUIRE (observation.configs == numSweptGrids * numSweptRates * numSweptBlockSizes * numEdgeOffsets);
        REQUIRE (observation.configs == 1080);

        // REACHABILITY, per outcome. An equivalence over a range where one side never
        // fires is satisfied by `return true;`.
        REQUIRE (observation.owned > 0);
        REQUIRE (observation.rejectedLow > 0);
        REQUIRE (observation.rejectedHigh > 0);

        // THE WIDENED BAND IS WHERE THE STAGE-2 SCAN NOW LOOKS, so the equivalence is
        // checked over the indices the walk VISITS, not merely the ones it used to
        // own. `stepScanBack == 2` contributes exactly two per block.
        REQUIRE (observation.widenedBelow == observation.blocks * mirroredScanBack);
        REQUIRE (observation.widenedAbove >= observation.blocks * mirroredScanForward);

        // THE REACHABILITY THAT MATTERS, AND THE ONE THIS FILE PREVIOUSLY LACKED.
        // Boundaries INSIDE the shared tolerance window, at both block edges. Without
        // these the section is green by construction: outside the window the two
        // formulations are separated by ~1e-6 steps against ~1e-13 of rounding noise
        // and cannot differ, so a divergence introduced into either one would go
        // unnoticed. Verified fails-without by deleting the upper tolerance from
        // `mirroredOwnsPpq` — red here, green everywhere else in the suite.
        REQUIRE (observation.insideEndWindow > 0);
        REQUIRE (observation.insideStartWindow > 0);
    }

    SECTION ("across every supported grid, tempo, sample rate and buffer size")
    {
        // BREADTH, and it is honest about being only that. These are the product's
        // real parameter combinations rather than engineered ones, so they exercise
        // the equivalence over ordinary geometry — but see the two `== 0` assertions
        // at the end: none of them lands inside the tolerance window, so this section
        // could not detect a divergence on its own and is not asked to.
        OwnershipObservation observation;
        std::int64_t configIndex = 0;

        for (const double stepPpq : sweptStepPpq)
            for (const double bpm : sweptBpm)
                for (const double sampleRate : sweptSampleRate)
                    for (const int blockSize : sweptBlockSize)
                    {
                        const auto origin = sweptFirstIndex[static_cast<std::size_t> (configIndex % 3)]; // NOLINT
                        ++configIndex;

                        sweepOwnershipEquivalence (BoundaryConfig { bpm, sampleRate, stepPpq, blockSize, origin },
                                                   equivalenceBlocksPerConfig,
                                                   observation);
                    }

        INFO (observation.describe ());

        // THE PROPERTY.
        REQUIRE (observation.mismatches == 0);

        // ANTI-VACUITY: the sweep's own size, from first principles.
        REQUIRE (observation.configs == broadSweepConfigs);
        REQUIRE (observation.blocks == broadSweepConfigs * equivalenceBlocksPerConfig);
        REQUIRE (observation.blocks == 14400);

        REQUIRE (observation.owned > 0);
        REQUIRE (observation.rejectedLow > 0);
        REQUIRE (observation.rejectedHigh > 0);

        REQUIRE (observation.widenedBelow == observation.blocks * mirroredScanBack);
        REQUIRE (observation.widenedAbove >= observation.blocks * mirroredScanForward);

        // STATED, NOT ASSUMED: this sweep reaches no boundary inside the tolerance
        // window. MEASURED before the section above existed — 0 of 600 configurations
        // put a step boundary on a block edge within 24 blocks — which is exactly why
        // the engineered section is the one carrying the property. If a future edit
        // makes these non-zero, that is a coverage IMPROVEMENT and the assertions
        // become `> 0`; it must not be discovered by them silently passing.
        REQUIRE (observation.insideEndWindow == 0);
        REQUIRE (observation.insideStartWindow == 0);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ISSUE #75's WITNESS: the #37 window, RENDERED, with the divergence pinned
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("midi-conformance/boundary-agreement: an event inside the #37 window diverges by exactly one sample "
           "(issue #75)",
           "[midi-conformance][determinism]")
{
    // ── THE GEOMETRY IS A PRECONDITION, NOT AN ASSUMPTION ────────────────────
    // Everything below is meaningless unless the child really does land INSIDE the
    // window and OUTSIDE the sample snap's reach. Both are checked from the numbers,
    // and the window is recomputed here from `stepIndexSnapSteps` x `samplesPerStep`
    // rather than quoted, because the window scales with the step (that is the whole
    // point of the formula in SequencerProcessor.h).
    constexpr double stepPpq = 0.25;
    constexpr double sampleRate = 48000.0;

    const double bpm = windowWitnessBpm (stepPpq, sampleRate);
    const double samplesPerStep = stepPpq * (60.0 / bpm) * sampleRate;
    const double windowSamples = stepIndexSnapStepsMirror * samplesPerStep;

    INFO ("bpm " << bpm << ", samplesPerStep " << samplesPerStep << ", window " << windowSamples << " samples, offset "
                 << windowInsideOffsetSamples);

    REQUIRE (bpm >= Transport::minBpm);
    REQUIRE (bpm <= Transport::maxBpm);

    // INSIDE the window…
    REQUIRE (windowInsideOffsetSamples < windowSamples);
    // …and far enough above the sample snap that the snap cannot absorb it, so the
    // LOWER CLAMP is what decides. Without this the case would pass for the wrong
    // reason (both carvings agreeing because nothing was ever negative).
    REQUIRE (windowInsideOffsetSamples > 10.0 * sampleOffsetSnapSamplesMirror);

    // …and the two carvings really are a head and a non-head.
    REQUIRE (static_cast<std::int64_t> (windowEdgeSample) % windowHeadBlockSize == 0);
    REQUIRE (static_cast<std::int64_t> (windowEdgeSample) % windowMidBlockSize != 0);

    // ── THE TWO RENDERS ──────────────────────────────────────────────────────
    const auto atHead = renderWindowWitness (windowHeadBlockSize, stepPpq, sampleRate);
    const auto atMidBlock = renderWindowWitness (windowMidBlockSize, stepPpq, sampleRate);

    INFO ("block " << windowHeadBlockSize << ": child 1 at " << atHead.childOnset << " (edge is a head: "
                   << atHead.edgeIsBlockHead << ", " << atHead.noteOns << " note-ons)\nblock " << windowMidBlockSize
                   << ": child 1 at " << atMidBlock.childOnset << " (edge is a head: " << atMidBlock.edgeIsBlockHead
                   << ", " << atMidBlock.noteOns << " note-ons)\ntrue position floors to " << atHead.exactPosition);

    // Anti-vacuity: both renders actually produced the child, and produced the SAME
    // number of notes (they cover the same absolute span — see `witnessSpan`), so the
    // comparison below is between two placements of one event rather than between two
    // different performances.
    REQUIRE (atHead.noteOns >= 2);
    REQUIRE (atMidBlock.noteOns == atHead.noteOns);
    REQUIRE (atHead.childOnset >= 0);
    REQUIRE (atMidBlock.childOnset >= 0);
    REQUIRE (atHead.edgeIsBlockHead);
    REQUIRE (! atMidBlock.edgeIsBlockHead);

    // ── THE DIVERGENCE, PINNED AS AN EXACT VALUE ─────────────────────────────
    // The mid-block carving places the child exactly: floor (12287.994) = 12287. The
    // head carving claims it for the block starting at 12288, gets a raw offset of
    // -0.006, and the surviving lower clamp emits it at offset 0 — 12288.
    //
    // ONE SAMPLE. That is the documented bound of "THE SNAP-BOUNDARY WINDOW" turned
    // into a measurement. Before this case the bound was an argument about code and
    // nothing had ever rendered from inside the window (issue #75).
    REQUIRE (atMidBlock.childOnset == static_cast<std::int64_t> (windowEdgeSample) - 1);
    REQUIRE (atHead.childOnset == static_cast<std::int64_t> (windowEdgeSample));
    REQUIRE (atHead.childOnset - atMidBlock.childOnset == 1);

    // …and it is bounded AT one sample: the window is under one sample wide at every
    // supported configuration, so the effect cannot compound. Re-derived here against
    // the coarsest supported grid at the slowest tempo and highest rate — the worst
    // supported case in the header's table (0.864 samples).
    const double worstWindow = stepIndexSnapStepsMirror * (1.5 * (60.0 / Transport::minBpm) * 192000.0);
    INFO ("worst supported window: " << worstWindow << " samples");
    REQUIRE (worstWindow < 1.0);
    REQUIRE (worstWindow > 0.8); // …and only just, which is the point of quoting it
}

// ─────────────────────────────────────────────────────────────────────────────
// THE 8-CHILD CHAIN: the two derivations, at eight times the density
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("midi-conformance/boundary-agreement: an eight-child ratchet chain agrees on every 1-sample gap",
           "[midi-conformance][determinism]")
{
    // ── WHY THIS RE-RUN EXISTS ───────────────────────────────────────────────
    // The `noteOff[n] == noteOn[n + 1] - 1` relation is this file's whole instrument:
    // the note-ON is the WALK's placement of a position and the note-OFF one sample
    // earlier is `cutoffForSamePitch`'s PREDICTION of the same position, so a broken
    // pair is a divergence between two writings of one derivation (issue #54).
    //
    // Phase 7.2 multiplied the number of those pairs by EIGHT and changed where they
    // come from: within a step, child c's off is decided by child c+1's onset — the
    // scan's `ahead == 0` band, which did not exist before. This is the densest place
    // the two derivations have to agree, and it is deliberately run over the same
    // grid x tempo x rate x buffer-size axes as the broad sweep rather than at one
    // configuration.
    ChainObservation observation;
    std::int64_t configIndex = 0;

    for (const double stepPpq : sweptStepPpq)
        for (const double bpm : sweptBpm)
            for (const double sampleRate : sweptSampleRate)
                for (const int blockSize : sweptBlockSize)
                {
                    const auto origin = sweptFirstIndex[static_cast<std::size_t> (configIndex % 3)]; // NOLINT
                    ++configIndex;

                    sweepChildChain (BoundaryConfig { bpm, sampleRate, stepPpq, blockSize, origin }, observation);
                }

    INFO (observation.describe ());

    // THE PROPERTY.
    REQUIRE (observation.mismatches == 0);

    // ANTI-VACUITY, from first principles: every configuration must have supplied its
    // full chain. A short render would silently reduce the pair count, and two empty
    // chains agree perfectly.
    REQUIRE (observation.configs == broadSweepConfigs);
    REQUIRE (observation.shortRenders == 0);
    REQUIRE (observation.pairs == broadSweepConfigs * (2 * chainRatchetChildren - 1));
    REQUIRE (observation.pairs == 9000);

    // …and the chain really is EIGHT deep, not one: `chainRatchetChildren` is what
    // makes fifteen of the sixteen pairs intra-step rather than one-per-step.
    REQUIRE (chainRatchetChildren == 8);
    REQUIRE (chainRatchetChildren == arpbox::engine::maxRatchetChildren);
}
