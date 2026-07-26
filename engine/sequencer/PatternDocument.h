#pragma once

#include "../EngineGuiGuard.h"
#include "../midi/NotePool.h"
#include "PatternChannel.h"
#include "PatternSnapshot.h"
#include "PatternTypes.h"

#include <cstdint>
#include <deque>
#include <vector>

namespace arpbox::engine
{
// ─────────────────────────────────────────────────────────────────────────────
// PatternDocument.h — the AUTHORITATIVE, EDITABLE pattern model
// (ARCHITECTURE §4 "Message-thread edit flow", §5.1 L1, §8.1).
//
//     UI edit → mutate PatternDocument → rebuild immutable PatternSnapshot →
//     publish through PatternChannel → audio thread adopts at a boundary;
//     the undo stack records the delta.
//
// MESSAGE-THREAD ONLY, ENTIRELY. There is no `noexcept` RT path in this class and
// there must never be one: it allocates (undo stack, snapshot builds), it copies
// ~24 KB of state per edit, and the audio thread's only contact with any of it is
// the immutable snapshot that comes out the far end.
// ─────────────────────────────────────────────────────────────────────────────

/** The message-thread pattern model: 16 patterns, 11 lanes each, plus the
    project-level grid / pool / output channel, with undo and snapshot publishing.

    ── UNDO IS A CAPPED POD STATE STACK, NOT juce::UndoManager ─────────────────
    `UndoManager` lives in `juce_data_structures`, which the engine deliberately
    does not link (§3.2 / engine/CMakeLists.txt: juce_core, juce_audio_basics,
    juce_audio_devices, juce_audio_formats, juce_audio_processors_headless,
    juce_dsp — and nothing else). Pulling a whole JUCE module into the
    deliberately-minimal UI-free engine to get undo would be the wrong trade when
    `PatternSetState` is already a copyable POD: full-state snapshots are ~24 KB
    each, need no per-edit delta type, and cannot desynchronise from the model the
    way hand-written inverse actions can.

    The cost is memory, so the stack is CAPPED at `maxUndoDepth` (256 ≈ 6 MB).
    §5.2 promises "unlimited undo" for document edits; read that as pragmatically
    capped at 256 gestures — and note that a GESTURE is one entry, not one mouse
    move, because `beginTransaction` / `endTransaction` collapse a marquee paste or
    a length drag into a single transaction. */
class PatternDocument
{
public:
    /** Undo entries retained. Each is a full `PatternSetState` (~24 KB), so this
        bounds the undo stack's footprint at roughly 6 MB. */
    static constexpr int maxUndoDepth = 256;

    // MESSAGE-THREAD ONLY:
    /** Constructs the default document.

        ── THE DEFAULT REPRODUCES THE PHASE-5 SCAFFOLD EXACTLY ─────────────────
        Not nostalgia — a deliberate migration decision. The Phase-5.2 sequencer
        node emits a hardcoded 16-step scaffold (C major ascending over one octave,
        played twice per bar, velocity 100, 50% gate, channel 1, 1/16 grid) and the
        existing sequencer timing tests are written against it. Matching those
        defaults means those tests migrate as a RENAME rather than a rewrite, the
        app stays audible at every commit through the transition, and Phase 6.4's
        goldens continue the Phase-5 timing baseline instead of starting a new one.

        The equivalence is not a coincidence to be preserved by hand — it FALLS OUT
        of the pipeline: an all-on GATE lane makes the gated ordinal equal the step
        index, `DirectionMode::up` over the 8-note stub pool makes the pool index
        `step % 8`, and a PITCH lane of 0 makes the note the pool note itself. So
        step g plays `{60,62,64,65,67,69,71,72}[g % 8]` — exactly
        `scaffoldRootNote + scaffoldSemitoneOffsets[g % 16]`.

        Concretely:
          * pool = {60,62,64,65,67,69,71,72} (C major, one octave), `sorted` ==
            `asPlayed` (a stub pool has no arrival history — see NotePool.h);
          * `gridStepPpq` 0.25, `outputChannel` 1, `startPatternIndex` 0;
          * every lane length 16, division 1, filled with `laneDefault` — which is
            already PITCH 0, OCT 0, VEL 100, LEN 50, RATCHET 1, PROB 100, COND
            none, MICRO/MOD 0;
          * pattern 0's GATE lane on across its 16 active steps;
          * patterns 1–15 identical but GATE all-off, i.e. silent.

        Lanes are filled with `laneDefault`, NEVER left value-initialised: a
        zero-filled lane set is storage-valid but musically invalid (VEL 0, LEN 0,
        RATCHET 0 are all out of range — see the note on `LaneState`). */
    PatternDocument ();

    PatternDocument (const PatternDocument&) = delete;
    PatternDocument& operator= (const PatternDocument&) = delete;

    // ── STATE ────────────────────────────────────────────────────────────────

    // MESSAGE-THREAD ONLY:
    /** The authoritative state. Read-only: every mutation goes through an edit
        method so undo and republishing cannot be bypassed. */
    const PatternSetState& state () const noexcept { return current; }

    // ── EDITS ────────────────────────────────────────────────────────────────
    // Each edit is one undo entry (or joins the open transaction), and each
    // triggers a rebuild + republish when a publish target is attached.
    //
    // Every one returns false and changes NOTHING on an out-of-range argument, and
    // false on a no-op (a value that already equals what is stored) — a no-op must
    // not consume an undo slot or spend ~24 KB and a snapshot build.

    // MESSAGE-THREAD ONLY:
    /** Sets one step of one lane. `value` is clamped into the lane's §12.1 range
        rather than rejected, so an operator-style caller cannot fail on overshoot.

        @param patternIndex  0..maxPatterns-1.
        @param lane          Any lane except `LaneId::count`.
        @param step          0..maxSteps-1 (the lane's STORAGE index; steps at or
                             beyond `length` are stored but never played).
        @param value         Clamped into `laneRange (lane)`. */
    bool setLaneValue (int patternIndex, LaneId lane, int step, int value);

    // MESSAGE-THREAD ONLY:
    /** Sets a lane's active length, 1..maxSteps. Values outside the pattern's other
        lane lengths are the POINT — unequal lengths at equal divisions is what
        produces polymeter (§5.1 L1). */
    bool setLaneLength (int patternIndex, LaneId lane, int length);

    // MESSAGE-THREAD ONLY:
    /** Sets a lane's clock division, 1..maxLaneDivision (higher = slower).

        Remember the asymmetry (documented on `isLaneTick`): GATE's division is a
        true divider on the TRIGGER RATE, every other lane's is value-hold. */
    bool setLaneDivision (int patternIndex, LaneId lane, int division);

    // MESSAGE-THREAD ONLY:
    /** Sets the pool traversal mode (§12.3). */
    bool setDirection (int patternIndex, DirectionMode direction);

    // MESSAGE-THREAD ONLY:
    /** Sets the pattern's master seed (§5.2). In Phase 6 it drives only the
        stochastic direction modes' traversal tables; Phase 12 makes it the root of
        the operator seed composition. */
    bool setMasterSeed (int patternIndex, std::uint64_t seed);

    // MESSAGE-THREAD ONLY:
    /** Runs the euclidean generator into the GATE lane (§5.1 L1), storing the
        parameters and setting the GATE lane's length to the necklace length.

        Always ENABLES euclid for the pattern — calling this IS the user asking for
        a generated gate lane. `steps` is clamped into `[1, maxSteps]`, `pulses`
        into `[0, steps]`, `rotate` taken modulo `steps` by the generator. */
    bool applyEuclid (int patternIndex, int steps, int pulses, int rotate);

    // MESSAGE-THREAD ONLY:
    /** Disables the euclidean generator without touching the GATE lane the last
        run left behind — the user keeps the rhythm and can now hand-edit it. */
    bool setEuclidEnabled (int patternIndex, bool enabled);

    // MESSAGE-THREAD ONLY:
    /** Sets the step grid in quarter notes — 0.25 = 1/16 (the default), 0.5 = 1/8,
        0.125 = 1/32, ×2/3 for triplets, ×3/2 for dotted (§2.1).

        PROJECT-LEVEL: there is no pattern index, deliberately. A per-pattern grid
        would let a quantized pattern switch change the meaning of the transport's
        step index mid-flight (see the note on `PatternState`). Values must be
        positive; polymeter is expressed with per-lane `division`, not with this. */
    bool setGrid (double stepPpq);

    // MESSAGE-THREAD ONLY:
    /** Sets the project swing amount as a percentage, CLAMPED into [50, 75]
        (§8.1 `transport.swingPct`). 50 is straight; 75 delays every odd global
        step by a full half step.

        PROJECT-LEVEL, mirroring `setGrid` exactly — no pattern index, for the
        same reason (see the swing note in PatternTypes.h). CLAMPED rather than
        rejected, unlike `setGrid`: unlike a grid, every finite swing value has a
        sane nearest legal neighbour, and a macro or mod-matrix source (§5.3) will
        eventually drive this continuously and must not fail on overshoot. A
        non-finite value IS rejected — there is nothing to clamp a NaN to. */
    bool setSwing (double swingPct);

    // MESSAGE-THREAD ONLY:
    /** Sets the project ratchet velocity ramp as a percentage applied to the LAST
        ratchet child, CLAMPED into [-100, +100] (§5.1 L2 "per-ratchet velocity
        ramp"). 0 — the default — is flat: every child carries its step's own VEL.

        PROJECT-LEVEL, like `setSwing` and for the same reason (see the ramp note
        in PatternTypes.h). Clamped, not rejected; a non-finite value IS rejected. */
    bool setRatchetVelocityRamp (double rampPct);

    // MESSAGE-THREAD ONLY:
    /** Replaces the L0 note pool view (§5.1). Phase 6 uses this for the stub pool
        and for tests; Phase 8's live THRU/SELF pool publishes through the same
        door. A `size` above `maxPoolSize` is clamped. */
    bool setPool (const PoolSnapshot& pool);

    // MESSAGE-THREAD ONLY:
    /** Sets the MIDI channel the engine emits on, 1..16 (§8.1). */
    bool setOutputChannel (int channel);

    // MESSAGE-THREAD ONLY:
    /** Sets the pattern active at transport start, 0..maxPatterns-1 (§8.1). */
    bool setStartPatternIndex (int patternIndex);

    // ── TRANSACTIONS ─────────────────────────────────────────────────────────

    // MESSAGE-THREAD ONLY:
    /** Opens a transaction: every edit until the matching `endTransaction`
        collapses into ONE undo entry and ONE republish.

        This is what makes a multi-edit gesture — a marquee paste, a length drag, a
        lane fill — behave as a single user action rather than 64 of them, and it
        is also the drag-performance answer: without it a mouse-move-per-step drag
        would build (and publish) a snapshot per step.

        Nestable; only the outermost pair has any effect. */
    void beginTransaction ();

    // MESSAGE-THREAD ONLY:
    /** Closes the innermost transaction. When the outermost one closes and
        anything actually changed, one undo entry is pushed and one republish
        happens. Unbalanced calls assert and are ignored. */
    void endTransaction ();

    // MESSAGE-THREAD ONLY:
    /** True while a transaction is open. */
    bool isTransactionOpen () const noexcept { return transactionDepth > 0; }

    // ── UNDO ─────────────────────────────────────────────────────────────────

    // MESSAGE-THREAD ONLY:
    /** True when there is at least one undo entry. */
    bool canUndo () const noexcept { return ! undoStack.empty (); }

    // MESSAGE-THREAD ONLY:
    /** True when there is at least one redo entry. */
    bool canRedo () const noexcept { return ! redoStack.empty (); }

    // MESSAGE-THREAD ONLY:
    /** Restores the previous state and republishes. No-op (false) when there is
        nothing to undo or a transaction is open — undoing mid-gesture would
        discard half of it. */
    bool undo ();

    // MESSAGE-THREAD ONLY:
    /** Re-applies the last undone state and republishes. Same open-transaction
        restriction as `undo`. */
    bool redo ();

    // MESSAGE-THREAD ONLY:
    /** Drops all undo and redo history. The current state is untouched. Use after
        a project load, where the pre-load history describes a different project. */
    void clearUndoHistory ();

    // MESSAGE-THREAD ONLY:
    /** Undo entries currently held, 0..maxUndoDepth. */
    int getUndoDepth () const noexcept { return static_cast<int> (undoStack.size ()); }

    // MESSAGE-THREAD ONLY:
    /** Redo entries currently held. */
    int getRedoDepth () const noexcept { return static_cast<int> (redoStack.size ()); }

    // ── PUBLISHING ───────────────────────────────────────────────────────────

    // MESSAGE-THREAD ONLY: allocates (snapshot build) and deletes (reclaim).
    /** Builds an immutable snapshot of the current state and publishes it.

        RECLAIMS FIRST, then publishes. Reclaiming here rather than only from a UI
        tick means (a) memory stays bounded during a long edit session even if the
        UI tick is starved or the app is headless, and (b) tests can drive the
        whole publish/adopt/retire cycle with no UI involvement at all — which is
        what makes the determinism suite self-sufficient. */
    void publishTo (PatternChannel& channel);

    // MESSAGE-THREAD ONLY:
    /** Attaches a channel that every committed edit republishes to automatically,
        or nullptr for manual publishing only (the default).

        With a target attached the §4 edit flow runs end to end from one call —
        which is how the UI uses it. Without one, `publishTo` is explicit, which is
        how tests that want to inspect intermediate states use it. Attaching does
        NOT publish; call `publishTo` once to prime the channel. */
    void setPublishTarget (PatternChannel* channel) noexcept { publishTarget = channel; }

    // MESSAGE-THREAD ONLY:
    /** Number of snapshots this document has built. Stamped onto each snapshot as
        `PatternSnapshot::buildCounter`, so a test can assert WHICH build the audio
        thread adopted. */
    std::uint64_t getBuildCounter () const noexcept { return buildCounter; }

    // MESSAGE-THREAD ONLY:
    /** Increments once per committed edit (and per undo/redo). Cheap "has anything
        changed since I last looked" for callers that publish on their own clock. */
    std::uint64_t getRevision () const noexcept { return revision; }

private:
    // Captures the pre-edit state if this is the first change of the gesture.
    // Call BEFORE mutating `current`.
    void beginEdit ();

    // Commits the change captured by `beginEdit`. Call AFTER mutating `current`.
    void endEdit ();

    // Pushes `previous` onto the undo stack, clears redo, and enforces the cap.
    //
    // BY CONST REFERENCE, AND THE COPY IS THE POINT. `PatternSetState` is trivially
    // copyable by design — that is exactly what makes this POD state stack viable
    // instead of linking juce_data_structures for `juce::UndoManager` (see the class
    // comment). A trivially-copyable type has no move that differs from its copy, so
    // a `&&` parameter here would advertise a transfer that cannot happen and every
    // `std::move` at a call site would be a no-op decoration. The ~24 KB copy into
    // the deque is unavoidable and already budgeted: `maxUndoDepth` × 24 KB ≈ 6 MB.
    void pushUndo (const PatternSetState& previous);

    // Publishes to `publishTarget` when one is attached.
    void republish ();

    // Validated accessor: nullptr when `patternIndex` / `lane` are out of range.
    LaneState* laneForEdit (int patternIndex, LaneId lane);

    PatternSetState current {};

    /** Pre-edit state held between `beginEdit` and `endEdit` (or across a whole
        transaction). One extra ~24 KB copy, which is the price of full-state
        undo. */
    PatternSetState pendingUndo {};

    std::deque<PatternSetState> undoStack;
    std::vector<PatternSetState> redoStack;

    int transactionDepth = 0;
    bool transactionChanged = false;

    PatternChannel* publishTarget = nullptr;
    std::uint64_t buildCounter = 0;
    std::uint64_t revision = 0;
};
} // namespace arpbox::engine
