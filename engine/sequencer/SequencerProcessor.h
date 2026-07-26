#pragma once

#include "../EngineGuiGuard.h"
#include "../graph/ICommandSink.h"
#include "../graph/Transport.h"
#include "../midi/SoundingNoteTable.h"
#include "PatternChannel.h"
#include "PatternSnapshot.h"
#include "PatternTypes.h"
#include "Provenance.h"
#include "StepLogic.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace arpbox::engine
{
/** What one step boundary contributes to the output. Defined at namespace scope
    BELOW `SequencerProcessor` (it names the class's documented defaults) and
    forward-declared here so `emitStep` can take it by reference. */
struct StepEmission;

// ── THE SUB-STEP GEOMETRY (Phase 7.2) ────────────────────────────────────────
// The two numbers every sub-step derivation in this file descends from. They are
// declared HERE, at namespace scope, because the step walk's scan widening and the
// retrigger lookahead's reach are both `constexpr` expressions of them: a future
// change to either number must be made in ONE place and re-derive everything
// automatically, rather than leaving a hand-computed literal behind.
//
// ── TWO THINGS ARE FORBIDDEN HERE, AND BOTH LOOK LIKE GOOD IDEAS ────────────
// Read this before changing how a sub-step position is derived. The subject is
// the #37 snap window, whose width is `1e-6 x stepPpq x 60 x sampleRate / bpm`
// and whose worst supported value is 0.864 samples (dotted quarter, 20 BPM,
// 192 kHz — the table is in "THE SNAP-BOUNDARY WINDOW" below). Phase 7.2 did NOT
// re-derive that table, and the reason it did not have to is that `stepPpq` is
// THE GRID, and the grid is untouched: MICRO, swing and ratchets displace events
// WITHIN a grid interval, they do not change the interval. Two changes would
// break that reasoning:
//
//   1. NEVER IMPLEMENT SWING AS A MODIFIED GRID. Halving `stepPpq` to get a
//      "swing grid", or feeding the walk a per-step-pair grid, changes the
//      window's own input — every row of that table would need recomputing, and
//      the worst supported case has only ~0.14 samples of headroom left. Swing is
//      a DISPLACEMENT (`swingShiftSteps`, sequencer/StepLogic.h) precisely so the
//      grid stays the one quantity the tolerance is denominated against.
//
//   2. RATCHET CHILDREN MUST NEVER GET THEIR OWN CEILING. A `stepPpq / noteCount`
//      sub-grid with its own snapped ceiling is a SECOND tolerance evaluated at
//      the same block edges as the first, and two tolerances at one edge is
//      exactly how block k and block k+1 come to disagree about who owns an event
//      — owned by zero blocks (dropped) or by two (duplicated). Child positions
//      are therefore PPQ OFFSETS inside the parent's step, tested by the one
//      `ownsPpq` against the one `stepPpq`. See the note on `ownsPpq` in the .cpp.
//
// ── THE HONEST CAVEAT: THE WINDOW'S HIT RATE ROSE ~8x ───────────────────────
// Its WIDTH is unchanged, but a step now offers up to eight PPQ values to be
// tested against block edges instead of one, so the CHANCE of some event landing
// inside the window per step is up by about that factor. Issue #75 (no test ever
// renders a boundary inside the window — `tests/boundary_agreement.cpp` filters
// to `snapWindow == 0`) therefore goes from a theoretical coverage gap to a live
// one, and its witness belongs on the critical path.

/** The hard bound on how far, in STEPS, a step's onset may be displaced from its
    grid position by the composed MICRO + swing shift.

    DERIVED FROM §12.1: MICRO is ±50 % of a step, and swing (`swingPct` 50..75)
    delays odd steps by up to +0.5 step. The COMPOSED total is clamped to ±this,
    once, inside `evaluateStep`, so the bound has exactly one enforcement point.

    RAISING THIS NUMBER IS NOT A LOCAL CHANGE. The step walk's scan widening below
    is TIGHT — both of its bounds are attained, not padded — so a larger shift
    silently drops notes at block edges, uniformly at every buffer size (which is
    precisely the failure no cross-buffer-size comparison can see). Re-derive
    `stepScanBack` / `stepScanForward` in SequencerProcessor.cpp first. */
inline constexpr double maxSubStepShiftSteps = 0.5;

/** Ratchet children per step (§12.1 RATCHET 1..8, §5.1 L2 "Ratchets (1–8)").

    DERIVED FROM THE LANE RANGE, not written as 8, so a future RATCHET range
    change re-derives `maxChildAheadSteps`, `StepEmission::notes`'s size and
    `maxRetriggerLookaheadSteps` automatically instead of leaving three
    hand-computed literals behind. */
inline constexpr int maxRatchetChildren = laneRange (LaneId::ratchet).hi;
static_assert (maxRatchetChildren == 8, "§12.1 caps RATCHET at 8; the whole sub-step geometry descends from it.");

/** How far after its parent's onset, in STEPS, the LAST ratchet child can sit.

    DERIVED: children are evenly spaced across the parent's own step, so child `c`
    of `n` sits `c / n` steps after the parent and the maximum over all legal
    `(n, c)` is `(maxRatchetChildren - 1) / maxRatchetChildren` = 7/8. */
inline constexpr double maxChildAheadSteps =
    static_cast<double> (maxRatchetChildren - 1) / static_cast<double> (maxRatchetChildren);
// Pinned against the LITERAL 0.875 rather than the expression `7.0 / 8.0`: both are
// exactly representable, but clang's `-Wfloat-equal` (on via
// juce_recommended_warning_flags) exempts a comparison against a floating LITERAL and
// not one against an arithmetic expression. The value is a dyadic rational, so this is
// an exact pin either way.
static_assert (maxChildAheadSteps == 0.875, "Child 7 of 8 sits 7/8 of a step after its parent.");

// RT-SAFE / constexpr: `ceil` for non-negative inputs.
//
// AT NAMESPACE SCOPE BECAUSE THE DERIVATIONS BELOW MUST BE `constexpr`.
// `std::ceil` is not guaranteed constant-evaluable in C++20, so a `static_assert`
// written against it may or may not compile depending on the standard library —
// and an unchecked derivation is exactly the hand-computed literal these constants
// exist to eliminate. Non-negative inputs only, which is all this is applied to.
constexpr std::int64_t ceilNonNegative (double x) noexcept
{
    const auto truncated = static_cast<std::int64_t> (x);

    return static_cast<double> (truncated) < x ? truncated + 1 : truncated;
}

/** LEN's ceiling expressed in STEPS (§12.1: LEN is 1..400 % of the step). */
inline constexpr double maxNoteLengthSteps = static_cast<double> (laneRange (LaneId::len).hi) / 100.0;
static_assert (maxNoteLengthSteps == 4.0, "LEN 400 % means a note can span four steps.");

/** Hard ceiling on how many steps FORWARD `cutoffForSamePitch` may look.

    ── FULLY DERIVED, AND THAT IS THE POINT (issue #69) ────────────────────────
    It was `5`, a private class constant with a hand-written derivation, and
    `tests/step_purity.cpp` mirrored it as a hand-copied literal because nothing
    outside the class could name it. #69 predicted exactly what has now happened:
    the constant MOVED, and a copied literal would have left the purity guard
    exercising a two-step-shallower access pattern than production while staying
    green. So it is at NAMESPACE SCOPE (the test references this symbol) and it is
    an EXPRESSION of the lane ranges it descends from:

        LEN ceiling                       maxNoteLengthSteps      4.0
      + last child's offset in its step   maxChildAheadSteps      0.875
      + this note's own displacement      maxSubStepShiftSteps    0.5
      + the CANDIDATE's displacement      maxSubStepShiftSteps    0.5
      =                                                           5.875  -> ceil 6
      + 1 for the +/-1 sample of floor/round jitter                      -> 7

    The last two terms are what Phase 7.2 added: a note can start half a step LATE
    and be cut short by a step that starts half a step EARLY, so the reach is two
    displacements wider than the length alone. The live bound is still computed per
    note from its actual reach (a 50 % gate looks one step ahead, not seven); this
    is only the defensive cap. */
inline constexpr int maxRetriggerLookaheadSteps =
    static_cast<int> (ceilNonNegative (maxNoteLengthSteps + maxChildAheadSteps + 2.0 * maxSubStepShiftSteps)) + 1;
static_assert (maxRetriggerLookaheadSteps == 7,
               "The retrigger lookahead's forward reach must be re-derived if any of LEN's ceiling, the "
               "RATCHET ceiling or the sub-step shift bound changes. If this fires, the derivation above "
               "is what to re-read — and tests/step_purity.cpp follows automatically (issue #69).");

/** How many steps BACKWARD `cutoffForSamePitch` must look.

    ── WHY LOOKING BACKWARD IS NECESSARY AT ALL ────────────────────────────────
    A note emitted for step k+1 can be overtaken by a child of step k that has not
    sounded yet: the walk visits indices in order, but under displacement index
    order is NOT sample order. Step k displaced by +0.5 places its child 7 at
    k+1.375, which is AFTER step k+1 displaced by -0.5 (at k+0.5).

    ── AND WHY ONE STEP IS ENOUGH, EXACTLY ─────────────────────────────────────
    A candidate at index i-b places its latest child at
    `(i - b) + maxSubStepShiftSteps + maxChildAheadSteps`; the note being scheduled
    starts no earlier than `i - maxSubStepShiftSteps`. For the candidate to land
    AFTER it:

        -b + 0.5 + 0.875 > -0.5   ⟹   b < 1.875   ⟹   b <= 1

    So the reach is `ceil (2 * maxSubStepShiftSteps + maxChildAheadSteps) - 1`,
    which is 1 today and re-derives itself if the geometry moves. */
inline constexpr int retriggerScanBackSteps =
    static_cast<int> (ceilNonNegative (2.0 * maxSubStepShiftSteps + maxChildAheadSteps)) - 1;
static_assert (retriggerScanBackSteps == 1, "The backward retrigger reach must be re-derived with the geometry.");

/** The ARP ENGINE node (ARCHITECTURE §3.3 "[ARP ENGINE node]", §4 step 4). A
    MIDI-only `AudioProcessor` sitting between the MIDI-in node and the hosted synth:
    `Transport → MidiIn → Seq → Synth → Master`.

    ── WHAT THIS NODE IS, AS OF PHASE 7.2 ──────────────────────────────────────
    Four things, and deliberately nothing else:

      1. The step-boundary WALK (below) — buffer-size independent, stateless. Since
         Phase 7.2 it walks `(step index, ratchet child)` PAIRS rather than step
         indices, because a step can now place up to eight notes at distinct
         sub-step positions.
      2. `evaluateStep()` — the L1+L2 read, a FREE FUNCTION declared below this
         class: the `PatternSnapshot`'s GATE / PITCH / OCT / VEL / LEN lanes plus
         the L2 step logic (PROB, COND, MOD A via `NEI`, RATCHET and MICRO —
         sequencer/StepLogic.h) resolved for one global step index. It is not a
         member, deliberately; see "THE PURITY OF THE EMISSION CORE IS STRUCTURAL".
      3. The quantized PATTERN SWITCH (`queuePatternSwitch`), which is why this class
         is an `ICommandSink` — as is the §12.2 FILL flag (`setFillHeld`).
      4. Note-off ownership via `SoundingNoteTable` (§5.5).

    TEN OF THE ELEVEN §12.1 LANES ARE LIVE. Still absent on purpose: MOD B (Phase
    14.1's mod matrix), the operator stack and constraint gate (§5.1 L3, Phase 12+),
    and the live note pool (Phase 8; the snapshot's stub pool is what is read
    today). See `evaluateStep`, whose in-body manifest is the authoritative list.

    ── THE STEP WALK, AND WHY IT IS BUFFER-SIZE INDEPENDENT ────────────────────
    Nothing about "where are we in the pattern" is stored in this object. There is no
    per-block step counter. Each block, the GLOBAL step index range is re-derived
    from the transport's latched PPQ span:

        firstIndex = ceilSnap (blockStartPpq () / stepPpq)
        endIndex   = ceilSnap (blockEndPpq ()   / stepPpq)   // exclusive
        for (n = firstIndex - scanBack; n < endIndex + scanForward; ++n)
            if (ownsPpq (placedPpqOf (n)))  emit step n

    `Transport` guarantees PPQ at an absolute sample is bit-identical however the
    blocks were carved (see Transport.h), so this range is a pure function of the
    absolute sample span — carve the same timeline into 32-sample or 2048-sample
    blocks and every step index is produced exactly once. THE INDEX SET IS
    UNCONDITIONALLY IDENTICAL; the emitted SAMPLE POSITION is identical too, with
    one bounded exception documented under "THE SNAP-BOUNDARY WINDOW" below. A
    stateful cursor would instead be a place for a missed or duplicated boundary to
    hide, which is why there isn't one.

    PHASE 7.2 SPLIT THE RANGE FROM THE DECISION, and that split is the whole of the
    change. `[firstIndex, endIndex)` used to be BOTH "which indices to look at" and
    "which indices are mine". Sub-step displacement (MICRO, swing, ratchets) breaks
    the second meaning — a displaced onset belongs to whichever block CONTAINS it,
    which need not be the block its grid position falls in — so the range is now only
    the former, widened by `stepScanBack` / `stepScanForward`
    (SequencerProcessor.cpp), and ownership is decided per event by `ownsPpq` against
    the event's PLACED position. `ownsPpq` is the SAME snapped ceiling re-expressed in
    PPQ, not a second one; at zero displacement it is provably the old index test, and
    tests/boundary_agreement.cpp asserts that equivalence over engineered
    boundary/block-edge coincidences rather than leaving it as an argument.

    WHAT THE OLD FORM WOULD HAVE DONE UNDER DISPLACEMENT, and why it could not simply
    be left alone: the walk's `jlimit` pinned an out-of-block offset to `numSamples-1`
    — a position decided by the DEVICE BUFFER SIZE, i.e. issue #36 exactly. That upper
    clamp is now the `ownsPpq` reject. The LOWER clamp survives and is a different
    thing entirely (it is the #37 window); conflating the two is the single easiest
    way to reintroduce #36.

    Two SNAPS make that exact in floating point. Both are orders of magnitude larger
    than the accumulated rounding they absorb, and both are smaller than one sample
    at every supported grid/tempo/sample-rate combination — though snap 1's margin
    is grid-dependent and NOT large (see the window note below; the coarsest grid at
    the slowest tempo and highest sample rate leaves only ~0.14 samples of it).
    Without them, one specific and very common family of configurations breaks:

      1. `stepIndexSnapSteps` — consecutive blocks share an edge only
         MATHEMATICALLY: `blockEndPpq()` of block k evaluates
         `ppq_k + advance * pps` while `blockStartPpq()` of block k+1 evaluates
         `anchorPpq + (s_k + advance - anchor) * pps`. Equal in exact arithmetic,
         but they can differ by an ulp. A boundary landing inside that disagreement
         would then be emitted twice, or not at all. Applying the SAME snapped ceiling
         to both ends of the half-open interval makes block k's `endIndex` and block
         k+1's `firstIndex` agree.
         BE PRECISE ABOUT HOW STRONG THAT IS (issue #82). `endIndex(k)` and
         `firstIndex(k+1)` are the same EXPRESSION on inputs that are MATHEMATICALLY
         equal but not bitwise equal, so the snap does not make them identical by
         arithmetic — it RELOCATES the sensitive point. The ceilings differ only if
         `blockEndPpq()_k / stepPpq - 1e-6` and the same quantity for block k+1 fall on
         opposite sides of an integer, i.e. only if the true position sits within a few
         ulps of `integer + 1e-6` steps. Musical positions pile up on the INTEGERS (the
         MEASURED 24.2 % / 27123 figures below), and nothing systematically sits 1e-6
         of a step above one, so the residual is off-attractor and of measure zero
         rather than structural. What that buys is worth stating plainly: index
         agreement holds for every boundary any test or any piece of music has
         produced, and it is a different and stronger property than the placement claim
         qualified below — but it is not unconditional, and a proof built on
         "unconditional" would be built on sand. The same analysis, with the same
         conclusion, applies to `ownsPpq`'s PPQ-side re-expression; see the long note
         there for the full argument and for the one liveness consequence it carries.
         MEASURED (sweep of 20–300 BPM in 0.5 steps x 4 sample rates x 12 buffer
         sizes, 1.08e8 block edges): the two expressions disagree bitwise at 24.2% of
         edges, and 27123 of those disagreements straddle a step boundary — i.e. a
         duplicated or skipped step. Not a hypothetical.

      2. `sampleOffsetSnapSamples` — the offset itself comes from
         `blockOffsetForPpq()`, whose PPQ subtraction loses absolute precision as the
         timeline grows (worst case ~1e-6 samples after a day at 300 BPM). Where the
         true offset is an exact integer — the common case, e.g. at 60 BPM / 48 kHz a
         16th note is exactly 12000 samples — a bare `floor()` of `integer - 1e-9`
         yields `integer - 1`, so the SAME musical event lands on a different absolute
         sample at a different buffer size. Snapping up before flooring removes that
         cliff. MEASURED: without it, 8 bars at 60 BPM / 48 kHz place dozens of steps
         one sample early, and which steps move depends on the buffer size.

    ── THE SNAP-BOUNDARY WINDOW: the systematic exception (issue #37) ──────────
    Snap 1 pays for index agreement with a small, bounded asymmetry in PLACEMENT,
    and this is the only place it is written down. A boundary whose TRUE position
    lies within `stepIndexSnapSteps` BELOW a block edge is claimed by the LATER
    block (that is what the snapped ceiling does, deliberately). Its
    `blockOffsetForPpq` result is then very slightly NEGATIVE, and the hard clamp in
    `processBlock` emits it at offset 0 — up to ONE SAMPLE later than a different
    block carving, under which the same boundary falls mid-block and is placed
    exactly. So:

      - the index set is identical at every buffer size;
      - a step is not duplicated and not skipped;
      - a step's emitted sample position is identical at every buffer size EXCEPT
        for a boundary inside this window, which may land up to one sample late.

    THE FIRST TWO USED TO CARRY THE WORD "ALWAYS" AND IT HAS BEEN REMOVED
    DELIBERATELY (issue #82). They hold for every boundary any test or any piece of
    music has produced, and they are what the rest of this file is built on — but
    they are not unconditional, because the index agreement they rest on is
    off-attractor rather than arithmetic (see the qualification under snap 1 above).
    THE THREE ITEMS ARE STILL NOT THE SAME KIND OF CLAIM, which is the distinction
    the old wording was trying to protect and which must not be lost: item 3 is a
    SYSTEMATIC effect with a measurable hit rate — ordinary music lands inside the
    window routinely, and Phase 7.2 raised the rate ~8x by testing up to eight
    sub-step positions per step against block edges — whereas items 1 and 2 fail only
    on a measure-zero ulp coincidence at `edge - snapPpq` that nothing in the system
    steers events toward. Do not flatten them into one caveat; the first is a
    behaviour to design around, the second a residual to be aware of.

    THE WINDOW SCALES WITH STEP LENGTH — quote it as a formula, never as a fixed
    number:

        window (samples) = stepIndexSnapSteps x samplesPerStep
                         = 1e-6 x stepPpq x 60 x sampleRate / bpm

    which is why the bound must be stated against the COARSEST supported grid, not
    the 16th-note scaffold (§2.1 makes the grid configurable, 1/32..1/4 with
    triplet/dotted, so `stepPpq` reaches 1.5 for a dotted quarter — 6x the
    scaffold's 0.25):

        grid            bpm    sampleRate   window (samples)
        1/16 (scaffold) 120    48 kHz       0.006
        1/16            20     192 kHz      0.144
        1/4 dotted      20     192 kHz      0.864   <- worst supported case

    Under one sample at every supported combination, so the effect is bounded at one
    sample and cannot compound — but note how thin the worst case is: the scaffold row
    sits ~167x inside one sample, the worst supported row only ~1.16x, i.e. ~0.14
    samples of headroom. ANY future change that coarsens
    the grid beyond a dotted quarter, or raises `stepIndexSnapSteps`, must re-derive
    this table first: past one sample the window stops being a one-sample blemish and
    starts moving events by arbitrary amounts.

    WHY IT IS DOCUMENTED RATHER THAN CLOSED: the residual PPQ error snap 1 absorbs is
    ~1e-10 steps, so the window is ~4 orders of magnitude wider than the disagreement
    it exists to cover and cannot simply be shrunk to nothing without the duplicated/
    skipped steps coming back (24.2% of block edges disagree — see the MEASURED note
    above). Closing it properly means deriving the emission sample as a pure function
    of the boundary index against the segment anchor, so the offset can never be
    negative; that is issue #37 option (a) and is not currently scheduled. The danger
    this note defuses is not the sample — it is a future reader trusting "identical by
    construction" absolutely.

    ONE FIX CLOSES ALL THREE RESIDUALS, which is the reason to keep them named
    together: #37 (this window's one-sample placement asymmetry), #75 (no test renders
    a boundary inside it — a coverage gap Phase 7.2's ~8x hit rate made live) and #82
    (the measure-zero ulp gap at `edge - snapPpq` that the snapped ceiling relocates
    rather than removes). All three exist because the emission sample is derived from a
    BLOCK-RELATIVE offset that can go slightly negative. Deriving it from the boundary
    index against the segment anchor removes the negative offset, and with it the
    clamp, the window and the relocated gap at once. Anyone scheduling #37 should
    expect to close #75 and #82 in the same change rather than pricing them
    separately.

    ── NOTE-OFF OWNERSHIP (§5.5) ───────────────────────────────────────────────
    Every note-on emitted here is registered in `SoundingNoteTable` with the absolute
    sample its off is due (see SoundingNoteTable.h for why samples, not PPQ). The
    table is the only thing that emits note-offs, so a note cannot leak.

    ── EVERY NOTE-OFF IS *SCHEDULED*, NEVER PLACED FROM A BLOCK OFFSET (#36/#46/#48)
    THE RULE, and it is the whole of a three-instance bug family: a note-off's
    position is decided on the ABSOLUTE sample timeline, at the moment the note is
    scheduled, and the table emits it in whichever block turns out to contain that
    sample. A within-block quantity (`offset`) must NEVER decide it.

    SINCE #48 THE RULE IS STRUCTURAL, NOT A CONVENTION THIS FILE UPHOLDS.
    `SoundingNoteTable` exposes no emission method that accepts a within-block
    offset: `retireNoLaterThan` and `flush` take absolute samples plus the block to
    convert against, and both emit at `min (the entry's own due sample, what the
    caller asked for)`. A caller can shorten a note; it cannot move an off later than
    its own schedule. A fourth instance of the family is therefore not writable here.
    See "THE PLACEMENT RULE" in SoundingNoteTable.h.

    Why the rule is not optional: `offset - 1` looks like "one sample before the
    note-on", but `offset` is measured from the block head, so at offset 0 the `- 1`
    has nowhere to go and clamps back to 0. Whether a given step lands at offset 0 is
    a property of the DEVICE BUFFER SIZE, not of the music, so the same pattern
    emitted its retrigger note-off at `onSample` on some buffer sizes and at
    `onSample - 1` on others — a §1.2 violation (measured: 137 BPM / 44.1 kHz,
    off @43456 at blocks 32/64 and @43455 at 128/256/480/512/1024/2048/4096).
    Re-clamping cannot fix it: when the on is at offset 0, `onSample - 1` belongs to
    a block that has already been rendered.

    The two places a note-off could be cut short, and how each obeys the rule:

      1. SAME-PITCH RETRIGGER — `cutoffForSamePitch`, a BOUNDED LOOKAHEAD applied
         when the note is registered:
             dueOff = min (onSample + lengthSamples, nextSamePitchOnSample - 1)
         `emitDueNoteOffs` then places it exactly, in whatever block holds it —
         including the previous block, which is precisely what `offset - 1` could
         not reach. The retrigger branch in `emitStep` survives as a SAFETY NET for
         what the lookahead cannot see (see `cutoffForSamePitch`).

      2. PATTERN SWITCH — `flushForPatternSwitch` hands the table the ABSOLUTE adopt
         sample. A note still sounding there is cut short at `adoptSample - 1`; a
         note whose own off was already due earlier in the block keeps its true
         sample and is left out of the CC123 sweep (issue #48 — forcing every entry
         onto `adoptSample - 1` made both the off position and the sweep's existence
         depend on the buffer size). `processBlock` PRE-FLUSHES at the end of the
         block when the adopt point is exactly the next block's head (the offset-0
         case). See both for the one residual, documented exception.

    ── THE PURITY OF THE EMISSION CORE IS STRUCTURAL (issue #53) ───────────────
    WHAT MAKES THE LOOKAHEAD LEGITIMATE: deciding what step k+1, k+2, … will emit
    must give the same answer as eventually emitting them. That holds only while
    the emission core is a PURE function of (snapshot, pattern index, step index) —
    no cursor, no accumulator, no cache, no side effect. It is the same property
    §9's offline MIDI drag-out depends on to render bit-identically to real time.

    UNTIL ISSUE #53 THAT WAS A COMMENT. IT IS NOW THE SIGNATURE. The core is
    `evaluateStep (const PatternSnapshot&, int patternIndex, std::int64_t,
    StepRuntime)`, a FREE FUNCTION at namespace scope — declared below this class,
    defined in SequencerProcessor.cpp. It is not a member, not a static member, and
    not a friend, so EVERY piece of `SequencerProcessor` state is unreachable from
    it: `transport`, `activeSnapshot`, `activePatternIndex`, the pending-switch
    fields, `fillHeld`, and any member a later phase adds. The legal inputs arrive
    as parameters and there is no side channel. This class holds no member that
    describes a step; there is nowhere to put a cursor that the core could read.

    ── PHASE 7.1 WIDENED THAT SIGNATURE. THE ARGUMENT, IN FULL ────────────────
    The note above said "three inputs. Nothing else." There are now four: a
    by-value `StepRuntime` carrying the §12.2 FILL flag. That is an AMENDMENT to
    this note, made deliberately and in the open, because a widening slipped past
    it silently is how #53's protection is lost.

    WHY THE FLAG CANNOT BE THE OTHER THREE. FILL is pad 16 held down right now. It
    is genuinely not a function of (snapshot, pattern index, step index): the same
    step of the same pattern must fire when the pad is down and rest when it is
    not, which is the entire point of the condition. Nothing derivable from a
    snapshot expresses it.

    WHY NOT PUT IT ON THE SNAPSHOT ANYWAY. Because a snapshot is republished as a
    whole. FILL changes on press AND on release, so a snapshot field would mean two
    full ~120 KB document rebuilds and two pointer swaps per pad tap, contending
    with the piano-roll edit flow that already rebuilds on every mouse move (§4).
    That is the wrong layer for a momentary performance control; it arrives as an
    `EngineCommandType::setFillHeld` command instead (§3.4 mechanism 1).

    WHY PURITY SURVIVES, and these three properties are the whole of it:
      (a) it is a VALUE, not a pointer/reference/handle to mutable state, so the
          core cannot reach through it to anything that changes under it;
      (b) it is CONST FOR THE ENTIRE BLOCK — `processBlock` latches `fillHeld` once,
          above the step walk, into a `const StepRuntime`;
      (c) the walk passes THAT SAME VALUE to the retrigger lookahead and to the
          emission, so a prediction and the eventual emission cannot disagree about
          it within a block.
    Purity is now "same four inputs ⇒ same result", and `tests/step_purity.cpp`
    checks it over both values of the flag. A FIFTH input must clear the same three
    bars or it does not go in.

    WHAT NOW FAILS IF PHASE 7.1 REACHES FOR ONE. Writing the natural impurity —
    an RNG cursor / a `PRE` cache held as a member and read while evaluating —
    does not compile: a free function cannot name a class member, and the fix a
    compiler suggests (make it a member again) is a review-visible reversal of this
    note, not a silent edit. Widening the SIGNATURE to carry mutable state is still
    physically possible, which is why the compile-time half is backed by a
    behavioural half: tests/step_purity.cpp calls `evaluateStep` reversed, repeated,
    shuffled and interleaved across patterns and requires every result to equal the
    in-order reference. A cursor, an accumulator or a memo — wherever it is
    parked, including a file-scope static in the .cpp that the type system cannot
    see — reddens that test.

    PHASE 7.1 ADDED PROB AND COND (§12.2) TO `evaluateStep`, and both landed
    legally. The probability roll is `rng::stepHash (masterSeed, domain, stepIndex)`
    — a per-index HASH, not a draw from a running per-step RNG cursor, which is what
    §5.2's seed composition already required. `PRE`/`!PRE`, whose result depends on
    the previous gated step's outcome, RE-DERIVES that outcome from the index inside
    the call (a bounded backward walk over the snapshot's `previousGatedOffset`
    table) rather than reading a cache the emitting walk filled. See
    sequencer/StepLogic.h. If a future step's emission ever genuinely becomes
    cursor-dependent, this lookahead must be DELETED, not patched — a mispredicted
    cutoff is a wrong note length, and #36/#46/#48's buffer-size-dependent class
    returns one level up.

    FLUSH POINTS handled here — after any of them the table is empty:
      - `Transport::stoppedThisBlock()` — the primary trigger. Latched edge, visible
        in the same block because the transport head node renders before this one.
      - `Transport::positionJumpedThisBlock()` — a locate (including the rewind that
        `transportStop` performs) breaks the sample timeline the pending note-offs are
        scheduled on, orphaning them.
      - `Transport::stopGeneration()` — belt and braces. This node is spliced into the
        graph by an `UpdateKind::async` edit, so it can begin rendering having MISSED
        the block in which a stop's edge was latched. Comparing the monotonic counter
        against a locally cached value catches that; the edges alone would not.
      - PATTERN SWITCH (Phase 6) — but NOT at the block head: it lands mid-block, at
        the resolved adopt step. `flushForPatternSwitch` owns that one; see it for why
        it cannot live in `handleDiscontinuities`.
      Pool change and plugin swap are Phase 8/9 flush points.

    A pattern EDIT (a snapshot adoption) is deliberately NOT a flush point — §5.5
    lists "pattern switch", not "pattern edit", and flushing on every piano-roll
    keystroke would make the editor unusable.

    ── QUANTIZED PATTERN SWITCH (§5.2 "Quantized apply", §6.1) ─────────────────
    `queuePatternSwitch` records a REQUEST only (`applyCommand` runs during the
    transport head node's drain, which is strictly BEFORE `Transport::beginBlock()`,
    so every latched getter still describes the PREVIOUS block — nothing may be
    computed there). The request is RESOLVED to an absolute step index at the top of
    the next `processBlock`, against latched values, and FIRES inside the step walk
    when the walk reaches that index. Resolution is one-shot: re-resolving every
    block would let `ceil` chase the playhead forever once it passed the target. A
    discontinuity INVALIDATES the resolution but keeps the request, because a stop or
    locate destroys the timeline the step index was anchored to.

    ── MIDI CONTRACT ───────────────────────────────────────────────────────────
    No audio buses. The incoming `MidiBuffer` is NEVER cleared: MidiIn's live QWERTY
    and hardware notes pass straight through so the keyboard keeps working today, and
    Phase 8's THRU mode consumes them as the note pool. Generated events are added at
    sample offsets strictly inside `[0, numSamples)`; `juce::MidiBuffer` keeps the
    merged result sorted by sample position (§5.5).

    ── RT-SAFETY ───────────────────────────────────────────────────────────────
    `processBlock` allocates nothing, locks nothing, logs nothing and constructs no
    `juce::String` or `juce::MidiMessage` (all MIDI is assembled from raw bytes). The
    outgoing buffer's capacity is warmed unconditionally every block — the same idiom
    and the same reason as `MidiInputProcessor`: `ensureSize` early-returns once
    satisfied, and re-checking every block re-warms the FRESH pool buffer the graph
    hands this node after a render-sequence rebuild (which a first-block-only flag
    would miss). */
class SequencerProcessor : public juce::AudioProcessor, public ICommandSink
{
public:
    // ── THE DOCUMENTED DEFAULTS (formerly "the scaffold") ────────────────────
    // These were Phase 5.2's hardcoded pattern. Phase 6 did NOT delete them: the
    // default-constructed `PatternDocument` reproduces every one of them exactly
    // (see the ctor note in PatternDocument.h — the equivalence FALLS OUT of the
    // pipeline rather than being maintained by hand), and ~73 KB of existing timing
    // and lifecycle tests are written against them. They are kept as the NAMES of
    // the default configuration, and a later task renames them accordingly. The
    // live values now come from the adopted `PatternSnapshot`; nothing in
    // `evaluateStep` reads the constants below.

    /** Default pattern length in steps (every lane's default `length`). */
    static constexpr int scaffoldNumSteps = 16;

    /** Default step grid: a 16th note (quarter notes per step). The LIVE value is
        `PatternSnapshot::gridStepPpq`; this is the fallback when no snapshot has
        been adopted, and the value the default document publishes. */
    static constexpr double scaffoldStepPpq = 0.25;

    /** Default stub-pool root (middle C) — `PoolSnapshot::sorted[0]` of the Phase 6
        stub pool. */
    static constexpr int scaffoldRootNote = 60;

    /** Default VEL lane value (`laneDefault (LaneId::vel)`). */
    static constexpr int scaffoldVelocity = 100;

    /** Default LEN lane value as a fraction of the step: 50% (§12.1 stores it as a
        percentage, 1–400, where >100% means tie/legato). */
    static constexpr double scaffoldGateFraction = 0.5;

    /** Default MIDI output channel (`PatternSetState::outputChannel`). */
    static constexpr int scaffoldChannel = 1;

    /** Constructs the node with NO audio buses (MIDI-only). */
    SequencerProcessor ();

    // MESSAGE-THREAD ONLY: retires the held snapshot (see the note on the
    // destructor's body).
    /** ~SequencerProcessor. */
    ~SequencerProcessor () override;

    // MESSAGE-THREAD ONLY: wiring. Injects the graph-owned transport this node reads
    // its musical position from and the graph-owned `PatternChannel` it adopts
    // pattern snapshots from. Call once, before the node joins the graph and before
    // playback. Both pointers are non-owning and must outlive this node —
    // `EngineGraph` guarantees that by member declaration order. `channelToFollow`
    // may be null (the node then emits nothing but still passes THRU MIDI).
    /** Sets the transport driving the step walk and the pattern-snapshot channel. */
    void setSharedState (const Transport* transportToFollow, PatternChannel* channelToFollow) noexcept;

    // MESSAGE-THREAD ONLY (observation): the table is AUDIO-THREAD-OWNED state, so a
    // message-thread read can race a concurrent block. Exposed for HEADLESS TESTS
    // that drive the graph themselves and assert the §5.5 invariant
    // ("table empty after every flush point"); the UI must never read it.
    /** The sounding-note table. Test/observation only — see the threading note. */
    const SoundingNoteTable& soundingNotes () const noexcept { return sounding; }

    // MESSAGE-THREAD ONLY (observation): AUDIO-THREAD-OWNED, same caveat as
    // `soundingNotes()` — a read concurrent with a block can tear. Headless tests
    // only; the UI reads the active pattern from `EngineSnapshot` (§10.1).
    /** The pattern index currently being played, 0..`maxPatterns`-1. */
    int activePattern () const noexcept { return activePatternIndex; }

    // MESSAGE-THREAD ONLY (observation): same caveat again.
    /** The snapshot this node has adopted, or nullptr before the first adoption. */
    const PatternSnapshot* adoptedSnapshot () const noexcept { return activeSnapshot; }

    // ── AudioProcessor overrides ─────────────────────────────────────────────

    /** Returns the node's display name. */
    const juce::String getName () const override { return "ARPBOX Sequencer"; }

    // MESSAGE-THREAD ONLY: called with audio stopped. Never on the audio thread.
    /** Drops any stale tracked notes (a fresh prepare means the previous graph
        configuration's notes can no longer be released) and re-seeds the
        stop-generation watermark. Nothing is allocated — the table is fixed-size. */
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;

    // MESSAGE-THREAD ONLY: release.
    /** Releases resources and forgets tracked notes (audio has stopped; there is no
        buffer left to emit their offs into).

        DELIBERATELY KEEPS THE ADOPTED SNAPSHOT — see the destructor's body for why
        retiring it here would silence the node across a device change. */
    void releaseResources () override;

    // RT-SAFE: audio thread. Allocation-free, lock-free, no logging, no juce::String.
    /** Adopts a newly published `PatternSnapshot` if one is waiting, passes incoming
        MIDI through untouched, flushes on a transport discontinuity, resolves and
        fires a queued pattern switch, emits the pattern's note-ons for every step
        boundary inside this block, and emits every note-off that comes due. Renders
        no audio. */
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    // RT-SAFE: audio thread. The graph runs float; this double path is unused.
    /** Double-precision path — must never be called (graph is float). */
    void processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) override;

    /** MIDI-only: only the no-audio-bus layout is supported. */
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    /** No editor (headless engine node). */
    juce::AudioProcessorEditor* createEditor () override { return nullptr; }
    /** Reports that no editor exists. */
    bool hasEditor () const override { return false; }

    /** Consumes MIDI (the THRU path, and Phase 8's note pool). */
    bool acceptsMidi () const override { return true; }
    /** Emits the generated arp MIDI to the synth. */
    bool producesMidi () const override { return true; }
    /** Not a tail-producing effect. */
    double getTailLengthSeconds () const override { return 0.0; }

    /** Single (default) program. */
    int getNumPrograms () override { return 1; }
    /** Current program index. */
    int getCurrentProgram () override { return 0; }
    /** No-op program change. */
    void setCurrentProgram (int) override {}
    /** No program names. */
    const juce::String getProgramName (int) override { return {}; }
    /** No-op program rename. */
    void changeProgramName (int, const juce::String&) override {}

    // MESSAGE-THREAD ONLY: sequencer state is project-level (§8.1 `patterns[16]`),
    // not a plugin blob; persistence arrives in Phase 11.
    /** No persisted state. */
    void getStateInformation (juce::MemoryBlock&) override {}
    /** No persisted state. */
    void setStateInformation (const void*, int) override {}

    // ── ICommandSink ─────────────────────────────────────────────────────────

    // RT-SAFE: audio thread, from the transport head node's drain — which runs
    // BEFORE `Transport::beginBlock()`, so every latched transport getter still
    // describes the PREVIOUS block. This method therefore only RECORDS the request;
    // all timing arithmetic happens in `processBlock` (see the quantized-switch note
    // in the class comment). Ignores every command type it does not own.
    /** Applies `queuePatternSwitch`; ignores all other types. */
    void applyCommand (const EngineCommand& command) noexcept override;

private:
    // `maxRetriggerLookaheadSteps` USED TO LIVE HERE, as a private class constant
    // with a hand-written derivation. It is now at namespace scope above, fully
    // derived from the lane ranges, because `tests/step_purity.cpp` has to name it
    // — see the note on the constant itself (issue #69).

    // RT-SAFE: audio thread. THE ONLY SNAP-THEN-FLOOR IN THIS FILE — the sample-side
    // counterpart of `snappedCeiling`'s "THE ONLY CEILING IN THIS FILE", and for the
    // identical reason. Absolute sample at which musical position `ppq` falls:
    //
    //     blockStartSample + floor (blockOffsetForPpq (ppq) + sampleOffsetSnapSamples)
    //
    // UNTIL PHASE 7.2 THIS IDIOM WAS WRITTEN OUT TWICE — once in
    // `stepBoundarySample` and once inline in the step walk — and issue #54 exists
    // because nothing but tests/boundary_agreement.cpp held the two copies to each
    // other. There is now one copy and both sites call it, so the class of divergence
    // #54 names is no longer writable here. `boundary_agreement` remains the check
    // that the walk's PLACEMENT and the lookahead's PREDICTION still describe the
    // same boundary; it is no longer the only thing standing between them.
    //
    // DELIBERATELY NOT CLAMPED into the block — so it is valid for a position
    // belonging to a LATER (or earlier) block, which is what `cutoffForSamePitch`,
    // the pattern-switch pre-flush and the walk's own ownership test all need. The
    // walk applies the #37 lower clamp itself, at its own call site, where the
    // reasoning for it lives.
    //
    // BUFFER-SIZE INDEPENDENT: `blockStartSample + (ppq - blockStartPpq) / ppqPerSample`
    // is the same absolute value however the timeline was carved (Transport.h: PPQ at
    // an absolute sample is bit-identical across carvings), and the residual is ~1e-6
    // samples worst case — two orders of magnitude under `sampleOffsetSnapSamples`.
    std::int64_t sampleForPpq (double ppq, std::int64_t blockStartSample) const noexcept;

    // RT-SAFE: audio thread. Absolute sample at which step boundary `index` falls.
    // A NAMED SPECIALISATION of `sampleForPpq` at `index * stepPpq` and nothing more —
    // it holds no arithmetic of its own, which is the point (see above).
    std::int64_t stepBoundarySample (std::int64_t index, double stepPpq, std::int64_t blockStartSample) const noexcept;

    // RT-SAFE: audio thread. The scheduled note-off sample for a note starting at
    // `onSample` whose natural end is `naturalDueSample`:
    //
    //     min (naturalDueSample, nextSamePitchOnSample - 1)
    //
    // i.e. §5.5's 1-sample retrigger gap, decided HERE on the absolute timeline
    // rather than later from a block offset (see "EVERY NOTE-OFF IS SCHEDULED" in
    // the class comment — this is the issue #46 fix).
    //
    // ── PHASE 7.2 REWROTE THE SCAN. THREE CHANGES, ALL FORCED BY DISPLACEMENT ──
    // Sub-step displacement breaks the one property the original loop rested on:
    // that onsets increase monotonically in INDEX order. Step k displaced by +0.5
    // places its child 7 at k+1.375 while step k+1 displaced by -0.5 places its
    // child 0 at k+0.5 — an inversion of 0.875 steps. So:
    //
    //   1. THE EARLY `break` ON `boundary > naturalDueSample` IS GONE. It concluded
    //      "no LATER index can shorten this note", which is now false. The whole
    //      bounded window is scanned and the MINIMUM qualifying onset wins.
    //   2. IT SCANS `(index, child)` PAIRS, AND BACKWARD AS WELL AS FORWARD — a
    //      note emitted for step k+1 can be overtaken by a not-yet-sounded child of
    //      step k. See `retriggerScanBackSteps` for why one step back is exact.
    //   3. IT INCLUDES THIS STEP'S OWN LATER CHILDREN (`ahead == 0`, `c >
    //      childIndex`), which is where a ratchet's 1-sample gaps come from.
    //
    // BOUNDED: `[-retriggerScanBackSteps, lookaheadSteps]` indices, each offering
    // at most `maxRatchetChildren` children — so at most nine `evaluateStep` calls
    // and 72 position derivations per emitted note, with `lookaheadSteps` computed
    // per note from its actual reach (a 50 % gate looks one step ahead, not seven).
    // The FORWARD scan stops short of a RESOLVED pattern switch, because
    // `evaluateStep` would otherwise be called with the OUTGOING pattern index for
    // steps the INCOMING pattern will play; the BACKWARD scan stops at the last
    // switch that actually fired, for the mirror-image reason.
    //
    // VALID ONLY BECAUSE `evaluateStep` IS PURE — see "THE PURITY OF THE EMISSION
    // CORE IS STRUCTURAL" in the class comment before changing its signature.
    //
    // `emission` and `childIndex` identify WHICH note is being scheduled: the
    // caller's own emission is reused for the `ahead == 0` case rather than
    // re-evaluated, so a note and the prediction made on its behalf cannot disagree
    // about the step they both came from.
    //
    // `runtime` is the BLOCK'S LATCHED VALUE, handed down from `processBlock` rather
    // than re-read from the `fillHeld` member: the prediction and the emission it
    // predicts must see the identical FILL flag, or the lookahead can schedule a
    // cutoff against a step the emission will not play.
    std::int64_t cutoffForSamePitch (const StepEmission& emission,
                                     std::int64_t stepIndex,
                                     int childIndex,
                                     int channel,
                                     int note,
                                     std::int64_t onSample,
                                     std::int64_t naturalDueSample,
                                     double stepPpq,
                                     std::int64_t blockStartSample,
                                     int lookaheadSteps,
                                     StepRuntime runtime) const noexcept;

    // RT-SAFE: audio thread. Two integer comparisons and at most two multiplies.
    //
    // ── THE FOURTH INSTANCE OF THE #36/#46/#48 FAMILY (issue #76) ────────────
    // Is an event of step `index`, sounding at `eventPpq`, DISCONTINUED by a quantized
    // pattern switch? True iff `index` belongs to the outgoing pattern (it is below the
    // adopt index) while the event itself sounds at or after the adopt boundary. Only
    // a ratchet child can be in that position — see `eventReachesAdoptBoundary` in the
    // .cpp for the 0.5 + 0.875 = 1.375 geometry.
    //
    // WHY THE WALK NEEDS THIS AT ALL. `evaluateStep` is called with
    // `activePatternIndex`, and once a switch has FIRED that is the INCOMING pattern —
    // while `stepScanBack` makes later blocks re-visit the pre-switch index to pick up
    // its remaining children. Those children were then described by the incoming
    // pattern's lanes. Whether a given child was reached before or after the switch
    // fired depended on whether its sample and the adopt sample shared a block, i.e.
    // on the DEVICE BUFFER SIZE.
    //
    // MEASURED (300 BPM / 48 kHz, 1/32 grid = 1200 samples/step, RATCHET 8, MICRO +50,
    // a `patternEnd` switch resolving to step 61 = sample 73200): step 60's children
    // 4..7 sit at 73200 / 73350 / 73500 / 73650, at or past the adopt point. Child 5
    // at 73350 carried velocity 111 (the incoming pattern) at blocks 32, 64, 96, 128
    // and 256, and velocity 100 (the outgoing pattern) at blocks 480, 512, 1024, 2048
    // and 4096. The switch flush compounded it: it cut whichever of those children
    // happened to be in the table when the switch fired — child 4 to a zero-length
    // note at 73200 at block 32, child 7 to one at 73650 at block 4096.
    //
    // WHY SUPPRESSION RATHER THAN "EVALUATE AGAINST THE OUTGOING PATTERN". Both make
    // the velocity deterministic, but only suppression also fixes the flush half, and
    // it fixes it STRUCTURALLY: nothing that starts at or after the adopt point is ever
    // in the table when the switch flushes, so there is nothing for the flush to cut
    // buffer-size-dependently and §5.5's "the table must be empty after a flush point"
    // needs no exception. It is also what §5.5 means — the outgoing pattern is
    // DISCONTINUED there, so four leftover pattern-A ratchet clicks on top of pattern
    // B's first step are not a rendering anyone asked for. The alternative would have
    // had to emit them and then immediately release them (co-located note-on/note-off
    // pairs — the very blemish `emitNote` documents as issue #46's shape) or leave them
    // in the table across the flush.
    //
    // AND WHY THIS CLOSES THE VELOCITY HALF WITHOUT A SECOND GUARD: blocks tile PPQ
    // monotonically, so an event whose PPQ is BELOW the adopt boundary cannot be owned
    // by a block after the one that owns the boundary — the block that fires the
    // switch. Every surviving event of a pre-switch step is therefore emitted before
    // the switch fires, with `activePatternIndex` still the outgoing pattern. The
    // walk needs no per-index pattern lookup; the suppression makes it unnecessary.
    // (`cutoffForSamePitch` still needs its own guards, because a LOOKAHEAD evaluates
    // arbitrary indices rather than only owned ones.)
    bool discontinuedByPatternSwitch (std::int64_t index, double eventPpq, double stepPpq) const noexcept;

    // RT-SAFE: audio thread. Emits ONE of a step's notes — `emission.notes
    // [childIndex]`: applies the same-pitch retrigger policy (§5.5), emits the
    // note-on and registers its scheduled note-off with the table.
    //
    // PER NOTE, NOT PER STEP, SINCE PHASE 7.2. A step now carries up to
    // `maxRatchetChildren` notes at distinct sub-step positions, and each is owned
    // by whichever block CONTAINS it — which at the worst supported step (a dotted
    // quarter at 20 BPM / 192 kHz, 864 000 samples) puts child 7 some 756 000
    // samples and many blocks after its parent. So ownership, offset conversion and
    // emission are all per `(index, child)`, and the walk calls this once per owned
    // child rather than once per step.
    //
    // `blockStartSample` and `numSamples` describe THIS block and are passed
    // explicitly even though `blockStartSample == onSample - offset` recovers one
    // from the others: the retrigger path places its note-off through
    // `SoundingNoteTable::retireNoLaterThan`, which works in absolute samples, and
    // an implicitly reconstructed block origin in determinism-critical code is a
    // coupling waiting to be broken by a future edit to the offset arithmetic.
    // `stepIndex` and `stepPpq` are what the same-pitch lookahead needs to describe
    // the surrounding steps.
    void emitNote (const StepEmission& emission,
                   int childIndex,
                   juce::MidiBuffer& midi,
                   std::int64_t stepIndex,
                   int offset,
                   std::int64_t onSample,
                   std::int64_t blockStartSample,
                   int numSamples,
                   double stepPpq,
                   double samplesPerStep,
                   StepRuntime runtime) noexcept;

    // RT-SAFE: audio thread. Flushes the sounding-note table if this block carries a
    // transport discontinuity, and invalidates any resolved pattern switch (the step
    // index it was anchored to no longer means what it did). Returns true if a flush
    // happened.
    //
    // `blockStartSample` / `numSamples` describe THIS block: the flush releases from
    // the block head on the ABSOLUTE timeline (`SoundingNoteTable::flush` takes a
    // sample, never an offset — see "THE PLACEMENT RULE" there).
    bool handleDiscontinuities (juce::MidiBuffer& midi, std::int64_t blockStartSample, int numSamples) noexcept;

    // RT-SAFE: audio thread. Resolves a recorded `queuePatternSwitch` request into an
    // absolute `adoptStepIndex`, ONCE, against the transport's latched block-start
    // position. No-ops when there is no request, when one is already resolved, or when
    // the request targets the pattern already playing.
    void resolvePendingSwitch (double stepPpq) noexcept;

    // RT-SAFE: audio thread. §5.5 flush for the pattern switch, released one sample
    // BEFORE the adopt point (the same 1-sample-gap discipline as the same-pitch
    // retrigger in `emitStep`).
    //
    // TAKES THE ABSOLUTE `adoptSample`, NOT A BLOCK OFFSET — issue #46. The old
    // signature took `offset` and did `jmax (0, offset - 1)`, which collapses the gap
    // whenever the adopt point lands at offset 0, i.e. whenever the DEVICE BUFFER
    // SIZE happens to put a block head there. Deciding on the absolute timeline and
    // converting once, here, removes that dependency for every caller.
    //
    // WHY IT IS NOT PART OF `handleDiscontinuities`: that runs at the BLOCK HEAD and
    // flushes at offset 0. A pattern switch lands MID-BLOCK, at the resolved step's
    // own offset — flushing it at the block head would cut every note in the current
    // pattern short by up to one buffer.
    void flushForPatternSwitch (juce::MidiBuffer& midi,
                                std::int64_t adoptSample,
                                std::int64_t blockStartSample,
                                int numSamples) noexcept;

    // RT-SAFE: audio thread. Forgets the pending switch request entirely.
    void clearPendingSwitch () noexcept;

    // Injected, non-owning (graph-owned) shared state.
    const Transport* transport = nullptr;     ///< Musical clock; read-only from here.
    PatternChannel* patternChannel = nullptr; ///< Snapshot publish/adopt/retire channel (§3.4 mech. 3).

    // Audio-thread-owned note lifecycle authority (§5.5).
    SoundingNoteTable sounding;

    // ── Adopted pattern state (audio-thread owned) ───────────────────────────

    /** The snapshot in use. Owned by this node between `PatternChannel::adopt` and
        the next adoption / retirement; NEVER deleted here (code-style.md: retired
        snapshots are freed on the message thread). */
    const PatternSnapshot* activeSnapshot = nullptr;

    /** Pattern being played, 0..`maxPatterns`-1. Seeded from the FIRST adopted
        snapshot's `startPatternIndex` and thereafter changed ONLY by a quantized
        pattern switch — a later adoption (i.e. a document edit) must not yank the
        user back to the start pattern. */
    int activePatternIndex = 0;
    bool patternIndexSeeded = false;

    // ── Pending quantized pattern switch (§5.2, §6.1) ────────────────────────
    // `pendingRequested` survives a discontinuity; `pendingResolved` does not.

    bool pendingRequested = false;                    ///< A `queuePatternSwitch` is outstanding.
    int pendingPatternIndex = 0;                      ///< Its destination pattern.
    QuantizeMode pendingQuantize = QuantizeMode::bar; ///< Its quantize mode (§5.2 default: bar).
    bool pendingResolved = false;                     ///< `adoptStepIndex` is valid.
    std::int64_t adoptStepIndex = 0;                  ///< Global step index the switch lands on.

    /** The step index of the LAST pattern switch that actually FIRED, or
        `INT64_MIN` if none has.

        ── WHY IT EXISTS: THE BACKWARD RETRIGGER SCAN NEEDS A FLOOR ────────────
        `cutoffForSamePitch` now looks one step BACKWARD (see
        `retriggerScanBackSteps`), and it evaluates those steps against
        `activePatternIndex` — which, immediately after a switch fires, is the
        INCOMING pattern. A step below the adopt point belongs to the OUTGOING
        one, so predicting against it would describe lanes that step never played.
        `pendingResolved`/`adoptStepIndex` cannot answer this: they are CLEARED the
        moment the switch fires, which is exactly when the backward scan starts
        needing the answer.

        Nothing musical depends on it beyond bounding the scan, and the bound
        errs safe: a skipped candidate means a note keeps its natural length
        instead of being cut short, and the switch's own flush releases it anyway.
        RESET BY A DISCONTINUITY — a locate re-anchors the step timeline, so a
        remembered index from before it no longer names the same musical moment. */
    std::int64_t lastFiredAdoptStepIndex = std::numeric_limits<std::int64_t>::min ();

    /** Last observed `Transport::stopGeneration()`. Compared every block so a stop
        whose latched edge this node MISSED (async graph insertion) still triggers a
        flush. Seeded on the first block so joining a session that has already been
        stopped does not fire a spurious flush. */
    std::uint64_t lastSeenStopGeneration = 0;
    bool stopGenerationSeeded = false;

    /** §12.2 FILL — pad 16 held. Set by `EngineCommandType::setFillHeld`.

        DELIBERATELY NOT AN ATOMIC, and not for performance. This is plain
        AUDIO-THREAD-OWNED state with exactly the ownership `pendingRequested` and
        `pendingPatternIndex` have: `applyCommand` runs during the TRANSPORT HEAD
        NODE's command drain, which the graph renders strictly BEFORE this node, so
        the write and every read happen on the audio thread, in order, within one
        block. The message thread never touches this member — it posts a command.
        An atomic here would advertise a race that does not exist and invite the
        next reader to look for the other side of it.

        `processBlock` LATCHES it once into a `const StepRuntime` above the step
        walk; nothing below reads the member. See the latch site for why. */
    bool fillHeld = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SequencerProcessor)
};

/** ONE note a step contributes to the output — a ratchet child, or (at
    `RATCHET == 1`, the default) the step's only note.

    `gateFractionOfStep` is shaped like the LEN lane (§12.1: 1–400 % of the step,
    >100 % meaning tie/legato) but is expressed against THIS NOTE'S OWN TIME SLOT
    rather than the whole step — see `StepEmission::noteCount`. */
struct StepNote
{
    int note = SequencerProcessor::scaffoldRootNote;     ///< MIDI note number, 0..127.
    int velocity = SequencerProcessor::scaffoldVelocity; ///< 1..127.

    /** This note's onset as a fraction of ONE STEP past the step's (already
        displaced) onset — `[0, 1)`. Child `c` of `n` sits at `c / n`.

        ── STORED RATHER THAN DERIVED FROM `noteCount`, DELIBERATELY ───────────
        Ratchet children are evenly spaced, so `c / noteCount` would reproduce
        this exactly and the field looks redundant today. It is not: §12.4's
        STRUM ("chord-mode roll spread", span + curve) is the next thing to write
        into this list and it places notes UNEVENLY, so deriving the position now
        would mean reshaping this struct — and every golden that depends on its
        layout — a second time. One shape, two features.

        DERIVED INDEPENDENTLY FROM THE PARENT, never by cumulative addition of a
        rounded slot: at `noteCount == 7` a slot is 822.857 samples and adding a
        rounded slot seven times drifts child 6 by ~6 samples. See the child-onset
        derivation in the step walk. */
    double positionInStep = 0.0;

    /** LEN as a fraction of this note's OWN slot length, i.e. already divided by
        `noteCount`. See `StepEmission::noteCount` for why LEN is per slot. */
    double gateFractionOfStep = SequencerProcessor::scaffoldGateFraction;

    /** §5.4 X-RAY bitmask — see sequencer/Provenance.h. Always carries
        `provenance::core`; children `c > 0` also carry `provenance::ratchetChild`.
        Phase 12 ORs in the operator-slot and constraint bits. */
    std::uint32_t provenance = provenance::none;
};

/** What one step boundary contributes to the output.

    ─── THE PIPELINE SEAM ──────────────────────────────────────────────────────
    `evaluateStep()` below is the ONE function each pipeline phase grows. Phase 6
    filled it with L0+L1 (note pool → pattern core lanes); Phase 7.1 added L2's
    probability and trig conditions; Phase 7.2 turned the single note into this
    FIXED-SIZE LIST (ratchets) and added the sub-step displacement (micro-timing +
    swing); Phase 12+ adds L3 (the operator stack) and the constraint gate. The
    step WALK and the MIDI EMISSION either side of it do not change shape again:
    that is the point of splitting them here.

    ── FIXED-SIZE, TRIVIALLY COPYABLE, AND THE `static_assert` IS LOAD-BEARING ──
    A `std::vector<StepNote>` is the obvious way to express "a small list", and it
    would be an ALLOCATION ON THE AUDIO THREAD — this type is constructed inside
    `evaluateStep`, which runs per step per block, and is additionally constructed
    up to nine times per emitted note by the retrigger lookahead. The
    `static_assert` below turns that reach for a container into a BUILD ERROR
    rather than an allocation the `pattern_alloc_guard` suite has to catch after
    the fact. Keep every member trivially copyable.

    The non-`gate` fields carry the documented defaults purely so a
    default-constructed value is well formed; `gate == false` means the step emits
    nothing, `noteCount` is 0, and nothing reads `notes`. */
struct StepEmission
{
    bool gate = false;                                 ///< False ⇒ this step emits nothing.
    int channel = SequencerProcessor::scaffoldChannel; ///< MIDI channel, 1..16 (shared by all this step's notes).

    /** Live entries in `notes`, `[0, maxRatchetChildren]`. 0 iff `! gate`.

        ── LEN APPLIES TO A CHILD'S OWN TIME SLOT, NOT TO THE WHOLE STEP ───────
        A step subdivided into `noteCount` children gives each child a slot of
        `1 / noteCount` steps, and `StepNote::gateFractionOfStep` is LEN measured
        against THAT. Two consequences, both wanted:

          * At `noteCount == 1` — RATCHET's default, and every pattern in every
            pre-7.2 golden — the slot IS the step, so the arithmetic is identical
            and no existing note changes length by a sample.
          * It is the only reading under which LEN > 100 % ties ACROSS CHILDREN
            the way LEN > 100 % already ties across steps. Under the alternative
            (LEN against the whole step) a 50 % gate on an 8-ratchet step would
            hold each child for four slots, so seven of the eight children would
            be immediately cut short by the next one and LEN would have no
            audible effect below 800 %.

        ── A SUPPRESSED CHILD IS ABSENT, NOT SILENT ───────────────────────────
        Per-child probability (§5.1 L2) can drop children, so `noteCount` is the
        number that PASSED, not the RATCHET lane value, and `notes[c]` is the c-th
        SURVIVING note. That is why `positionInStep` has to be stored: the list is
        compacted, so a survivor's index no longer determines where it sits. */
    int noteCount = 0;

    /** THE COMPOSED SUB-STEP DISPLACEMENT of this step's onset, in STEPS —
        MICRO (§12.1, ±50 % of a step) plus swing (§8.1 `transport.swingPct`),
        summed and then clamped ONCE to ±`maxSubStepShiftSteps`. Applies to the
        whole step, children included (they are placed relative to it).

        ── WHY IT IS RETURNED FROM HERE RATHER THAN READ IN THE WALK ───────────
        Three reasons, and the second is the load-bearing one:

          1. THE ONE-LANE-READ-PATH DISCIPLINE. `evaluateStep`'s in-body manifest
             is the authoritative list of which lanes are read; a lane read
             out-of-band in the walk is invisible to it and to every purity test
             written against it.
          2. THE RETRIGGER LOOKAHEAD PREDICTS THE *SHIFTED* ONSET. It decides a
             note-off from where the next same-pitch note-on will land, and it
             learns that from this field. Reading MICRO in the walk instead would
             leave the lookahead predicting UNSHIFTED grid boundaries — up to half
             a step wrong, i.e. a worse-behaved version of exactly what issue #46
             fixed.
          3. Both inputs are already inside the legal parameter set (MICRO is a
             lane, swing is a snapshot field), so nothing about purity is
             stretched to accommodate them.

        Zero for a default-constructed value and for a straight (`swingPct == 50`)
        pattern with MICRO at its 0 default — bit-zero, so `gridPpq + 0.0 *
        stepPpq` is `gridPpq` and the pre-7.2 goldens cannot move. */
    double shiftSteps = 0.0;

    /** This step's notes, `[0, noteCount)` valid. Sized by the RATCHET lane's
        §12.1 ceiling, so the worst legal case fits with no allocation. */
    std::array<StepNote, static_cast<std::size_t> (maxRatchetChildren)> notes {};
};

static_assert (std::is_trivially_copyable_v<StepEmission>,
               "StepEmission is built on the AUDIO THREAD, once per step per block plus up to nine times "
               "per emitted note by the retrigger lookahead. A non-trivially-copyable member means "
               "somebody reached for a std::vector (or a juce::Array, or a std::string) for the note list "
               "or the provenance mask — which is an audio-thread allocation. Keep every member POD.");
static_assert (std::is_trivially_copyable_v<StepNote>, "See the note on StepEmission.");

// RT-SAFE: audio thread. Allocation-free, lock-free, branch-predictable table reads.
//
// ── THE PURE EMISSION CORE (issue #53) ───────────────────────────────────────
// A FREE FUNCTION ON PURPOSE, and the purpose is enforcement rather than tidiness.
// The retrigger lookahead (`SequencerProcessor::cutoffForSamePitch`) predicts what
// steps k+1, k+2, … will emit and schedules a note-off from the prediction; the
// prediction is only sound while evaluating a step is a pure function of the three
// parameters below. Expressing that as a comment on a member function is what
// issue #53 was filed about. Expressed as a free function it is checked by the
// compiler: there is no `this`, so no member of `SequencerProcessor` — present or
// future, mutable or not — is nameable here.
//
// THE WHOLE CONTRACT, and nothing may be added to either side of it:
//   inputs  = (snapshot, patternIndex, stepIndex, runtime). Nothing else. No
//             transport, no block, no cursor, no bar counter passed by a side
//             channel. `runtime` is a by-value POD, const for the whole block, and
//             passed IDENTICALLY to the lookahead and to the emission — see the
//             Phase 7.1 amendment in the class comment for why it clears the bar.
//   outputs = the returned value. Nothing else. No stored state, no cache primed
//             for the next call, no RNG stream advanced as a side effect.
// Same four inputs ⇒ same result, on any thread, in any order, any number of
// times, in real time or in §9's offline drag-out pass. tests/step_purity.cpp
// requires exactly that of whatever this function grows into.
//
// @param snapshot      The adopted pattern set. Never null (the callers null-check).
// @param patternIndex  Which of the 16 patterns is playing; clamped by `pattern()`.
// @param stepIndex     The GLOBAL step index — it keeps counting across loops.
// @param runtime       The block's latched live inputs (§12.2 FILL). See StepLogic.h.
/** Decides what global step `stepIndex` of `patternIndex` emits. */
StepEmission
evaluateStep (const PatternSnapshot& snapshot, int patternIndex, std::int64_t stepIndex, StepRuntime runtime) noexcept;
} // namespace arpbox::engine
