#pragma once

#include "../EngineGuiGuard.h"
#include "../midi/NotePool.h"

#include <array>
#include <cstdint>

namespace arpbox::engine
{
// ─────────────────────────────────────────────────────────────────────────────
// PatternTypes.h — the shared vocabulary of the L1 pattern core (ARCHITECTURE
// §5.1, §12.1, §12.3). Types and constexpr clamps only: no state, no threading,
// no allocation, nothing that can drift. Everything downstream (PatternDocument,
// PatternSnapshot, the operator stack, the serializer) speaks these names.
// ─────────────────────────────────────────────────────────────────────────────

/** Maximum steps in a pattern (ARCHITECTURE §2.1 "Up to 64 steps", §5.1 "≤64 steps"). */
inline constexpr int maxSteps = 64;

/** Patterns per project (ARCHITECTURE §2.1 "16 patterns/project", §8.1 `patterns[16]`). */
inline constexpr int maxPatterns = 16;

/** Largest per-lane clock division (ARCHITECTURE §5.1 "independent length + clock
    division ⇒ polymeter"). Musically /1 /2 /3 /4 /6 /8 covers the useful range, and
    8 matches the RATCHET lane's ceiling (§12.1), so both fit one bound. */
inline constexpr int maxLaneDivision = 8;

// `maxPoolSize` is defined in ../midi/NotePool.h (see the rationale there) and is
// in scope for every includer of this header.

/** The 11 parameter lanes of ARCHITECTURE §12.1, in table order.

    APPEND-ONLY. These values are the lane ordinals written into `project.json`
    (§8.1 "lanes (per-lane: length, division, steps[])"), so renumbering silently
    reinterprets every saved project. New lanes go before `count`, never between
    existing entries. */
enum class LaneId : std::uint8_t
{
    gate = 0, ///< on/off — trig lane; the euclidean generator writes it.
    pitch,    ///< −24..+24 degrees — pool index offset (THRU) / absolute degree (SELF).
    oct,      ///< −4..+4 octaves.
    vel,      ///< 1–127 MIDI velocity.
    len,      ///< 1–400 % of step; >100% ⇒ tie/legato (§5.5).
    /** 1–8 subdivisions. EVALUATED (Phase 7.2) — `ratchetChildCount` in
        sequencer/StepLogic.h fills `StepEmission`'s note list from it, and the
        RATCHET CEILING SIZES THE WHOLE SUB-STEP GEOMETRY: `maxRatchetChildren`,
        `maxChildAheadSteps`, the step walk's scan widening and the retrigger
        lookahead's reach are all `constexpr` expressions of `laneRange
        (LaneId::ratchet).hi` (sequencer/SequencerProcessor.h). Raising this lane's
        range is therefore not a local change — the `static_assert`s there are what
        stop it being made as one. */
    ratchet,

    /** −50..+50 % step. EVALUATED (Phase 7.2) — composed with the project-level
        `swingPct` into `StepEmission::shiftSteps` and clamped ONCE, inside
        `evaluateStep`, to `maxSubStepShiftSteps`. */
    micro,
    prob, ///< 0–100 %. EVALUATED (Phase 7.1) — sequencer/StepLogic.h.
    cond, ///< TrigCondition ordinal. EVALUATED (Phase 7.1) — sequencer/StepLogic.h.
    /** 0–127 mod source. **EVALUATED SINCE PHASE 7.1**, three phases before the
        Phase 14 mod matrix it was originally annotated for. User decision D7 gives
        §12.2's `NEI`/`!NEI` conditions their v1 meaning as `MOD A >= 64` at the
        step (`neighbourModThreshold` in sequencer/StepLogic.h), which makes this
        lane AUDIBLE now. Phase 14.1 must not assume MOD A is an unread lane it can
        repurpose freely: changing what this value means changes which steps fire
        in every pattern that uses a NEI condition. */
    modA,
    modB, ///< 0–127 mod source. STORED in Phase 6, EVALUATED by Phase 14.1.
    count
};

/** Number of parameter lanes — 11 (§12.1). */
inline constexpr int numLanes = static_cast<int> (LaneId::count);
static_assert (numLanes == 11, "ARCHITECTURE §12.1 defines exactly 11 parameter lanes.");

/** Pool traversal modes (ARCHITECTURE §12.3), in the order the doc lists them.

    APPEND-ONLY — same serialization reason as `LaneId`. See
    sequencer/DirectionModes.h for the exact order each mode produces; the
    semantics of `converge` / `diverge` / `outsideIn` are pinned there because
    Phase 6.4 freezes them into golden MIDI. */
enum class DirectionMode : std::uint8_t
{
    up = 0,          ///< Ascending.
    down,            ///< Descending.
    upDownInclusive, ///< Up then down, endpoints played twice.
    upDownExclusive, ///< Up then down, endpoints played once.
    converge,        ///< Outside → middle.
    diverge,         ///< Middle → outside (the exact reverse of `converge`).
    outsideIn,       ///< The round trip: `converge` then `diverge`.
    asPlayed,        ///< Arrival order — a POOL-VIEW selection, see `usesAsPlayedView`.
    walk,            ///< ±1 brownian, reflecting at the pool ends. Seeded.
    randomNoRepeat,  ///< Uniform, never twice in a row (loop point included). Seeded.
    spiral,          ///< Up-2 / back-1.
    count
};

/** Number of direction modes — 11 (§12.3). */
inline constexpr int numDirectionModes = static_cast<int> (DirectionMode::count);
static_assert (numDirectionModes == 11, "ARCHITECTURE §12.3 defines exactly 11 direction modes.");

/** When a staged change is allowed to take effect (ARCHITECTURE §5.2 "Quantized
    apply", §8.1 `transport.quantizeApply`, §6.1 pattern switching). Default is
    `bar` per §5.2. APPEND-ONLY. */
enum class QuantizeMode : std::uint8_t
{
    instant = 0, ///< At the next step boundary in the current block.
    beat,        ///< At the next quarter-note boundary.
    bar,         ///< At the next bar line (the default).
    patternEnd   ///< When the current pattern's last step completes.
};

/** Elektron-style trig conditions (ARCHITECTURE §12.2).

    ── PHASE 6 STORES THESE; PHASE 7.1 EVALUATES THEM ──────────────────────────
    The COND lane holds these ordinals now so §8.1's schema and Phase 16's lane
    strip are complete, but the step tick ignores the lane until Phase 7.1 lands
    the condition gate (and the RNG infrastructure the probability roll needs).

    ── THE A:B SET: B ∈ {2, 4, 8, 16}, A RUNNING 1..B (issue #62) ──────────────
    §12.2 USED to trail off — "1:2, 2:2, 1:4, 2:4, 3:4, 1:8 …" — and Phase 6 had to
    INFER where the ellipsis ended. It guessed B ∈ {2, 4, 8}. Issue #62 escalated
    the guess; the resolution is B ∈ {2, 4, 8, 16}, i.e. 2 + 4 + 8 + 16 = 30 A:B
    entries, because Elektron hardware (§12.2's stated design reference) offers
    1:16 through 16:16 and 16-step cycles are musically common. §12.2 now
    ENUMERATES the set exhaustively — the ellipsis that caused this is gone, so
    nobody has to infer it again. There is deliberately no :3/:5/:6/:7 family.
    `A:B` fires on loop A of every B pattern loops (§12.2 "pattern-loop-aware").

    ── APPEND-ONLY, AND THE LAST FREE MOMENT HAS PASSED ────────────────────────
    These ordinals are the COND lane's stored values (§8.1). The :16 family was
    INSERTED in its natural position rather than bolted on after `notNei`, which
    was legal exactly once: at the time of the change NOTHING serialized a
    `TrigCondition` (Phase 11 owns persistence and does not exist), the COND lane
    was stored-but-unevaluated, and no golden could encode it (goldens are emitted
    MIDI; COND is not evaluated). All three of those facts have an expiry date, and
    the first one to lapse ends the exemption.

    SO: FROM HERE THIS ENUM IS APPEND-ONLY, WITH NO REMAINING GRACE. Insert or
    renumber and every saved pattern's conditions silently become different
    conditions — 1:4 turns into 3:8 with no error anywhere. New entries (an odd-B
    family, a new named condition) go AFTER `notNei` and before `count`, however
    ugly that reads. The `static_assert`s below pin the current ordinals so an
    accidental insertion fails to compile rather than corrupting projects. */
enum class TrigCondition : std::uint8_t
{
    none = 0, ///< No condition — the step always passes the gate.

    // A:B loop cycles, §12.2. Fires on loop A of every B loops of the pattern.
    ab1of2,
    ab2of2,

    ab1of4,
    ab2of4,
    ab3of4,
    ab4of4,

    ab1of8,
    ab2of8,
    ab3of8,
    ab4of8,
    ab5of8,
    ab6of8,
    ab7of8,
    ab8of8,

    ab1of16,
    ab2of16,
    ab3of16,
    ab4of16,
    ab5of16,
    ab6of16,
    ab7of16,
    ab8of16,
    ab9of16,
    ab10of16,
    ab11of16,
    ab12of16,
    ab13of16,
    ab14of16,
    ab15of16,
    ab16of16,

    first,    ///< 1ST   — only on the first loop of the pattern.
    notFirst, ///< !1ST  — every loop except the first.
    fill,     ///< FILL  — only while the FILL flag is held (pad 16).
    notFill,  ///< !FILL — only while FILL is not held.
    pre,      ///< PRE   — only if the previous step's condition passed.
    notPre,   ///< !PRE  — only if the previous step's condition failed.
    nei,      ///< NEI   — only if the neighbour mod lane's result passed.
    notNei,   ///< !NEI  — only if the neighbour mod lane's result failed.
    count
};

/** Number of trig conditions, including `none`. */
inline constexpr int numTrigConditions = static_cast<int> (TrigCondition::count);

// ── THE SERIALIZED ORDINALS, PINNED ──────────────────────────────────────────
// The append-only rule above, made checkable. These are the values Phase 11 will
// write into `project.json`'s COND lane; an insertion anywhere in the enum shifts
// one of them and fails HERE, at compile time, instead of silently remapping every
// saved pattern's conditions. Adding a new entry before `count` moves only
// `numTrigConditions`, which is the one line a legal append is expected to touch.
static_assert (static_cast<int> (TrigCondition::none) == 0, "COND ordinal 0 is `none`.");
static_assert (static_cast<int> (TrigCondition::ab1of2) == 1, "The A:B block starts at ordinal 1.");
static_assert (static_cast<int> (TrigCondition::ab1of4) == 3, "The :4 family follows the :2 family.");
static_assert (static_cast<int> (TrigCondition::ab1of8) == 7, "The :8 family follows the :4 family.");
static_assert (static_cast<int> (TrigCondition::ab1of16) == 15, "The :16 family follows the :8 family.");
static_assert (static_cast<int> (TrigCondition::ab16of16) == 30, "2 + 4 + 8 + 16 = 30 A:B entries.");
static_assert (static_cast<int> (TrigCondition::first) == 31, "The named conditions follow the A:B block.");
static_assert (static_cast<int> (TrigCondition::notNei) == 38, "`notNei` is the last entry; append AFTER it.");
static_assert (numTrigConditions == 39, "1 `none` + 30 A:B + 8 named (ARCHITECTURE §12.2).");
static_assert (numTrigConditions - 1 <= 255, "COND ordinals must fit the enum's std::uint8_t base type.");

/** One parameter lane's stored data (ARCHITECTURE §5.1 L1, §12.1).

    ── WHY EVERY LANE IS int16, NOT ITS "NATIVE TYPE" ──────────────────────────
    §8.1 says "Lane step values are stored as dense arrays of the lane's native
    type". THAT IS A FILE-FORMAT STATEMENT, and Phase 11's serializer honours it —
    it writes each lane at its own width. This is the IN-MEMORY representation,
    where one uniform signed 16-bit slot is deliberately chosen instead: every
    §12.1 range fits (widest is LEN's 1..400 and MICRO's −50..+50), and it means
    ONE `laneValueAt` / one clamp / one operator write path rather than eleven
    templated ones — the operator stack (§5.1 L3) targets lanes through a mask and
    must be able to write any lane through the same code. Do not read this as a
    schema violation.

    `length` and `division` are per-lane, which is what produces polymeter
    (§5.1 L1: "each lane has independent length + clock division").

    NOTE FOR WHOEVER BUILDS PatternDocument: a value-initialised `LaneState` has
    all-ZERO `values`, which is OUT OF RANGE for VEL (1..127), LEN (1..400) and
    RATCHET (1..8) — a zero-filled lane set is storage-valid but not musically
    valid. Fill a new pattern's lanes with `laneDefault (lane)` rather than
    relying on `{}`. This type stays an aggregate on purpose (it has to be
    trivially copyable to ride a snapshot), so it cannot do that for itself. */
struct LaneState
{
    std::uint8_t length = 16;                     ///< Active steps, 1..maxSteps.
    std::uint8_t division = 1;                    ///< Clock divider, 1..maxLaneDivision (higher = slower).
    std::array<std::int16_t, maxSteps> values {}; ///< Step values; valid over [0, length).
};

/** Euclidean generator settings for the GATE lane (ARCHITECTURE §5.1 L1
    "Euclidean generator (steps/pulses/rotate) writes the GATE lane", §8.1
    `euclid {steps,pulses,rotate}`). See sequencer/Euclid.h for the exact
    necklace and rotation convention. */
struct EuclidParams
{
    std::uint8_t steps = 16; ///< Necklace length, 1..maxSteps.
    std::uint8_t pulses = 0; ///< Onsets, clamped into [0, steps].
    std::int8_t rotate = 0;  ///< Rotation, any sign, taken modulo `steps`.
    bool enabled = false;    ///< When false the GATE lane keeps its hand-edited values.
};

/** Inclusive value bounds for a lane. */
struct LaneRange
{
    std::int16_t lo; ///< Minimum legal value (inclusive).
    std::int16_t hi; ///< Maximum legal value (inclusive).
};

// RT-SAFE: audio thread. Pure constexpr switch; no allocation.
/** The §12.1 value range for `lane`. An unknown/`count` lane yields `{0, 0}`. */
constexpr LaneRange laneRange (LaneId lane) noexcept
{
    switch (lane)
    {
    case LaneId::gate:
        return { 0, 1 };
    case LaneId::pitch:
        return { -24, 24 };
    case LaneId::oct:
        return { -4, 4 };
    case LaneId::vel:
        return { 1, 127 };
    case LaneId::len:
        return { 1, 400 };
    case LaneId::ratchet:
        return { 1, 8 };
    case LaneId::micro:
        return { -50, 50 };
    case LaneId::prob:
        return { 0, 100 };
    case LaneId::cond:
        return { 0, static_cast<std::int16_t> (numTrigConditions - 1) };
    case LaneId::modA:
    case LaneId::modB:
        return { 0, 127 };
    case LaneId::count:
    default:
        return { 0, 0 };
    }
}

// RT-SAFE: audio thread. Pure constexpr clamp; no allocation.
/** Clamps `value` into `laneRange (lane)`. THE single clamp for lane values —
    operators (§5.1 L3) write through it so no operator can push a lane out of
    range, and it takes/returns `int` so intermediate operator arithmetic that
    overflows int16 is clamped rather than wrapped. */
constexpr std::int16_t clampLaneValue (LaneId lane, int value) noexcept
{
    const auto range = laneRange (lane);
    const int lo = range.lo;
    const int hi = range.hi;

    return static_cast<std::int16_t> (value < lo ? lo : (value > hi ? hi : value));
}

// RT-SAFE: audio thread. Pure constexpr switch; no allocation.
/** The musically neutral starting value for `lane` — what a freshly created
    pattern's lane is filled with. GATE is off (a new pattern is silent until the
    user punches notes in or runs the euclidean generator), PITCH/OCT/MICRO are
    centred, VEL sits at 100, LEN at 50% of the step, RATCHET at 1 (no
    subdivision), PROB at 100% (always fires) and COND at `none`. */
constexpr std::int16_t laneDefault (LaneId lane) noexcept
{
    switch (lane)
    {
    case LaneId::vel:
        return 100;
    case LaneId::len:
        return 50;
    case LaneId::ratchet:
        return 1;
    case LaneId::prob:
        return 100;
    case LaneId::gate:
    case LaneId::pitch:
    case LaneId::oct:
    case LaneId::micro:
    case LaneId::cond:
    case LaneId::modA:
    case LaneId::modB:
    case LaneId::count:
    default:
        return 0;
    }
}

// RT-SAFE: audio thread. Pure arithmetic.
/** Floor division over the 64-bit step timeline. C++ integer division truncates
    TOWARD ZERO, so `-1 / 4 == 0`; the polymeter index needs `-1` there or a
    negative step would read the wrong lane slot AND alias onto a positive one.
    The `int` sibling of this lives in midi/NotePool.h (`floorDivInt`), which the
    octave carry uses; this is the step-timeline width. Precondition: `b > 0`. */
constexpr std::int64_t stepFloorDiv (std::int64_t a, std::int64_t b) noexcept
{
    const std::int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

// RT-SAFE: audio thread. Pure arithmetic.
/** Floor modulus over the 64-bit step timeline — the result is always in
    `[0, b)`, for any sign of `a`. This is the house floor-mod idiom (it matches
    `wrapStepIndex` in sequencer/SequencerProcessor.cpp). Precondition: `b > 0`. */
constexpr std::int64_t stepFloorMod (std::int64_t a, std::int64_t b) noexcept
{
    const std::int64_t r = a % b;
    return r < 0 ? r + b : r;
}

// RT-SAFE: audio thread. Two integer divisions plus three predictable guard
// branches; no allocation, no locks.
/** THE polymeter index formula (§5.1 L1) — the slot of `lane.values` that global
    step `globalStep` reads:

        index = floorMod (floorDiv (globalStep, division), length)

    THIS IS THE ONLY IMPLEMENTATION OF THAT FORMULA IN THE CODEBASE. Everything
    that needs it — the audio-thread step tick, `PatternSnapshot`'s readers, the
    tests — calls through here or through the two wrappers below. A second copy
    that "looks the same" is how a polymeter phase bug ships, because the two only
    disagree on the negative / degenerate inputs nobody writes a test for.

    The lane advances once every `division` pattern steps and wraps at its own
    `length`, so lanes of different lengths phase against each other instead of
    resetting together. `globalStep` is the GLOBAL step index (it keeps counting
    across pattern loops), which is what makes the phase relationship persist
    across the loop point rather than resetting every bar. Classic polymeter falls
    out of equal divisions with unequal lengths: GATE 16 against PITCH 5, both
    division 1, realign only every lcm (16, 5) = 80 steps.

    THE GUARDS ARE NOT DEFENSIVE PADDING. `globalStep` should never be negative
    (the transport's step index is not) and `length`/`division` should never be 0,
    but this runs on the audio thread where the alternative to a guard is a
    negative modulus or a division by zero on data that arrived through a snapshot
    swap. Negative steps floor-wrap (so the sequence read backwards from 0 is the
    same cycle, not a mirror of it); a 0 `length`/`division` is read as 1; a
    `length` above `maxSteps` is clamped so the index cannot leave `values`. */
constexpr int laneIndex (const LaneState& lane, std::int64_t globalStep) noexcept
{
    const std::int64_t division = lane.division > 0 ? static_cast<std::int64_t> (lane.division) : 1;
    const std::int64_t length = lane.length > 0 ? static_cast<std::int64_t> (lane.length) : 1;
    const std::int64_t clampedLength = length > maxSteps ? maxSteps : length;

    return static_cast<int> (stepFloorMod (stepFloorDiv (globalStep, division), clampedLength));
}

// RT-SAFE: audio thread. Delegates to `laneIndex`.
/** Reads `lane` at global step `globalStep` through the polymeter formula above.

    ── VALUE-HOLD, NOT TICK-GATED ──────────────────────────────────────────────
    This answers "what does the lane READ as right now", which is defined at every
    step, not only on the lane's own division ticks. A VEL lane at division 4 holds
    each of its velocities across four steps. Whether a step should FIRE AT ALL is
    a separate question answered by the GATE lane's `isLaneTick` — see the division
    note on `isLaneTick`. */
constexpr std::int16_t laneValueAt (const LaneState& lane, std::int64_t globalStep) noexcept
{
    return lane.values[static_cast<std::size_t> (laneIndex (lane, globalStep))];
}

// RT-SAFE: audio thread. One modulus.
/** True when `globalStep` lands on one of `lane`'s own division ticks, i.e.
    `floorMod (globalStep, division) == 0`.

    ── WHY ONLY THE GATE LANE ASKS THIS ────────────────────────────────────────
    A lane's `division` has two defensible readings and they sound different, so
    the split is fixed here:

      * GATE — a TRUE CLOCK DIVIDER on the trigger rate. A step fires only when
        `isLaneTick (gate, g)` is true AND the held gate value is non-zero. GATE
        division 2 HALVES the trigger rate; it does not double-trigger.
      * EVERY OTHER LANE — VALUE-HOLD. Sampled with `laneValueAt` whenever a
        trigger fires, on its own tick or not.

    Reading GATE as value-hold instead would make division 2 repeat each gate
    value twice at full rate — a completely different rhythm from the same data. */
constexpr bool isLaneTick (const LaneState& lane, std::int64_t globalStep) noexcept
{
    const std::int64_t division = lane.division > 0 ? static_cast<std::int64_t> (lane.division) : 1;
    return stepFloorMod (globalStep, division) == 0;
}

/** One pattern's authoritative core state (ARCHITECTURE §5.1 L1, §8.1
    `patterns[16]`). POD and copyable by design: the message-thread
    `PatternDocument` edits it, and an immutable `PatternSnapshot` is built from a
    copy of it and published to the audio thread by pointer swap (§3.4 mechanism 3).

    ── THERE IS DELIBERATELY NO GRID HERE ──────────────────────────────────────
    §2.1 describes a "per-pattern grid" while §8.1 puts `grid` under `transport`.
    RESOLVED IN FAVOUR OF §8.1: the step grid is PROJECT-LEVEL and lives on
    `PatternSetState::gridStepPpq`. A per-pattern grid would make a quantized
    pattern switch (§6.1) change the meaning of the transport's step index
    mid-flight, which is exactly the kind of discontinuity §5.5 exists to
    prevent. Polymeter is expressed by per-lane `division`, not by per-pattern
    grid, so nothing is lost. */
struct PatternState
{
    std::array<LaneState, numLanes> lanes {};    ///< Indexed by `LaneId`.
    DirectionMode direction = DirectionMode::up; ///< Pool traversal (§12.3).
    EuclidParams euclid {};                      ///< GATE generator (§5.1 L1).
    std::uint64_t masterSeed = 0;                ///< §5.2 master seed; also seeds `walk`/`randomNoRepeat`.
};

// RT-SAFE: audio thread. Pure indexing.
/** `state.lanes[lane]`, without the cast at every call site. */
constexpr const LaneState& laneOf (const PatternState& state, LaneId lane) noexcept
{
    return state.lanes[static_cast<std::size_t> (lane)];
}

/** MESSAGE-THREAD ONLY: mutable lane accessor for document edits. */
constexpr LaneState& laneOf (PatternState& state, LaneId lane) noexcept
{
    return state.lanes[static_cast<std::size_t> (lane)];
}

// ── SWING (§8.1 `transport.swingPct`) ────────────────────────────────────────
// PROJECT-LEVEL, exactly like the grid, and for the same reason: §8.1 groups it
// under `transport {bpm, swingPct, grid, quantizeApply}`. It is deliberately NOT
// on `PatternState` (it is not per pattern — a quantized pattern switch must not
// change the feel mid-flight, the same argument the grid note above makes) and
// deliberately NOT on `engine::Transport` (the emission core `evaluateStep`
// cannot reach the transport by construction — issue #53 — so parking swing there
// would force it out of the emission core and into the walk, which is precisely
// what the one-lane-read-path discipline forbids).
//
// 50 % is STRAIGHT: `swingShiftSteps` (sequencer/StepLogic.h) turns it into a
// displacement of exactly 0.0 steps, which is what makes every pre-7.2 golden
// provably unmoved by this field rather than approximately unmoved.

/** Straight timing — every step on its grid position. The default. */
inline constexpr double minSwingPct = 50.0;

/** Maximum swing: odd steps delayed by a full half step (`maxSubStepShiftSteps`
    in sequencer/SequencerProcessor.h is derived partly from this). */
inline constexpr double maxSwingPct = 75.0;

/** Default swing amount — straight. */
inline constexpr double defaultSwingPct = minSwingPct;

// ── RATCHET VELOCITY RAMP (§5.1 L2 "per-ratchet velocity ramp") ──────────────
// A PERCENTAGE applied to the LAST ratchet child, interpolated linearly from 0 %
// at child 0 (see `ratchetVelocity` in sequencer/StepLogic.h). -50 halves the
// last child (a roll that decays); +50 makes it half again as loud.
//
// ── WHY PROJECT-LEVEL RATHER THAN A LANE ────────────────────────────────────
// §5.1 L2 requires the ramp, but §12.1's lane table has no lane for it and §8.1's
// per-pattern schema no key — because it is a FEEL control of exactly the kind
// §8.1 groups under `transport`, the same argument swing above gets. A per-step
// ramp would also be the wrong ergonomics: the RATCHET lane already varies the
// child COUNT per step, and varying the envelope per step as well is what §12.4's
// RATCHETIZER operator ("amount, beat weighting") is for.
//
// 0 IS THE DEFAULT AND IS SHORT-CIRCUITED, so every child of a default project
// carries the step's own VEL byte-identically and no pre-7.2 golden can move.

/** Ramp floor: the last child is silenced down to VEL's minimum. */
inline constexpr double minRatchetVelocityRampPct = -100.0;

/** Ramp ceiling: the last child is twice the step's VEL (clamped at 127). */
inline constexpr double maxRatchetVelocityRampPct = 100.0;

/** Default ramp — flat; every child carries the step's own VEL. */
inline constexpr double defaultRatchetVelocityRampPct = 0.0;

/** The whole pattern set plus the project-level fields the sequencer needs
    (ARCHITECTURE §8.1). This is the state a `PatternSnapshot` is built from; it is
    NOT itself the snapshot (that is a later delegation).

    `gridStepPpq` is the length of ONE pattern step in quarter notes: 0.25 = 1/16,
    0.5 = 1/8, 0.125 = 1/32, and triplet/dotted grids are the same values scaled by
    2/3 and 3/2 (§2.1 "1/32..1/4, triplet/dotted"). It is TOP-LEVEL, not per
    pattern — see the note on `PatternState`. So is `swingPct` — see above. */
struct PatternSetState
{
    std::array<PatternState, maxPatterns> patterns {}; ///< §8.1 `patterns[16]`.
    double gridStepPpq = 0.25;                         ///< One step in quarter notes (1/16 default).
    double swingPct = defaultSwingPct;                 ///< §8.1 `transport.swingPct`, 50..75 (50 = straight).

    /** §5.1 L2 ratchet velocity ramp, -100..+100 % applied to the LAST child
        (0 = flat). PROJECT-LEVEL for the same reason as swing — see
        `minRatchetVelocityRampPct` ABOVE IN THIS HEADER, beside the swing range,
        where the bounds and the "why not a lane" argument live. (sequencer/
        StepLogic.h holds the ramp's only CONSUMER, `ratchetVelocity`; it points
        back here for the constants, so a pointer in that direction was circular.) */
    double ratchetVelocityRampPct = 0.0;

    int startPatternIndex = 0; ///< Pattern active at transport start, 0..maxPatterns-1.
    int outputChannel = 1;     ///< MIDI channel the engine emits on, 1..16.
    PoolSnapshot pool {};      ///< L0 note pool view (§5.1); stub-filled in Phase 6.
};
} // namespace arpbox::engine
