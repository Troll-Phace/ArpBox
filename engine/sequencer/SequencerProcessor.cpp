#include "SequencerProcessor.h"

#include "../midi/NotePool.h"

#include <cmath>
#include <cstddef>
#include <limits>

namespace arpbox::engine
{
namespace
{
    // Reserved capacity (bytes) for the graph-owned OUTGOING MidiBuffer, ensured
    // unconditionally every block (ensureSize early-returns once satisfied). Sized for
    // this node's deterministic worst case, which PHASE 6 DOUBLED: a full-table flush
    // is SoundingNoteTable::capacity note-offs plus up to 16 CC123 messages, and
    // juce::MidiBuffer stores a 3-byte event as 4 (position) + 2 (length) + 3 = 9
    // bytes → 272 * 9 ≈ 2.4 KB per flush. TWO flushes are now reachable in ONE block —
    // a block-head discontinuity flush plus a mid-block pattern-switch flush — so the
    // worst case is ~4.8 KB, and 4096 would have been exceeded. Exceeding the
    // reservation grows juce::MidiBuffer's backing array, i.e. ALLOCATES ON THE AUDIO
    // THREAD (tests/infra_alloc_guard.cpp catches it). 8 KB covers both flushes and
    // leaves room for the same block's note-ons and the upstream MIDI-in traffic
    // already in the buffer.
    //
    // UNCONDITIONAL on purpose, exactly as MidiInputProcessor does it: the graph can
    // hand this node a FRESH, small pool buffer after a render-sequence rebuild (a
    // synth swap, or this node's own async insertion), and re-checking every block
    // re-warms that buffer — a first-block-only flag would miss it.
    //
    // ── PHASE 7.2 DID NOT RAISE IT, AND THAT IS A DECISION, NOT AN OVERSIGHT ──
    // Ratchets multiply this node's note events by up to 8. The worst additional
    // traffic in one block is bounded by how many steps a block can contain: at the
    // tightest supported step (735 samples) a 4096-sample block holds ~6 steps, each
    // up to 8 children, each an on plus an off — 6 x 8 x 2 x 9 bytes ~= 864 bytes on
    // top of the ~4.8 KB two-flush worst case this number was sized for. It fits with
    // ~2.5 KB to spare.
    //
    // THAT MARGIN IS ARGUED, NOT PROVED, so the constant is deliberately left alone:
    // raising it speculatively would make the argument unfalsifiable. The armed
    // ratchet scenario in tests/pattern_alloc_guard.cpp is what turns it into a
    // proof — if that goes red, 16384 is the answer, not a re-derivation of this
    // paragraph.
    constexpr int outgoingWarmupBytes = 8192;

    // ── DETERMINISM SNAPS (see the extended note in SequencerProcessor.h) ─────

    // Step-index snap, in units of one STEP. Absorbs the ulp-level disagreement
    // between block k's `blockEndPpq()` and block k+1's `blockStartPpq()`, which are
    // equal only in exact arithmetic.
    //
    // IT IS IN STEPS, SO ITS WIDTH IN SAMPLES SCALES WITH THE GRID — quote it as
    // `1e-6 x stepPpq x 60 x sampleRate / bpm`, never as one number:
    //   0.006 samples  at the 16th grid, 120 BPM, 48 kHz (the scaffold)
    //   0.144 samples  at the 16th grid, 20 BPM, 192 kHz
    //   0.864 samples  at a DOTTED QUARTER (stepPpq 1.5 — the coarsest grid §2.1
    //                  allows), 20 BPM, 192 kHz: the worst supported case
    // So: always thousands of times larger than the ~1e-10-step error it absorbs, and
    // always under one sample — but at the coarse end by only ~0.14 samples (~1.16x
    // inside it), where the scaffold grid sits ~167x inside it. That margin is the
    // whole safety story. See "THE SNAP-BOUNDARY WINDOW" in the header
    // for what that residue costs (issue #37) and for the re-derivation any future
    // grid change owes.
    constexpr double stepIndexSnapSteps = 1.0e-6;

    // Sample-offset snap, in SAMPLES. `blockOffsetForPpq` subtracts two PPQ values
    // whose absolute magnitude grows with the timeline, so an offset that is
    // mathematically an exact integer can arrive as `integer - 1e-7`. Flooring that
    // would place the event one sample early, and only at some buffer sizes. 1e-4
    // samples (~2 ns at 48 kHz) is far above the residual error and far below one
    // sample, so the snap can never move an event to a different sample.
    //
    // CONTRAST WITH stepIndexSnapSteps: this one is denominated in SAMPLES, so its
    // margin is grid-independent — 1e-4 samples is 1e-4 samples at every grid, tempo
    // and sample rate, and a coarser grid does not erode it. Only snap 1 needs the
    // per-grid re-derivation.
    constexpr double sampleOffsetSnapSamples = 1.0e-4;

    // RT-SAFE: how many whole `grid` units fit at or after `x`, snapped — i.e.
    // `ceil (x / grid)` with `stepIndexSnapSteps` of tolerance so a value sitting an
    // ulp above an exact multiple is not pushed to the NEXT multiple.
    //
    // THE ONLY CEILING IN THIS FILE. Both callers below go through it: the step walk
    // (grid = one step) and the pattern-switch quantizer (grid = one beat / bar /
    // pattern loop). A second, subtly different ceiling is how a quantized switch
    // would land one boundary away from where the walk expects it.
    double snappedCeiling (double x, double grid) noexcept
    {
        return std::ceil (x / grid - stepIndexSnapSteps);
    }

    // RT-SAFE: index of the first step boundary at or after `ppq`, snapped. Applied to
    // BOTH ends of the half-open block interval so consecutive blocks agree exactly.
    std::int64_t snappedStepCeiling (double ppq, double stepPpq) noexcept
    {
        return static_cast<std::int64_t> (snappedCeiling (ppq, stepPpq));
    }

    // RT-SAFE: THE ONLY OWNERSHIP TEST IN THIS FILE.
    //
    // Does THIS block emit an event whose musical position is `ppq`? Until Phase 7.2
    // the walk answered a narrower question — "is the step INDEX inside this block's
    // index range" — which is only the same question while an event sits exactly on
    // its grid position. Once MICRO, swing and ratchets displace it, the index says
    // nothing about which block contains the event, and the old formulation is wrong
    // in BOTH directions: a positively displaced event belongs to a later block, a
    // negatively displaced one to an earlier block.
    //
    // ── THE SAME TOLERANCE ON BOTH ENDS, AND WHAT THAT DOES AND DOES NOT BUY ─
    // `snapPpq` is ONE expression — the same `stepIndexSnapSteps`, against the same
    // `stepPpq` — subtracted from BOTH `blockStartPpq()` and `blockEndPpq()`. Be
    // precise about what that achieves, because an earlier revision of this note
    // claimed more than the code earns (issue #82).
    //
    // WHAT HOLDS. Block k's upper decision boundary is `blockEndPpq()_k - snapPpq` and
    // block k+1's lower one is `blockStartPpq()_{k+1} - snapPpq`. The two are the SAME
    // EXPRESSION, so they cannot drift as CODE — no edit can give one end a tolerance
    // the other lacks — and their inputs are MATHEMATICALLY EQUAL, so the half-open
    // spans tile the timeline. That is what makes ownership well defined and is the
    // property the rest of this file relies on.
    //
    // WHAT DOES *NOT* HOLD: THE TILING IS NOT EXACT IN FLOATING POINT. Those two
    // inputs are computed differently — `ppq_k + advance * pps` against
    // `anchorPpq + (s_k + advance - anchor) * pps` — and agree only to within their
    // rounding, measured at ~1e-10 steps (a few ulps at realistic PPQ magnitudes).
    // SUBTRACTING THE SAME CONSTANT FROM BOTH DOES NOT MAKE THEM BITWISE EQUAL. The
    // snap's WIDTH is irrelevant to that: "the snap is ~7 orders of magnitude wider
    // than the disagreement" — the old justification — confuses the width of the
    // tolerance with the POSITION of the residual. A gap/overlap of that ~1e-10-step
    // width survives, RELOCATED from the block edge to `edge - snapPpq`, and an event
    // landing inside it is owned by zero blocks (DROPPED) or by two (DUPLICATED).
    //
    // WHY THE DESIGN IS SOUND ANYWAY, AND THIS IS THE ACTUAL ARGUMENT: the snap moves
    // the sensitive point OFF THE ATTRACTOR. Musical positions are rational multiples
    // of `stepPpq` and therefore pile up ON block edges — MEASURED over 1.08e8 edges,
    // the two expressions disagree bitwise at 24.2 % of them and 27123 of those
    // disagreements straddle a step boundary, i.e. duplicate or skip a step. Nothing
    // in the system systematically sits exactly `1e-6 * stepPpq` BELOW an edge. So the
    // snap converts a systematic, high-rate, measured failure into an off-attractor
    // coincidence of measure zero.
    //
    // WHAT A FUTURE READER MAY RELY ON, stated so it is usable: ownership is exact for
    // every event whose PPQ is not within a few ulps of `edge - snapPpq`, which is
    // every event any test or any piece of music has ever produced. It is NOT exact in
    // the absolute sense, so do not build a proof on top of "exactly one block owns
    // every event" — build it on "at most one, and exactly one off a measure-zero set".
    // Closing the residual properly is issue #37 option (a) (derive the emission
    // sample as a pure function of the boundary index against the segment anchor); this
    // is the same residual family as #37 and #75, not a separate hazard.
    //
    // THE ONE CONSEQUENCE THAT IS NOT MERELY A SAMPLE: if the ADOPT STEP's `gridPpq`
    // ever landed in the gap, no block would pass `ownsPpq` for it, the quantized
    // switch would never fire, `pendingResolved` would stay set — and since issue #76
    // `discontinuedByPatternSwitch` would go on suppressing that step's late children
    // indefinitely. Measure-zero like the rest, but a liveness effect rather than a
    // one-sample blemish, so it is the one worth naming.
    //
    // A SECOND TOLERANCE HERE IS STILL FORBIDDEN, and the sharper reason is now
    // visible: a different constant, a different `stepPpq`, or a per-child sub-grid
    // snap would put the two ends' residuals at DIFFERENT positions, which turns this
    // measure-zero coincidence into a systematic disagreement — a gap or overlap of
    // the tolerance's full width rather than of a few ulps. Neither shows up as a
    // cross-buffer-size difference, because it happens identically at every size.
    //
    // NOT A RIVAL TO `snappedCeiling`: this is the SAME ceiling re-expressed. For
    // integer `n`, `n >= snappedCeiling (blockStartPpq, stepPpq)` and
    // `n * stepPpq >= blockStartPpq - snapPpq` are the same predicate, and likewise at
    // the upper end. tests/boundary_agreement.cpp asserts that equivalence directly
    // over its 9600-boundary sweep rather than leaving it as this paragraph.
    bool ownsPpq (const Transport& transport, double ppq, double stepPpq) noexcept
    {
        const double snapPpq = stepIndexSnapSteps * stepPpq; // SAME constant, SAME stepPpq, BOTH ends

        return ppq >= transport.blockStartPpq () - snapPpq && ppq < transport.blockEndPpq () - snapPpq;
    }

    // RT-SAFE: does an event belonging to step `index` reach the adopt boundary of a
    // pattern switch resolved at `boundaryIndex`?
    //
    // TRUE means the event belongs to a step the OUTGOING pattern owns but SOUNDS at
    // or after the point that pattern is discontinued (§5.5; `flushForPatternSwitch`:
    // "THE OUTGOING PATTERN IS DISCONTINUED FROM `adoptSample` ONWARD"). Only ratchet
    // children can be in that position: a step's own displacement is capped at ±0.5,
    // so step `B - 1` cannot reach step `B`'s boundary on displacement alone, but a
    // child sits a further 7/8 of a step ahead and 0.5 + 0.875 = 1.375 clears it by
    // 0.375 of a step.
    //
    // THE INDEX TEST IS FIRST AND THAT ORDERING IS LOAD-BEARING: `boundaryIndex` is
    // `INT64_MIN` when no switch has fired, and short-circuiting here keeps
    // `boundaryIndex * stepPpq` — which would be a meaningless huge negative double —
    // from being evaluated at all.
    //
    // THE SAME SNAP, THE SAME `stepPpq`, ONE MORE TIME. `eventPpq` is built as
    // `index * stepPpq + shift * stepPpq + position * stepPpq` while the boundary is
    // `boundaryIndex * stepPpq`, so on a non-dyadic grid (a triplet: stepPpq = 1/6)
    // an event landing exactly ON the boundary can arrive an ulp below it. Subtracting
    // `stepIndexSnapSteps * stepPpq` makes such an event count as AT the boundary,
    // which is the same direction `snappedCeiling` and `ownsPpq` already round (the
    // later side claims a coincident position). This is NOT a second tolerance in the
    // sense `ownsPpq` warns about: no block quantity appears here, so the answer is
    // identical in every carving and cannot make two blocks disagree about ownership.
    bool
    eventReachesAdoptBoundary (std::int64_t index, double eventPpq, std::int64_t boundaryIndex, double stepPpq) noexcept
    {
        if (index >= boundaryIndex)
            return false;

        const double snapPpq = stepIndexSnapSteps * stepPpq;

        return eventPpq >= static_cast<double> (boundaryIndex) * stepPpq - snapPpq;
    }

    // ── THE SCAN WIDENING, DERIVED (Phase 7.2) ───────────────────────────────
    // Because ownership is now decided on the PLACED position, the walk must VISIT
    // every step index that could place an event inside this block — which is no
    // longer just the indices whose grid position falls in it.
    //
    // An event placed by step `n` sits at `n + shift + child` steps, with
    // `shift` in [-maxSubStepShiftSteps, +maxSubStepShiftSteps] and `child` in
    // [0, maxChildAheadSteps]. Writing the block's owned span as [S, E) in steps:
    //
    //   n can reach forward into the block  ⟹  n + maxSubStepShiftSteps
    //                                            + maxChildAheadSteps >= S
    //                                       ⟹  n >= S - 1.375  ⟹  n >= ceil(S) - 2
    //   n can reach backward into the block ⟹  n - maxSubStepShiftSteps < E
    //                                       ⟹  n < E + 0.5     ⟹  n < ceil(E) + 1
    //
    // THIS IS TIGHT, NOT PADDING — BOTH BOUNDS ARE ATTAINED. `scanBack == 2` is
    // reached whenever `frac(S) > 0.375` (e.g. S = 10.2: ceil(8.825) = 9 = 11 - 2),
    // and `scanForward == 1` whenever `frac(E) > 0.5` (e.g. E = 10.7: n = 11 must be
    // visited, and 11 < ceil(10.7) + 1 = 12 only because of the +1). Someone will
    // eventually want to relax `maxSubStepShiftSteps` past 0.5; this note is what
    // stops them doing it without re-deriving these two numbers. Widening the shift
    // without widening the scan drops notes at block edges UNIFORMLY AT EVERY BUFFER
    // SIZE, so no cross-size determinism test can see it — only literal event counts
    // can (tests/substep_ownership.cpp).
    // `std::ceil` is not guaranteed constant-evaluable in C++20, and these must be
    // `constexpr` so the static_asserts below actually check the derivation at build
    // time. Non-negative inputs only, which is all this is ever applied to.
    constexpr std::int64_t ceilNonNegative (double x) noexcept
    {
        const auto truncated = static_cast<std::int64_t> (x);

        return static_cast<double> (truncated) < x ? truncated + 1 : truncated;
    }

    constexpr std::int64_t stepScanBack = ceilNonNegative (maxSubStepShiftSteps + maxChildAheadSteps); // == 2
    constexpr std::int64_t stepScanForward = ceilNonNegative (maxSubStepShiftSteps);                   // == 1

    static_assert (stepScanBack == 2, "scan widening must be re-derived if the sub-step geometry changes");
    static_assert (stepScanForward == 1, "scan widening must be re-derived if the sub-step geometry changes");

    // `laneOf (const PatternData&, LaneId)` USED TO LIVE HERE. Phase 7.1 moved it to
    // namespace scope in PatternSnapshot.h because StepLogic.cpp needs the identical
    // accessor; see the note there.
} // namespace

SequencerProcessor::SequencerProcessor ()
    : juce::AudioProcessor (BusesProperties ()) // MIDI-only: no audio buses
{
}

// MESSAGE-THREAD ONLY:
SequencerProcessor::~SequencerProcessor ()
{
    // Hand the held snapshot back for message-thread deletion. THIS IS THE ONLY PLACE
    // the node lets go of it, and the reasoning has two halves:
    //
    //   WHY HERE. `PatternChannel`'s destructor reclaims its retirement queue and
    //   deletes any never-adopted pending snapshot, but deliberately does NOT touch
    //   the audio thread's held pointer — this node owns that one. `EngineGraph`
    //   declares the channel BEFORE `graph`, so the channel outlives this node and
    //   this `retire()` lands in a live queue, which the channel's own destructor
    //   then drains. Calling a "RT-safe, audio-thread" method from the message thread
    //   is legal here for the usual reason: audio is stopped and this object is being
    //   destroyed, so there is exactly one thread touching the SPSC producer side.
    //
    //   WHY NOT IN `releaseResources()`. That is the obvious-looking home and it is
    //   WRONG: `releaseResources`/`prepareToPlay` are a matched pair the graph runs on
    //   every device change, and nothing republishes a snapshot in between — so
    //   retiring there would leave `activeSnapshot` permanently null and the node
    //   permanently SILENT after the user switches audio device. Holding a ~120 KB
    //   immutable object across a released period costs nothing and has no such hole.
    if (patternChannel != nullptr && activeSnapshot != nullptr)
        patternChannel->retire (activeSnapshot);

    activeSnapshot = nullptr;
}

// MESSAGE-THREAD ONLY:
void SequencerProcessor::setSharedState (const Transport* transportToFollow, PatternChannel* channelToFollow) noexcept
{
    transport = transportToFollow;
    patternChannel = channelToFollow;
}

// RT-SAFE:
void SequencerProcessor::applyCommand (const EngineCommand& command) noexcept
{
    // FAN-OUT CONTRACT (ICommandSink.h): every sink sees every command, so everything
    // this node does not own MUST fall through untouched.
    switch (command.type)
    {
    case EngineCommandType::queuePatternSwitch:
    {
        // RECORD ONLY — no arithmetic. This runs during the transport head node's
        // drain, which is strictly BEFORE `Transport::beginBlock()`, so every latched
        // getter here still describes the PREVIOUS block. Resolving the quantize
        // boundary against stale values would place the switch one block early.
        const int index = static_cast<int> (command.targetId);

        // Defensive rejection rather than clamping (the same posture as
        // `Transport::applyCommand` after issue #3): a malformed command must not
        // silently switch to a pattern the caller did not ask for.
        if (index < 0 || index >= maxPatterns)
            return;

        if (command.value.u > static_cast<std::uint32_t> (QuantizeMode::patternEnd))
            return;

        pendingRequested = true;
        pendingPatternIndex = index;
        pendingQuantize = static_cast<QuantizeMode> (command.value.u);
        pendingResolved = false; // resolved at the top of the next processBlock
        break;
    }

    case EngineCommandType::setFillHeld:
        // §12.2 FILL — pad 16 held. UNLIKE the pattern switch above there is no
        // arithmetic to defer: this is a raw flag with no quantize boundary and no
        // dependence on any latched transport value, so recording it here is the
        // whole of the work. `processBlock` latches it once per block (see the latch
        // site) and every step in the block sees the same value.
        //
        // ANY NON-ZERO READS AS HELD, per the command's documented payload contract
        // in EngineCommand.h — no rejection path, because there is no malformed
        // value a bool can take.
        fillHeld = command.value.i != 0;
        break;

    // ── NOT OURS — the fan-out no-op arm (ICommandSink.h dispatch contract) ─────
    // Explicit enumeration with no `default:`, for the reason spelled out at the
    // twin arm in Transport::applyCommand: runtime-identical to the `default:
    // break;` it replaced, but a newly added `EngineCommandType` becomes a compile
    // diagnostic here (`-Wswitch-enum` + `-Wswitch`) that `lint.sh warnings` fails
    // the build on, instead of being silently swallowed (#70, #79).
    case EngineCommandType::none:
    case EngineCommandType::setMasterGainDb:
    case EngineCommandType::setLimiterEnabled:
    case EngineCommandType::setTestToneEnabled:
    case EngineCommandType::setTestToneFrequency:
    case EngineCommandType::transportPlay:
    case EngineCommandType::transportStop:
    case EngineCommandType::transportLocate:
    case EngineCommandType::setTempoBpm:
        break;
    }
}

// MESSAGE-THREAD ONLY:
void SequencerProcessor::prepareToPlay (double, int)
{
    // Audio is stopped and there is no MidiBuffer to emit into, so a silent discard is
    // the only option here — but it is also the correct one: the graph is (re)starting,
    // and the synth this node feeds is itself being re-prepared (or replaced), so
    // nothing downstream is still holding those voices.
    sounding.reset ();

    // Re-seed on the next block rather than adopting a possibly stale watermark.
    stopGenerationSeeded = false;

    // The REQUEST survives a re-prepare; its resolution does not. `adoptStepIndex` is
    // anchored to a sample/PPQ timeline the graph is about to rebuild.
    pendingResolved = false;
}

// MESSAGE-THREAD ONLY:
void SequencerProcessor::releaseResources ()
{
    sounding.reset ();
    stopGenerationSeeded = false;
    pendingResolved = false;

    // `activeSnapshot` is deliberately RETAINED — see the destructor.
}

// RT-SAFE:
StepEmission
evaluateStep (const PatternSnapshot& snapshot, int patternIndex, std::int64_t stepIndex, StepRuntime runtime) noexcept
{
    // ── L0 NOTE POOL → L1 PATTERN CORE → L2 STEP LOGIC (§5.1) ────────────────
    // A PURE function of the four parameters — no cursor, no accumulator, and (by
    // construction, being a free function) no access to any `SequencerProcessor`
    // member at all. That is what makes §9's offline drag-out render bit-identical
    // to real time, what lets the transport be located anywhere without the
    // arpeggio shifting, and what makes `cutoffForSamePitch`'s lookahead sound.
    // See "THE PURITY OF THE EMISSION CORE IS STRUCTURAL" in the header — and its
    // Phase 7.1 amendment, which argues the fourth parameter — before adding
    // anything here that is not derived from these four arguments.
    //
    // LANES READ HERE: GATE, PITCH, OCT, VEL, LEN (Phase 6); PROB, COND (Phase
    // 7.1); MOD A, read by the `NEI`/`!NEI` conditions (Phase 7.1, user decision
    // D7 — see `neighbourModThreshold` in StepLogic.h); MICRO and RATCHET (Phase
    // 7.2 — MICRO composed with the snapshot's project-level `swingPct` into
    // `shiftSteps`, RATCHET into the note list, with PROB re-read per child).
    // TEN OF THE ELEVEN §12.1 LANES ARE NOW LIVE. The one that is not:
    //   MOD B → Phase 14.1 (mod-matrix per-step source)
    // The omission is deliberate, not an oversight — see LaneId in PatternTypes.h,
    // which carries the same per-lane phase annotations.

    const PatternData& data = snapshot.pattern (patternIndex);

    // GATE: a true clock divider (`isLaneTick`) AND a non-zero held value. This is the
    // SAME predicate `gatePrefixPulses` was summed from, so the fired steps and the
    // pool ordinal cannot drift apart.
    if (! PatternSnapshot::isGated (data, stepIndex))
        return {};

    // ── L2 STEP LOGIC (§5.1 L2, §12.2) ───────────────────────────────────────
    // BOTH CHECKS SIT AFTER THE GATE CHECK AND BEFORE `poolIndexAt`, and that
    // placement is the whole of the interaction between L2 and L1. A step
    // suppressed here has already been counted by the GATE lane's prefix table, so
    // it STILL CONSUMES ITS GATED ORDINAL and does not shift the pool traversal for
    // every step after it. Moving either check below `poolIndexAt` would change
    // nothing; moving the SUPPRESSION into the ordinal — which is the tempting
    // "fix" — would make the arpeggio's pitch sequence a function of RNG draw
    // count, i.e. of block carving. See "PHASE 7, READ THIS BEFORE YOU 'FIX' IT"
    // on `PatternSnapshot::gatedOrdinal`.
    //
    // CONDITIONS GATE BEFORE PROBABILITY ROLLS (§5.1 L2), written in that order
    // here. Worth knowing before "simplifying": with a per-index HASH rather than a
    // stream, the two orders produce identical output today, because neither check
    // consumes anything the other could observe. The order is preserved anyway,
    // because that equivalence stops holding the instant anything downstream of
    // this point becomes stateful, and §5.1 fixes which order is correct.
    if (! conditionPasses (data, stepIndex, runtime))
        return {};

    if (! probabilityPasses (data, stepIndex))
        return {};

    // The pool size is clamped even though `PatternDocument::setPool` already clamps:
    // this dereferences a fixed-size array on the audio thread from data that arrived
    // through a pointer swap, and a guard is cheaper than a corrupted read.
    const int poolSize = juce::jmin (maxPoolSize, static_cast<int> (snapshot.pool.size));

    // Which pool DEGREE this step lands on: the gated ordinal run through the
    // pre-built traversal table for the active direction mode (§12.3).
    const int poolIndex = snapshot.poolIndexAt (data, stepIndex, poolSize);

    if (poolIndex < 0)
        return {}; // empty pool / degenerate traversal — the caller must not emit

    // PITCH is a DEGREE offset, not a semitone offset (§12.1): `poolNoteAtDegree`
    // wraps it through the pool with octave carry, so the pattern keeps working when
    // the held chord changes size. OCT is the straight ±4-octave transpose on top.
    const int degree = poolIndex + laneValueAt (laneOf (data, LaneId::pitch), stepIndex);
    const int poolNote = poolNoteAtDegree (poolNotes (snapshot.pool, data.asPlayedView), poolSize, degree);

    StepEmission emission;
    emission.gate = true;
    emission.channel = snapshot.outputChannel;

    // Neither of these is range-clamped here: `poolNoteAtDegree` deliberately leaves
    // fold-vs-clamp to the constraint gate (Phase 12.3), and `emitNote` applies the
    // hard 0..127 / 1..127 MIDI limits as the last line of defence.
    const int stepNote = poolNote + 12 * laneValueAt (laneOf (data, LaneId::oct), stepIndex);
    const int stepVelocity = laneValueAt (laneOf (data, LaneId::vel), stepIndex);

    // LEN is stored as a PERCENTAGE of the step, 1..400 (§12.1); >100% ⇒ tie/legato.
    const double lenFractionOfStep = static_cast<double> (laneValueAt (laneOf (data, LaneId::len), stepIndex)) / 100.0;

    // ── L2 RATCHETS (§5.1 L2, §12.1 RATCHET 1..8) ────────────────────────────
    // The step becomes a LIST of notes at evenly spaced sub-step positions. Three
    // decisions are written into the loop below and each has a comment on it,
    // because each has a plausible-looking alternative that is wrong.
    const int childCount = ratchetChildCount (data, stepIndex);

    // LEN AGAINST THE CHILD'S OWN SLOT, NOT THE WHOLE STEP. Identical at
    // `childCount == 1` — the default, and every pre-7.2 golden — which is what
    // makes this change provably silent on existing material. See the long note on
    // `StepEmission::noteCount` for why this is the only reading under which LEN >
    // 100 % ties across children.
    const double childGateFraction = lenFractionOfStep / static_cast<double> (childCount);

    for (int child = 0; child < childCount; ++child)
    {
        // PER-CHILD PROBABILITY, A PURE HASH OF (seed, domain, step, child) — never
        // a running stream. `ratchetChildPasses` carries the full argument; the part
        // that matters here is that the retrigger lookahead calls this function for
        // steps it is not emitting, so a stream's pull count would make prediction
        // and emission disagree. Child 0 is never asked: its fate was already
        // decided by the condition and probability checks above.
        if (! ratchetChildPasses (data, stepIndex, child))
            continue;

        // COMPACTED: a suppressed child leaves no hole, so `notes[k]` is the k-th
        // SURVIVOR and `positionInStep` — not the index — says where it sits.
        StepNote& note = emission.notes[static_cast<std::size_t> (emission.noteCount)];

        note.note = stepNote;
        note.velocity = ratchetVelocity (stepVelocity, child, childCount, snapshot.ratchetVelocityRampPct);

        // DERIVED INDEPENDENTLY FROM THE PARENT — `child / childCount`, one division,
        // NOT `previousPosition + 1 / childCount`. Cumulative addition of a rounded
        // slot drifts: at `childCount == 7` the slot is 822.857 samples at the
        // canonical clock and child 6 ends up ~6 samples early.
        note.positionInStep = static_cast<double> (child) / static_cast<double> (childCount);
        note.gateFractionOfStep = childGateFraction;

        // §5.4 provenance. Every note is `core`; only `child > 0` is a ratchet child
        // (child 0 IS the step's onset). Phase 12 ORs its operator-slot bits in here.
        note.provenance = provenance::core | (child > 0 ? provenance::ratchetChild : provenance::none);

        ++emission.noteCount;
    }

    // Child 0 always survives (see above), so `noteCount >= 1` here — but assert it
    // rather than assume it, because a future edit that makes child 0 conditional
    // would otherwise produce a `gate == true` emission with nothing in it, which
    // reads downstream as a step that fired silently rather than as a bug.
    jassert (emission.noteCount >= 1);

    // ── MICRO + SWING: COMPOSED HERE, CLAMPED ONCE, ON THE TOTAL ─────────────
    // §12.1 stores MICRO as a percentage of the step, -50..+50, and says "swing
    // applies on top" — so the composition is ADDITIVE with MICRO first.
    //
    // THE CLAMP IS ON THE SUM, AND CLAMPING PER SOURCE WOULD BE A DIFFERENT (AND
    // WRONG) DESIGN. MICRO alone reaches ±0.5 and swing alone reaches +0.5, so two
    // per-source clamps admit a composed ±1.0 — which BREAKS THE DERIVATION the
    // step walk's scan widening rests on (`stepScanBack`/`stepScanForward` below
    // are computed from `maxSubStepShiftSteps` and both bounds are ATTAINED, not
    // padded). A composed displacement past 0.5 would place events in blocks the
    // walk never visits, so they would vanish — uniformly at every buffer size,
    // which is the one failure no cross-carving determinism test can see.
    //
    // THE SATURATION IS THEREFORE DELIBERATE AND AUDIBLE: at `swingPct 75`, MICRO
    // +50 and MICRO 0 place an odd step on the SAME sample. That is documented
    // behaviour (the golden `micro-swing-compose` bakes a saturating step in on
    // purpose), not a rounding artefact.
    //
    // AND IT IS THE ONLY ENFORCEMENT POINT. `maxSubStepShiftSteps` is checked
    // here, once, inside the pure core — so the walk, the retrigger lookahead and
    // §9's offline pass all see a displacement that is already in range, and none
    // of them has to re-clamp (a second clamp would be a second tolerance, which
    // is the `ownsPpq` hazard one level up).
    emission.shiftSteps =
        juce::jlimit (-maxSubStepShiftSteps,
                      maxSubStepShiftSteps,
                      static_cast<double> (laneValueAt (laneOf (data, LaneId::micro), stepIndex)) / 100.0 +
                          swingShiftSteps (snapshot.swingPct, stepIndex));

    return emission;
}

// RT-SAFE:
std::int64_t SequencerProcessor::sampleForPpq (double ppq, std::int64_t blockStartSample) const noexcept
{
    // SNAP UP, THEN FLOOR — the two operations, written once (see the header). The
    // walk's block clamp is deliberately NOT applied here: this function describes
    // positions OUTSIDE the current block as readily as inside it, and the clamp is a
    // separate decision the walk makes at its own call site (issue #37).
    const double rawOffset = transport->blockOffsetForPpq (ppq);

    return blockStartSample + static_cast<std::int64_t> (std::floor (rawOffset + sampleOffsetSnapSamples));
}

// RT-SAFE:
std::int64_t SequencerProcessor::stepBoundarySample (std::int64_t index,
                                                     double stepPpq,
                                                     std::int64_t blockStartSample) const noexcept
{
    return sampleForPpq (static_cast<double> (index) * stepPpq, blockStartSample);
}

// RT-SAFE:
std::int64_t SequencerProcessor::cutoffForSamePitch (const StepEmission& emission,
                                                     std::int64_t stepIndex,
                                                     int childIndex,
                                                     int channel,
                                                     int note,
                                                     std::int64_t onSample,
                                                     std::int64_t naturalDueSample,
                                                     double stepPpq,
                                                     std::int64_t blockStartSample,
                                                     int lookaheadSteps,
                                                     StepRuntime runtime) const noexcept
{
    if (activeSnapshot == nullptr || transport == nullptr || stepPpq <= 0.0)
        return naturalDueSample;

    // THE MINIMUM QUALIFYING ONSET, not the first one found. Until Phase 7.2 this
    // loop returned on its first match and broke out at the first boundary past the
    // note's end, both of which are only valid while onsets INCREASE MONOTONICALLY
    // IN INDEX ORDER. Sub-step displacement breaks that: step k displaced by +0.5
    // places its child 7 at k+1.375 while step k+1 displaced by -0.5 places its
    // child 0 at k+0.5 — an inversion of 0.875 steps. So the whole bounded window is
    // scanned, in BOTH directions, over `(index, child)` PAIRS, and the earliest
    // qualifying onset wins.
    std::int64_t earliestOnset = std::numeric_limits<std::int64_t>::max ();

    for (int ahead = -retriggerScanBackSteps; ahead <= lookaheadSteps; ++ahead)
    {
        const std::int64_t index = stepIndex + ahead;

        // STOP SHORT OF A RESOLVED PATTERN SWITCH, FORWARD. This scan passes
        // `activePatternIndex` to `evaluateStep`, so from the adopt step onwards it
        // would describe the OUTGOING pattern's lanes for steps the INCOMING pattern
        // will play. Nothing is lost by skipping: `flushForPatternSwitch` releases
        // this note at `adoptSample - 1` anyway, which is at or before any cutoff we
        // could have found beyond the switch.
        //
        // `continue`, NOT `break` — the loop no longer visits indices in a single
        // direction, so terminating it here would also discard the backward band.
        if (ahead > 0 && pendingResolved && index >= adoptStepIndex)
            continue;

        // AND BACKWARD, THE MIRROR IMAGE. Below the last switch that actually FIRED,
        // `activePatternIndex` is the wrong pattern for the step — it is the incoming
        // one, and those steps were played by the outgoing one. See
        // `lastFiredAdoptStepIndex` for why the pending-switch fields cannot answer
        // this (they are cleared exactly when the switch fires).
        if (ahead < 0 && index < lastFiredAdoptStepIndex)
            continue;

        // THE PREDICTION. `evaluateStep` is a free function taking exactly the four
        // things a step depends on, so peeking at a NEIGHBOURING index here cannot
        // disturb (or be disturbed by) the emitting walk — see issue #53 in the
        // header.
        //
        // AT `ahead == 0` THE CALLER'S OWN EMISSION IS REUSED rather than
        // re-evaluated. Purity means the two would agree, so this is not a
        // correctness crutch — it saves the call, and it makes "a note and the
        // prediction made on its behalf describe the same step" true by construction
        // instead of true by argument.
        //
        // `runtime` IS THE CALLER'S LATCHED VALUE, threaded down rather than re-read
        // from the member, so this prediction and the emission it predicts evaluate
        // the FILL conditions against the identical flag.
        const StepEmission candidate =
            ahead == 0 ? emission : evaluateStep (*activeSnapshot, activePatternIndex, index, runtime);

        if (! candidate.gate)
            continue;

        // The SAME clamp `emitNote` applies, so the comparison is against the channel
        // that will actually be emitted rather than the raw snapshot value.
        if (juce::jlimit (1, 16, candidate.channel) != channel)
            continue;

        // ON THIS STEP, ONLY THIS NOTE'S SUCCESSORS ARE CANDIDATES. A ratchet's
        // children share a pitch, so child c+1 is what ends child c — this is where
        // §5.5's 1-sample intra-ratchet gaps come from. Notes at or before
        // `childIndex` are this note itself or its predecessors.
        const int firstChild = ahead == 0 ? childIndex + 1 : 0;

        for (int c = firstChild; c < candidate.noteCount; ++c)
        {
            const StepNote& candidateNote = candidate.notes[static_cast<std::size_t> (c)];

            if (juce::jlimit (0, 127, candidateNote.note) != note)
                continue;

            // THE CANDIDATE'S *PLACED* ONSET — its step's grid position, displaced by
            // its own `shiftSteps`, plus its own sub-step position. This is the reason
            // MICRO and swing are read inside `evaluateStep` rather than out-of-band
            // in the walk: a prediction made against the unshifted grid would be up to
            // half a step wrong, and the note-off it schedules wrong by the same
            // amount. Derived through `sampleForPpq`, THE one snap-then-floor, so this
            // prediction and the walk's eventual placement of the same event cannot
            // come from different arithmetic (issue #54) — and through the same
            // `stepPpq`, so no second tolerance is introduced (see `ownsPpq`).
            const double placedPpq =
                (static_cast<double> (index) + candidate.shiftSteps + candidateNote.positionInStep) * stepPpq;
            const std::int64_t onset = sampleForPpq (placedPpq, blockStartSample);

            // Not in this note's future — it cannot end a note that starts at or after
            // it. (Reachable under displacement, where an earlier index can place
            // later and a later index earlier; a same-sample collision is left to
            // `emitNote`'s retrigger branch, which the table's placement rule keeps
            // from inverting.)
            if (onset <= onSample)
                continue;

            // Past the note's natural end: it cannot SHORTEN the note, so it is not a
            // candidate. This replaces the deleted monotonicity `break` — the same
            // filter, applied per candidate instead of terminating the scan.
            if (onset - 1 >= naturalDueSample)
                continue;

            earliestOnset = juce::jmin (earliestOnset, onset);
        }
    }

    if (earliestOnset == std::numeric_limits<std::int64_t>::max ())
        return naturalDueSample;

    // §5.5's 1-sample gap, on the absolute timeline. `jmax` guards the degenerate
    // sub-sample-spacing case, which is unreachable: the tightest supported step is
    // 735 samples (1/32-triplet at 300 BPM / 44.1 kHz) and its narrowest ratchet
    // spacing is 735 / 8 = 91 samples, so the gap always has room. Without the guard
    // that case would schedule an off before its own on.
    return juce::jmax (onSample, earliestOnset - 1);
}

// RT-SAFE:
bool SequencerProcessor::discontinuedByPatternSwitch (std::int64_t index,
                                                      double eventPpq,
                                                      double stepPpq) const noexcept
{
    // TWO BOUNDARIES, BECAUSE A BLOCK CAN BE ON EITHER SIDE OF THE FIRING — and this
    // is exactly the insufficiency of guarding on `lastFiredAdoptStepIndex` alone.
    // Inside the block where the switch fires, the walk reaches the pre-switch index
    // BEFORE the adopt index, so the flag is still unset there; the resolved-but-not-
    // yet-fired switch is what has to answer. After the firing, `pendingResolved` has
    // been cleared and the flag is what answers. Testing both makes the decision a
    // property of the MUSIC (which step, which sample, which boundary) rather than of
    // which block happens to be rendering — which is the whole of the fix.
    if (eventReachesAdoptBoundary (index, eventPpq, lastFiredAdoptStepIndex, stepPpq))
        return true;

    return pendingResolved && eventReachesAdoptBoundary (index, eventPpq, adoptStepIndex, stepPpq);
}

// RT-SAFE:
void SequencerProcessor::emitNote (const StepEmission& emission,
                                   int childIndex,
                                   juce::MidiBuffer& midi,
                                   std::int64_t stepIndex,
                                   int offset,
                                   std::int64_t onSample,
                                   std::int64_t blockStartSample,
                                   int numSamples,
                                   double stepPpq,
                                   double samplesPerStep,
                                   StepRuntime runtime) noexcept
{
    if (! emission.gate || childIndex < 0 || childIndex >= emission.noteCount)
        return;

    const StepNote& stepNote = emission.notes[static_cast<std::size_t> (childIndex)];

    const int channel = juce::jlimit (1, 16, emission.channel);
    const int note = juce::jlimit (0, 127, stepNote.note);
    const int velocity = juce::jlimit (1, 127, stepNote.velocity);

    // §5.5 overlap policy — same-pitch retrigger, THE SAFETY NET. In the normal case
    // this branch no longer decides anything: `cutoffForSamePitch` has already
    // scheduled the outgoing note's off at `onSample - 1` when the note was
    // registered (issue #46), so by the time we get here D <= onSample always and the
    // placement comes from the table's own absolute-timeline conversion. The branch
    // stays because the lookahead has blind spots that MUST NOT hang a note:
    //   - a note registered before the lookahead could see this step (a snapshot
    //     adoption, and from Phase 8 a pool change, between the two steps);
    //   - `maxRetriggerLookaheadSteps` exceeded by a future LEN range;
    //   - a step the lookahead skipped because a pattern switch was resolved, which
    //     was then invalidated by a discontinuity before it fired;
    //   - THE PREDICTION WENT STALE (issue #50). `cutoffForSamePitch` runs at note-on
    //     time and predicts the NEXT same-pitch onset from the pattern as it stands
    //     then. A `PatternSnapshot` adopted before that predicted step arrives can
    //     have REMOVED it — the user punched the note out, changed PITCH/OCT, turned
    //     its GATE off, or edited a lane length so the step no longer lands on that
    //     pool degree. The already-scheduled cutoff is not revisited, so the sounding
    //     note ends at the sample the OLD pattern implied, up to a few steps earlier
    //     than the new one does. It is a wrong note LENGTH, never a hung note (the
    //     off is scheduled and the table still owns it) and never a determinism
    //     violation (the adoption happens at a block head, identically at every
    //     buffer size — the audible result depends on WHEN the edit was made, which
    //     is true of every live edit). Closing it means re-deriving pending cutoffs
    //     on adoption, which is real work for a transient artefact of live editing;
    //     tracked rather than fixed.
    //   - THE FILL FLAG MOVED (Phase 7.1) — the same shape as #50, one input over.
    //     `runtime.fillHeld` is latched per block, so a pad 16 press or release
    //     lands at a BLOCK HEAD. A cutoff predicted in block k against `fillHeld
    //     == false` can be arrived at in block k+1 with the flag now true, and a
    //     `FILL`-conditioned step the lookahead predicted as a rest then fires (or
    //     the reverse). Identical consequences to #50, for identical reasons: it is
    //     a wrong note LENGTH and never a hung note, because the off is already
    //     scheduled and `SoundingNoteTable` owns it outright; and it is NOT a
    //     determinism violation, because the flag can only change at a block head
    //     and WHICH block head is decided by when the performer pressed the pad —
    //     the same absolute sample at every buffer size, exactly as for a snapshot
    //     adoption. Within one block the flag is constant, so prediction and
    //     emission never disagree about it.
    //
    // TWO CASES, and conflating them is how the emitted stream becomes buffer-size
    // dependent (issue #36):
    //
    //   D > onSample  — the note is genuinely still sounding, and this on cuts it
    //     short. Note-off THEN note-on with a 1-sample gap. AT OFFSET 0 THE GAP
    //     CANNOT EXIST and this falls back to co-locating them, which is exactly the
    //     buffer-size dependency issue #46 is about — hence the lookahead upstream.
    //     Reaching this line with D > onSample means the lookahead missed, and the
    //     one-sample blemish is the deliberate price of never hanging a note.
    //
    //   D <= onSample — the off was ALREADY DUE and is only still in the table
    //     because the step walk runs before `emitDueNoteOffs` (see processBlock step
    //     3). Retiring it at `offset - 1` would place it wherever the re-on happens
    //     to fall, whereas a smaller buffer — one that puts the off and the re-on in
    //     DIFFERENT blocks — emits it at its exact due sample. Same musical input,
    //     different MIDI, decided by the device buffer size: a §1.2 violation. So
    //     place it at its TRUE position instead, via the table's own conversion.
    //
    // Since #48 the distinction is enforced by the table rather than reasoned about
    // here: `retireNoLaterThan` emits at `min (D, cap)`, so the already-due case
    // keeps its own sample no matter what cap this call site passes. The two cases
    // below therefore only choose how far a STILL-SOUNDING note may be shortened.
    //
    // REJECTED ALTERNATIVE — "just run emitDueNoteOffs before the step walk". It
    // does not fix this: a note-on emitted at step k schedules an off that can come
    // due WITHIN the same block, before step k+1's same-pitch on. At 300 BPM /
    // 48 kHz on a 1/32 grid, step k lands at offset 100 with a 50% gate (off due at
    // 150) and step k+1 lands at offset 200 on the same pitch — at the time the
    // reordered emitDueNoteOffs would have run, step k's note did not yet exist.
    // Closing that would require per-step interleaving of the two walks. Keep the
    // ordering; fix the placement.
    // ── `findStartedAtOrBefore`, NEVER `find` (ISSUE #77) ────────────────────
    // An entry whose own note-ON is LATER than this one's is not a note this note is
    // retriggering — it is a note that has not sounded yet, which the walk reached
    // early because its emission order is INDEX order and sub-step displacement broke
    // the correspondence with sample order. Retiring it produced a ZERO-LENGTH note
    // whose existence depended on whether the two onsets shared a block, i.e. on the
    // DEVICE BUFFER SIZE.
    //
    // MEASURED (the `ratchet-swing-retrigger` golden, 137 BPM / 44.1 kHz, RATCHET 8,
    // swing 66 %, LEN 150 %, one-note pool): the note-on at 337 — step -1's child 6,
    // reached before step 0's child 0 at sample 0 — had its off at 602 (correct, one
    // sample before the same-pitch onset at 603) at blocks 32, 64, 96, 128, 256 and
    // at 337 at blocks 480, 512, 1024, 2048, 4096. Five of ten agreed with the baked
    // reference; the reference was right.
    //
    // Nothing is lost by skipping it: the EARLIER note's own `cutoffForSamePitch` has
    // already scheduled it to end before that later onset — the scan looks backward
    // as well as forward and takes the minimum qualifying onset, which is exactly why
    // the small-block carvings were already correct. See `findStartedAtOrBefore` in
    // SoundingNoteTable.h, including what this costs the one-entry-per-pitch bound.
    if (const int existing = sounding.findStartedAtOrBefore (channel, note, onSample); existing >= 0)
    {
        // BOTH CASES GO THROUGH ONE ABSOLUTE-SAMPLE CALL. `retireNoLaterThan` emits
        // at `min (the entry's own due sample, this cap)`, so the cap says only "the
        // latest sample on which the outgoing note may still sound" and can never
        // push an off past its own schedule. There is no signature here that could
        // express the buffer-size-dependent `offset - 1` this replaces.
        //
        // The cap differs by case ONLY at D == onSample exactly: `onSample` keeps
        // that off co-located with the retrigger (off first by insertion order),
        // which is what the already-due branch has always done and what the goldens
        // hold. `onSample - 1` is the genuine 1-sample gap for a note still sounding.
        const std::int64_t capSample = sounding.isDueAtOrBefore (existing, onSample) ? onSample : onSample - 1;

        sounding.retireNoLaterThan (existing, midi, capSample, blockStartSample, numSamples);
    }

    // Note length from the LEN-shaped gate fraction, resolved to samples at the
    // CURRENT tempo (see SoundingNoteTable.h on why the schedule is in samples). At
    // least one sample so a note can never be zero-length.
    //
    // `stepNote.gateFractionOfStep` IS ALREADY PER SLOT — `evaluateStep` divided LEN
    // by the child count — so this multiplication is against the WHOLE step either
    // way and needs no ratchet term of its own. At `noteCount == 1` the slot is the
    // step and the arithmetic is bit-identical to Phase 6's.
    const double lengthInSamples = stepNote.gateFractionOfStep * samplesPerStep;
    const std::int64_t lengthSamples =
        juce::jmax<std::int64_t> (1, static_cast<std::int64_t> (std::llround (lengthInSamples)));

    const std::int64_t naturalDueSample = onSample + lengthSamples;

    // How far the lookahead has to reach, DERIVED FROM THIS NOTE rather than fixed.
    //
    // MEASURED FROM THE STEP'S OWN *UNSHIFTED* GRID BOUNDARY, which is the Phase 7.2
    // change and the reason this is not simply `lengthSamples / stepSamplesFloor`.
    // The scan enumerates INDICES relative to `stepIndex`, so the reach it needs is
    // the distance from that index's grid position to the end of this note — a span
    // that already contains the step's displacement (up to ±0.5 steps) AND the
    // child's own sub-step position (up to +0.875), without either having to be
    // named here. Naming them is what a future STRUM or a widened MICRO range would
    // silently invalidate.
    //
    // Computed in integers (the floor of `samplesPerStep` only ever makes the
    // estimate larger, never smaller) so no float edge case can under-reach. `+ 2`
    // rather than `+ 1`: one for the floor, and one because the CANDIDATE step may
    // itself be displaced up to half a step EARLIER, so an index just past the
    // reach can still place a note inside it. Then capped at
    // `maxRetriggerLookaheadSteps`, which bounds all of this from the lane ranges.
    const auto stepSamplesFloor = static_cast<std::int64_t> (samplesPerStep);
    const std::int64_t reachSamples = naturalDueSample - stepBoundarySample (stepIndex, stepPpq, blockStartSample);
    const int lookaheadSteps =
        stepSamplesFloor >= 1
            ? static_cast<int> (
                  juce::jlimit<std::int64_t> (1, maxRetriggerLookaheadSteps, reachSamples / stepSamplesFloor + 2))
            : maxRetriggerLookaheadSteps;

    // THE ISSUE #46 FIX (see "EVERY NOTE-OFF IS SCHEDULED" in the header): decide the
    // off's ABSOLUTE sample now — `min (natural end, next same-pitch on - 1)` — and
    // let `SoundingNoteTable::emitDueNoteOffs` place it in whichever block contains
    // it, including the block BEFORE this one when the retrigger lands on a block
    // head. That is the placement `offset - 1` structurally could not express.
    //
    // FOR A RATCHET CHILD THIS IS ALSO WHERE THE INTRA-STEP GAP COMES FROM, and it
    // needs no new mechanism: children share a pitch, so child c's next same-pitch
    // onset is child c+1 of this same step, the scan finds it (`ahead == 0`,
    // `c > childIndex`), and the off is scheduled at `onset(c+1) - 1`. When c+1
    // reaches this function the table's `retireNoLaterThan` emits at that already
    // scheduled sample and §5.5's 1-sample gap falls out of code that predates
    // ratchets entirely.
    const std::int64_t dueOffSample = cutoffForSamePitch (emission,
                                                          stepIndex,
                                                          childIndex,
                                                          channel,
                                                          note,
                                                          onSample,
                                                          naturalDueSample,
                                                          stepPpq,
                                                          blockStartSample,
                                                          lookaheadSteps,
                                                          runtime);

    // Register BEFORE emitting: a full table must suppress the note-on, never leave an
    // untracked note sounding (SoundingNoteTable overflow policy).
    //
    // `onSample` IS PASSED SO THE TABLE KNOWS THIS NOTE'S OWN LOWER BOUND. Since the
    // walk's emission order is no longer sample order (index order is, and
    // displacement breaks the correspondence), a later-emitted note can start
    // EARLIER than one already in the table, and a cap derived from it would
    // otherwise place that entry's off BEFORE its own note-on — an inverted pair,
    // i.e. a hung note at the synth. See "THE PLACEMENT RULE" in SoundingNoteTable.h.
    if (! sounding.add (channel, note, onSample, dueOffSample))
        return;

    const juce::uint8 bytes[3] = { static_cast<juce::uint8> (0x90 | ((channel - 1) & 0x0F)),
                                   static_cast<juce::uint8> (note & 0x7F),
                                   static_cast<juce::uint8> (velocity & 0x7F) };
    midi.addEvent (bytes, 3, offset);
}

// RT-SAFE:
bool SequencerProcessor::handleDiscontinuities (juce::MidiBuffer& midi,
                                                std::int64_t blockStartSample,
                                                int numSamples) noexcept
{
    // The transport head node has already drained the command queue and latched the
    // block-start state, so every signal below describes THIS block. A discontinuity
    // is always at the block head (commands land on block boundaries by construction —
    // see Transport.h), hence offset 0.
    const std::uint64_t generation = transport->stopGeneration ();

    if (! stopGenerationSeeded)
    {
        // First block after construction / prepare / async insertion: adopt the
        // counter without flushing. A flush here would be spurious — and while an
        // EMPTY flush emits nothing (SoundingNoteTable::flush only sweeps channels it
        // sounded on), seeding is still the honest behaviour.
        lastSeenStopGeneration = generation;
        stopGenerationSeeded = true;
    }

    // Missed-block safety net: this node is spliced in by an UpdateKind::async graph
    // edit, so it can start rendering having never seen the block whose latch carried
    // a stop edge. The monotonic counter catches that; the edges alone would not.
    const bool missedStop = generation != lastSeenStopGeneration;
    lastSeenStopGeneration = generation;

    const bool stopped = transport->stoppedThisBlock ();

    // A position jump orphans every pending note-off: they are scheduled on a sample
    // timeline the locate has just broken.
    const bool jumped = transport->positionJumpedThisBlock ();

    if (! (stopped || jumped || missedStop))
        return false;

    // RELEASE FROM THE BLOCK HEAD. A transport discontinuity destroys the timeline
    // the pending offs are scheduled on, so NOTHING here "ended on its own schedule"
    // — the table's already-ended window `[blockStartSample, releaseFromSample)` is
    // empty by construction, every entry is cut short at offset 0, and every entry
    // is swept. That includes entries orphaned by a locate, whose due samples now
    // sit in the past: they are cut short, not drained, so the CC123 sweep still
    // covers their channels.
    sounding.flush (midi, blockStartSample, numSamples, blockStartSample);
    jassert (sounding.isEmpty ()); // §5.5: the table MUST be empty after a flush point

    // INVALIDATE, BUT DO NOT DISCARD, a resolved pattern switch. `adoptStepIndex` is a
    // point on a timeline this discontinuity just destroyed — `transportStop` rewinds
    // to PPQ 0, so a switch resolved for bar 5 would sit forever in the future. The
    // REQUEST survives and re-resolves against the new position on the next block.
    //
    // Note the asymmetry with re-resolving every block, which would be the bug this
    // one-shot design avoids: once `blockStartPpq` passes the target, the ceiling
    // jumps to the following bar and the switch outruns the playhead indefinitely.
    pendingResolved = false;

    // FORGET THE LAST FIRED SWITCH TOO. It is a step index on the timeline this
    // discontinuity has just broken (`transportStop` rewinds to PPQ 0), so keeping it
    // would clamp the backward retrigger scan against a boundary that no longer names
    // the same musical moment. The table is empty here, so nothing needs the bound
    // right now — and by the time something does, a switch will have re-fired or not.
    lastFiredAdoptStepIndex = std::numeric_limits<std::int64_t>::min ();

    return true;
}

// RT-SAFE:
void SequencerProcessor::clearPendingSwitch () noexcept
{
    pendingRequested = false;
    pendingResolved = false;
}

// RT-SAFE:
void SequencerProcessor::resolvePendingSwitch (double stepPpq) noexcept
{
    if (! pendingRequested || pendingResolved || activeSnapshot == nullptr)
        return;

    // A switch to the pattern already playing is a NO-OP, not a fast reload. Firing it
    // would flush the sounding-note table mid-bar for no musical reason — an audible
    // click for a command the user experiences as "nothing should happen".
    if (pendingPatternIndex == activePatternIndex)
    {
        clearPendingSwitch ();
        return;
    }

    // Latched, block-start values — safe here (unlike in `applyCommand`) because
    // `Transport::beginBlock` has already run for THIS block.
    const double startPpq = transport->blockStartPpq ();
    double targetPpq = startPpq;

    switch (pendingQuantize)
    {
    case QuantizeMode::instant:
        // The next step boundary at or after the block start — i.e. the walk's own
        // `firstIndex`. "Instant" is still step-quantized: emitting a pattern change
        // between two step boundaries has no representation in the step walk.
        targetPpq = startPpq;
        break;

    case QuantizeMode::beat:
        targetPpq = snappedCeiling (startPpq, 1.0);
        break;

    case QuantizeMode::bar:
        // NOT `ppqOfLastBarStart () + quarterNotesPerBar`: that form SKIPS A WHOLE BAR
        // when `startPpq` sits exactly on a bar line (last bar start == startPpq, so
        // it targets the NEXT one). The snapped ceiling is correct on the boundary and
        // reuses the machinery the step walk is already proven against.
        targetPpq = snappedCeiling (startPpq, Transport::quarterNotesPerBar) * Transport::quarterNotesPerBar;
        break;

    case QuantizeMode::patternEnd:
    {
        // "Pattern end" is the GATE LANE's full cycle, not the lcm of all 11 lane
        // periods. lcm (64, 63, 62, …) is astronomically large — a "switch at pattern
        // end" that never fires. GATE is the trig lane, so its loop is the one the
        // listener hears as the pattern repeating.
        const PatternData& data = activeSnapshot->pattern (activePatternIndex);
        const auto periodSteps = static_cast<double> (data.gatePeriodSteps > 0 ? data.gatePeriodSteps : 1);
        const double periodPpq = periodSteps * stepPpq;

        targetPpq = snappedCeiling (startPpq, periodPpq) * periodPpq;
        break;
    }
    }

    adoptStepIndex = snappedStepCeiling (targetPpq, stepPpq);
    pendingResolved = true;
}

// RT-SAFE:
void SequencerProcessor::flushForPatternSwitch (juce::MidiBuffer& midi,
                                                std::int64_t adoptSample,
                                                std::int64_t blockStartSample,
                                                int numSamples) noexcept
{
    // THE OUTGOING PATTERN IS DISCONTINUED FROM `adoptSample` ONWARD. That single
    // absolute sample is everything the table needs, and it decides both halves of
    // the flush (see `SoundingNoteTable::flush`):
    //
    //   - A note STILL SOUNDING at the adopt point is cut short at `adoptSample - 1`
    //     — the same 1-sample-gap discipline as the same-pitch retrigger, so a note
    //     the outgoing pattern holds is released BEFORE the incoming pattern's
    //     note-on on the same pitch rather than at the same timestamp.
    //
    //   - A note whose own off was due EARLIER IN THIS BLOCK already ended; it keeps
    //     its true sample and is left out of the CC123 sweep. THIS IS ISSUE #48.
    //     Passing a single offset dragged such a note to `adoptSample - 1` (plus a
    //     spurious sweep) whenever the buffer was large enough to hold both samples
    //     in one block, while a smaller buffer emitted it exactly, from the earlier
    //     block, via `emitDueNoteOffs`. Same music, different MIDI, decided by the
    //     device buffer size — the third instance of the family #36 and #46 came
    //     from, and the reason the table now owns the placement outright.
    //
    // DECIDED ON THE ABSOLUTE TIMELINE, CONVERTED ONCE, INSIDE THE TABLE (issue #46).
    // The original form `jmax (0, offset - 1)` collapsed the gap whenever the adopt
    // point sat at block offset 0 — a buffer-size property, not a musical one.
    // `processBlock` PRE-FLUSHES from the previous block when `adoptSample - 1` lies
    // there, so the conversion's lower clamp is reached only in the residual case:
    //
    // THE RESIDUAL CASE, stated out loud: a switch RESOLVED and ADOPTED inside the
    // same block at offset 0 (an `instant` switch whose command was drained on a
    // block head that is also a step boundary) has no earlier block to pre-flush
    // into, so the flush lands ON the adopt sample. That is not a new dependency —
    // which block drains the command already decides `adoptStepIndex` itself for
    // `instant` (see `resolvePendingSwitch`), so the switch point is buffer-size
    // dependent before the flush placement ever is. juce::MidiBuffer preserves
    // insertion order among equal timestamps and this runs before the adopt step's
    // `emitStep`, so the off still precedes the on.
    //
    // NO DOUBLE-FLUSH WITH THE PRE-FLUSH PATH: that path empties the table in the
    // preceding block and deliberately leaves `pendingResolved` set, so this call
    // fires on an EMPTY table in the next block and emits nothing at all (neither
    // offs nor CC123). The switch still lands here, which is what §5.5 requires —
    // the table, not an inference about the table, is what must be empty.
    sounding.flush (midi, blockStartSample, numSamples, adoptSample);
    jassert (sounding.isEmpty ()); // §5.5: the table MUST be empty after a flush point
}

// RT-SAFE:
void SequencerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // MIDI-only node: no audio to render. Clear the (zero-channel) buffer defensively.
    buffer.clear ();

    // Capacity warm-up — unconditional; see outgoingWarmupBytes.
    midi.ensureSize (outgoingWarmupBytes);

    // NOTE: `midi` is deliberately NOT cleared. It carries MidiIn's live QWERTY and
    // hardware notes, which must keep reaching the synth (and become Phase 8's THRU
    // note pool). This node only ADDS to it.

    const int numSamples = buffer.getNumSamples ();

    if (transport == nullptr || numSamples <= 0)
        return;

    // 0. Adopt a newly published PatternSnapshot, at the BLOCK HEAD, with NO FLUSH.
    //
    //    WHY THE BLOCK HEAD AND NOT THE FIRST STEP BOUNDARY. §4 step 3 says "adopt at
    //    that sample offset", and here the two are observationally identical: the only
    //    in-block readers of the snapshot are `evaluateStep` and the grid, and both sit
    //    downstream of this point. Adopting here additionally keeps `gridStepPpq`
    //    unambiguous for the whole walk — adopting mid-block could otherwise change the
    //    step length underneath a half-finished step index range.
    //
    //    WHY NO FLUSH. §5.5's flush points are "transport stop, pattern SWITCH, pool
    //    change, plugin swap" — an adoption is a pattern EDIT. Flushing here would cut
    //    every sounding note on every piano-roll keystroke.
    if (patternChannel != nullptr && patternChannel->adopt (activeSnapshot))
    {
        // ONLY the first adoption seeds the active pattern. Re-seeding on every
        // adoption would yank the user back to `startPatternIndex` on every edit,
        // undoing whatever pattern they had switched to.
        if (! patternIndexSeeded && activeSnapshot != nullptr)
        {
            activePatternIndex = juce::jlimit (0, maxPatterns - 1, activeSnapshot->startPatternIndex);
            patternIndexSeeded = true;
        }
    }

    // THE BLOCK'S SAMPLE ORIGIN, read once. Every note-off placement in this node is
    // decided on the absolute timeline and converted against this pair, including
    // the discontinuity flush below — which is why the read sits ABOVE step 1 rather
    // than with the rest of the walk's arithmetic. `Transport::beginBlock` has
    // already latched it (the head node renders first), and nothing here mutates it.
    const std::int64_t blockStartSample = transport->blockStartTimeInSamples ();

    // 1. Discontinuities first: flush at the block head before anything new is emitted.
    handleDiscontinuities (midi, blockStartSample, numSamples);

    // 2. Step boundaries inside this block. STATELESS: the global step index range is
    //    re-derived from the transport's latched PPQ span every block. That makes the
    //    set of emitted step indices identical at every buffer size unconditionally,
    //    and their absolute sample positions identical too — EXCEPT for a boundary
    //    falling inside the `stepIndexSnapSteps` window just below a block edge, which
    //    the clamp below can emit up to one sample late. Bounded at one sample, never
    //    a duplicated or skipped step; see "THE SNAP-BOUNDARY WINDOW" in the header
    //    for the grid-relative bound and issue #37.
    const double ppqPerSample = transport->ppqPerSample ();

    // THE GRID IS PROJECT-LEVEL (§8.1 `transport.grid`; see the note on `PatternState`
    // in PatternTypes.h), so one value governs the whole walk and the walk stays a
    // SINGLE segment. A per-pattern grid would make a quantized pattern switch change
    // the meaning of the step index mid-flight — precisely the discontinuity §5.5
    // exists to prevent. Fall back to the documented default before anything is
    // adopted, so `samplesPerStep` is never derived from a zero.
    const double curStepPpq = (activeSnapshot != nullptr && activeSnapshot->gridStepPpq > 0.0)
                                  ? activeSnapshot->gridStepPpq
                                  : scaffoldStepPpq;

    // Resolve a queued pattern switch ONCE, now that `beginBlock` has latched this
    // block's position. Cheap no-op when there is nothing pending.
    resolvePendingSwitch (curStepPpq);

    // A stopped transport reports blockEndPpq() == blockStartPpq(), so the walk below
    // would produce an empty range anyway; the explicit guard just says so out loud.
    // THE BLOCK'S LIVE (NON-SNAPSHOT) INPUTS, LATCHED ONCE, ABOVE THE WALK.
    //
    // Reading `fillHeld` per step instead would be OBSERVATIONALLY IDENTICAL today:
    // the only writer is `applyCommand`, which runs during the transport head node's
    // drain — strictly before this node renders — so the member cannot change while
    // the walk below is running. It is latched anyway, for two reasons that are
    // about the future rather than about today:
    //
    //   1. IT IS WHAT MAKES THE PURITY AMENDMENT TRUE. The Phase 7.1 note in the
    //      header rests on the fourth input being const for the whole block and
    //      passed IDENTICALLY to the lookahead and to the emission. A `const` local
    //      makes that a property of the code rather than a property of the current
    //      graph node ordering; the lookahead literally cannot see a different value
    //      from the emission, because there is only one value.
    //   2. THE DAY ANYTHING CAN DRAIN MID-BLOCK, per-step reads become a real bug —
    //      a sub-block drain (for sample-accurate parameter automation, say) would
    //      make step 3 and step 11 of one block evaluate FILL differently, and which
    //      steps fell on which side would be a function of the device buffer size.
    //      That is the #36 family, one input over. This line pre-empts it.
    const StepRuntime runtime { fillHeld };

    if (transport->isPlaying () && ppqPerSample > 0.0)
    {
        const double samplesPerStep = curStepPpq / ppqPerSample;

        // Half-open [firstIndex, endIndex) with the SAME snapped ceiling applied to
        // both ends, so block k's endIndex equals block k+1's firstIndex exactly.
        //
        // AS OF PHASE 7.2 THESE ARE THE SCAN'S ORIGIN, NOT ITS DECISION. Ownership is
        // decided per event by `ownsPpq` against the event's PLACED position; this
        // pair only says where to start and stop LOOKING. See `stepScanBack` /
        // `stepScanForward` for why the scan reaches two indices back and one forward,
        // and why that widening is tight rather than generous.
        const std::int64_t firstIndex = snappedStepCeiling (transport->blockStartPpq (), curStepPpq);
        const std::int64_t endIndex = snappedStepCeiling (transport->blockEndPpq (), curStepPpq);

        for (std::int64_t index = firstIndex - stepScanBack; index < endIndex + stepScanForward; ++index)
        {
            // THE UNSHIFTED GRID POSITION of this step. The quantized pattern switch
            // stays on it — a quantized apply is a control event, not musical content,
            // and must not be dragged around by a swing or MICRO value.
            const double gridPpq = static_cast<double> (index) * curStepPpq;

            // ── THE QUANTIZED PATTERN SWITCH LANDS HERE (§5.2, §6.1) ─────────
            // Inside the walk, at the resolved step's own sample offset, BEFORE that
            // step is described — so the very first step of the new pattern is played
            // from the new pattern.
            //
            // WHY THE LOOP IS GUARANTEED TO VISIT `adoptStepIndex` (or defer it):
            // `resolvePendingSwitch` derives the target PPQ with a ceiling from
            // `blockStartPpq ()`, so `targetPpq >= blockStartPpq ()` and therefore
            // `adoptStepIndex >= firstIndex` — the index can never fall BELOW this
            // block's range and be silently skipped. If it is `>= endIndex` it simply
            // belongs to a later block and stays pending, and because the walk's
            // half-open ranges tile the timeline exactly (block k's `endIndex` IS
            // block k+1's `firstIndex`, by construction), that later block visits it.
            //
            // THE `ownsPpq` GUARD IS MANDATORY SINCE THE SCAN WAS WIDENED, and the
            // failure it prevents is NOT a repeated switch — it is a STOLEN one.
            // `stepScanForward` makes the block BEFORE the owner visit
            // `adoptStepIndex` too, and firing is one-shot (`clearPendingSwitch`
            // clears the request), so on the index test alone that earlier block fires
            // first and the owner never fires at all. The pattern then changes up to
            // one block early, at a sample decided by the DEVICE BUFFER SIZE — the #36
            // shape again. (The scan-BACK band cannot steal it: those indices are
            // below `firstIndex`, and `adoptStepIndex >= firstIndex` always holds for
            // a resolved switch.)
            //
            // MEASURED, by deleting this guard: 9 tests red — pattern_switch's four
            // quantize-mode literals, three of sequencer_offdeterminism's cross-block-
            // size cases, the switch-flush table-empty assertion, and TWO GOLDENS.
            //
            // Only the owning block passes `ownsPpq (gridPpq)`, which is exactly the
            // "is it mine" the index range used to express. Tested against `gridPpq`,
            // NOT `placedPpq`, per the note above.
            //
            // THE TEST IS ON THE INDEX, NOT ON THE EMISSION: the switch must fire even
            // when the adopt step is not gated, or a pattern whose target step happens
            // to be a rest would never switch.
            if (pendingResolved && index == adoptStepIndex && ownsPpq (*transport, gridPpq, curStepPpq))
            {
                // The adopt point on the absolute timeline, through THE ONE
                // snap-then-floor, with the #37 lower clamp (see below) and nothing
                // else — the same value the walk used to hand it before the switch
                // and the emission had separate positions to reason about.
                const std::int64_t adoptSample =
                    juce::jmax (blockStartSample, sampleForPpq (gridPpq, blockStartSample));

                // Normally a no-op by now: the pre-flush below, or the switch-aware
                // bound in `cutoffForSamePitch`, has already retired everything the
                // outgoing pattern was holding. It still runs — the table, not an
                // inference about the table, is what §5.5 requires to be empty.
                flushForPatternSwitch (midi, adoptSample, blockStartSample, numSamples);
                activePatternIndex = pendingPatternIndex;

                // REMEMBERED FOR THE BACKWARD RETRIGGER SCAN, and it must be recorded
                // BEFORE `clearPendingSwitch` destroys the only copy of the index. See
                // `lastFiredAdoptStepIndex`: from here on `activePatternIndex` is the
                // INCOMING pattern, so steps below this point must not be evaluated
                // against it.
                lastFiredAdoptStepIndex = adoptStepIndex;

                clearPendingSwitch ();
            }

            // THE EMISSION CORE, called with exactly the four inputs it is allowed
            // to see (issue #53, and its Phase 7.1 amendment for `runtime`).
            // `activeSnapshot` can still be null here — nothing has been adopted yet
            // — and a default `StepEmission` has `gate == false` (and `shiftSteps
            // == 0`), so nothing is placed and nothing is emitted, which is what the
            // old null-check inside the core did. The switch above cannot have fired
            // in that case either (`resolvePendingSwitch` returns early on a null
            // snapshot).
            //
            // CALLED BEFORE THE OWNERSHIP TEST SINCE PHASE 7.2 STAGE 3, because the
            // displacement that DECIDES ownership now comes out of the core. It is
            // called for every scanned index, including the three the scan widening
            // adds; that is ~3 extra pure table reads per block and it is what buys
            // the lane-read discipline documented on `StepEmission::shiftSteps`.
            const StepEmission emission = activeSnapshot != nullptr
                                              ? evaluateStep (*activeSnapshot, activePatternIndex, index, runtime)
                                              : StepEmission {};

            // THE PLACED POSITION of this step's onset: the grid position displaced
            // by the composed MICRO + swing shift, which `evaluateStep` has already
            // clamped to ±`maxSubStepShiftSteps`.
            //
            // THE SEAM IS HERE, IN PPQ, NOT DOWN IN SAMPLES. A samples-side
            // displacement would use the EMITTING block's `samplesPerStep`, so a
            // tempo change landing between an event's home block and its emitting
            // block would make the two blocks derive different absolute samples for
            // the same event — and the ownership test would then drop it in both or
            // duplicate it in both. A PPQ displacement is tempo-independent, and
            // `Transport::reanchor` preserves the PPQ reached, so the tiling survives
            // a re-anchor. It also leaves this expression BIT-IDENTICAL to `gridPpq`
            // whenever `shiftSteps` is 0, which is what makes the pre-7.2 goldens the
            // proof that stages 1-3 are behaviour-neutral.
            const double placedPpq = gridPpq + emission.shiftSteps * curStepPpq;

            // ── OWNERSHIP: DOES THIS BLOCK EMIT THIS EVENT? ──────────────────
            // THE UPPER CLAMP THAT USED TO LIVE BELOW IS NOW THIS REJECT, and the two
            // must never be conflated again. `jlimit`'s upper arm pinned an event past
            // the block end to `numSamples - 1` — a position decided by the DEVICE
            // BUFFER SIZE, which is the exact shape of issue #36. The event does not
            // ── OWNERSHIP AND EMISSION ARE PER `(index, child)` ──────────────
            // NOT PER STEP, since Phase 7.2 stage 4. A step's children sit at
            // distinct sub-step positions and each belongs to whichever block
            // CONTAINS it — which is emphatically not always the parent's block: at
            // the worst supported step (a dotted quarter at 20 BPM / 192 kHz,
            // 864 000 samples) child 7 is 756 000 samples and many blocks after
            // child 0. So each child gets its own `ownsPpq` test, its own offset
            // conversion and its own `emitNote`.
            //
            // `noteCount` IS 0 WHEN THE STEP DOES NOT FIRE, so an ungated or
            // suppressed step simply runs this loop zero times — there is no
            // separate gate test here, and the pattern switch above has already had
            // its (deliberately emission-independent) chance to fire.
            for (int child = 0; child < emission.noteCount; ++child)
            {
                // THE CHILD'S PLACED POSITION. `positionInStep` is a PPQ OFFSET
                // INSIDE THE PARENT'S STEP, added to the parent's already-displaced
                // position — NOT a position on a `stepPpq / noteCount` sub-grid with
                // a snapped ceiling of its own. That distinction is the second of the
                // two things forbidden in the header's sub-step geometry note: a
                // per-child ceiling would be a SECOND tolerance evaluated at the same
                // block edges as `ownsPpq`'s, which is precisely how two adjacent
                // blocks come to disagree about who owns an event.
                const double childPpq =
                    placedPpq + emission.notes[static_cast<std::size_t> (child)].positionInStep * curStepPpq;

                // ── IS THIS EVENT DISCONTINUED BY A PATTERN SWITCH? (#76) ────
                // BEFORE the ownership test, deliberately: this question is about the
                // MUSIC (does this note sound at or after the point its own pattern is
                // discontinued) and must be answered identically in every block,
                // including the blocks that do not own the event. Asking it after
                // ownership would be observationally the same today and would invite a
                // future reader to fold it into the ownership predicate, where it would
                // acquire a dependence on the block.
                if (discontinuedByPatternSwitch (index, childPpq, curStepPpq))
                    continue;

                // ── DOES THIS BLOCK EMIT THIS EVENT? ─────────────────────────
                // THE UPPER CLAMP THAT USED TO LIVE BELOW IS NOW THIS REJECT, and the
                // two must never be conflated again. `jlimit`'s upper arm pinned an
                // event past the block end to `numSamples - 1` — a position decided by
                // the DEVICE BUFFER SIZE, which is the exact shape of issue #36. The
                // event does not belong to this block at all; the block that owns it
                // emits it exactly.
                //
                // NO "ALREADY EMITTED" SET IS NEEDED, and adding one would be the
                // cursor issue #53 forbids: `childPpq (index, c)` is a PURE function of
                // the snapshot and the pair, the snapped half-open PPQ spans tile the
                // timeline exactly, so each `(index, c)` is owned by exactly one block
                // — and the scan widening guarantees that block visits it.
                if (! ownsPpq (*transport, childPpq, curStepPpq))
                    continue;

                // THE ONE SNAP-THEN-FLOOR (`sampleForPpq`), then THE LOWER CLAMP,
                // WHICH SURVIVES — it is a different thing entirely from the upper one
                // above. It is the issue #37 window: a position within
                // `stepIndexSnapSteps` below this block's start is claimed by THIS
                // block by the snapped ceiling (and by `ownsPpq`, which is the same
                // ceiling re-expressed), so the raw sample comes back slightly BELOW
                // `blockStartSample` and the clamp emits it at offset 0 — up to one
                // sample later than a carving that put it mid-block. That is the
                // documented exception, not a defect to "fix" by deleting the clamp;
                // see "THE SNAP-BOUNDARY WINDOW" in the header. NOTE that the window's
                // HIT RATE is now up to ~8x what it was, because a step offers up to
                // eight positions to be tested against block edges instead of one; its
                // WIDTH is unchanged (it is denominated against the untouched grid).
                const std::int64_t rawOffset = sampleForPpq (childPpq, blockStartSample) - blockStartSample;

                // UNREACHABLE BY CONSTRUCTION, and kept as a structural net rather
                // than an assumption: `ownsPpq` accepted, so `childPpq < blockEndPpq -
                // snapPpq`, i.e. the raw offset is below `numSamples - snapSamples`
                // where `snapSamples = 1e-6 x samplesPerStep >= 7.35e-4` (the tightest
                // supported step is 735 samples). That margin exceeds
                // `sampleOffsetSnapSamples` (1e-4), so the floor can never reach
                // `numSamples`. An out-of-range `addEvent` offset is a real hazard and
                // juce::MidiBuffer does not validate it, so the guard stays.
                if (rawOffset >= static_cast<std::int64_t> (numSamples))
                {
                    jassertfalse; // ownership accepted an event this block cannot place
                    continue;
                }

                const auto offset = static_cast<int> (juce::jmax<std::int64_t> (0, rawOffset));
                const std::int64_t onSample = blockStartSample + static_cast<std::int64_t> (offset);

                emitNote (emission,
                          child,
                          midi,
                          index,
                          offset,
                          onSample,
                          blockStartSample,
                          numSamples,
                          curStepPpq,
                          samplesPerStep,
                          runtime);
            }
        }
    }

    // 3. Note-offs that come due inside this block — INCLUDING notes started above,
    //    whose gate can be shorter than one block at fast tempos / large buffers.
    //    Emitted after the note-ons, which is fine: juce::MidiBuffer inserts in sorted
    //    sample order, so the buffer handed downstream is strictly sample-sorted (§5.5).
    sounding.emitDueNoteOffs (midi, blockStartSample, numSamples);

    // 4. PATTERN-SWITCH PRE-FLUSH — the other half of the issue #46 fix.
    //
    //    `flushForPatternSwitch` releases at absolute `adoptSample - 1`. When the
    //    adopt point is EXACTLY this block's end, that sample is this block's LAST
    //    one, and the block that fires the switch could never emit it — which is the
    //    whole shape of #46: at some buffer sizes a boundary is a block head and at
    //    others it is not, and `offset - 1` silently became `offset`. So the flush is
    //    performed HERE instead, in the block that actually contains the sample. The
    //    emitted stream is then identical at every buffer size.
    //
    //    AFTER `emitDueNoteOffs`, deliberately: notes whose own off comes due earlier
    //    in this block must keep their true positions rather than all be dragged to
    //    the block's last sample.
    //
    //    NOTHING IS EMITTED WHEN THERE IS NOTHING SOUNDING (`SoundingNoteTable::flush`
    //    on an empty table emits neither offs nor CC123), so this is invisible to the
    //    common short-gate case, and the switch itself still fires in the next block —
    //    `pendingResolved` is deliberately NOT cleared here.
    //
    //    EQUALITY, not `>=`: only the exact next-block-head case needs moving. Any
    //    other relationship is either still in the future or already handled inside
    //    the walk at a positive offset.
    if (pendingResolved && transport->isPlaying () && ppqPerSample > 0.0)
    {
        const std::int64_t adoptSample = stepBoundarySample (adoptStepIndex, curStepPpq, blockStartSample);

        if (adoptSample == blockStartSample + static_cast<std::int64_t> (numSamples))
            flushForPatternSwitch (midi, adoptSample, blockStartSample, numSamples);
    }
}

// RT-SAFE:
void SequencerProcessor::processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer&)
{
    // The root graph runs in single precision; this must never be invoked.
    jassertfalse; // graph is single precision
    buffer.clear ();
}

bool SequencerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // MIDI-only: accept only the empty (no main audio in/out) layout.
    return layouts.getMainInputChannelSet ().isDisabled () && layouts.getMainOutputChannelSet ().isDisabled ();
}
} // namespace arpbox::engine
