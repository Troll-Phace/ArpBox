#pragma once

#include "../EngineGuiGuard.h"
#include "../graph/ICommandSink.h"
#include "../graph/Transport.h"
#include "../midi/SoundingNoteTable.h"
#include "PatternChannel.h"
#include "PatternSnapshot.h"
#include "PatternTypes.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <cstdint>

namespace arpbox::engine
{
/** The ARP ENGINE node (ARCHITECTURE §3.3 "[ARP ENGINE node]", §4 step 4). A
    MIDI-only `AudioProcessor` sitting between the MIDI-in node and the hosted synth:
    `Transport → MidiIn → Seq → Synth → Master`.

    ── WHAT THIS NODE IS, AS OF PHASE 6 ────────────────────────────────────────
    Four things, and deliberately nothing else:

      1. The step-boundary WALK (below) — buffer-size independent, stateless.
      2. `describeStep()` — the L1 pattern-core read: the adopted `PatternSnapshot`'s
         GATE / PITCH / OCT / VEL / LEN lanes resolved for one global step index.
      3. The quantized PATTERN SWITCH (`queuePatternSwitch`), which is why this class
         is an `ICommandSink`.
      4. Note-off ownership via `SoundingNoteTable` (§5.5).

    Still absent on purpose: step logic (PROB/COND/RATCHET/MICRO — Phase 7), the
    operator stack and constraint gate (§5.1 L3, Phase 12+), and the live note pool
    (Phase 8; Phase 6 reads the snapshot's stub pool). The remaining six lanes are
    STORED in the snapshot and simply not read yet — see `describeStep`.

    ── THE STEP WALK, AND WHY IT IS BUFFER-SIZE INDEPENDENT ────────────────────
    Nothing about "where are we in the pattern" is stored in this object. There is no
    per-block step counter. Each block, the GLOBAL step index range is re-derived
    from the transport's latched PPQ span:

        firstIndex = ceilSnap (blockStartPpq () / stepPpq)
        endIndex   = ceilSnap (blockEndPpq ()   / stepPpq)   // exclusive
        for (n = firstIndex; n < endIndex; ++n)  emit step n

    `Transport` guarantees PPQ at an absolute sample is bit-identical however the
    blocks were carved (see Transport.h), so this range is a pure function of the
    absolute sample span — carve the same timeline into 32-sample or 2048-sample
    blocks and every step index is produced exactly once. THE INDEX SET IS
    UNCONDITIONALLY IDENTICAL; the emitted SAMPLE POSITION is identical too, with
    one bounded exception documented under "THE SNAP-BOUNDARY WINDOW" below. A
    stateful cursor would instead be a place for a missed or duplicated boundary to
    hide, which is why there isn't one.

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
         k+1's `firstIndex` the same integer by construction.
         THIS "BY CONSTRUCTION" IS EXACT AND IS NOT THE CLAIM QUALIFIED ABOVE — it is
         about index AGREEMENT between adjacent blocks (`endIndex(k)` and
         `firstIndex(k+1)` are literally the same expression on the same input), which
         holds with no exception. The qualified claim is a different one: WHERE inside
         its block a boundary is placed. Do not weaken this line on account of the
         window note below; they are about different things.
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

    ── THE SNAP-BOUNDARY WINDOW: the one exception (issue #37) ─────────────────
    Snap 1 pays for exact index agreement with a small, bounded asymmetry in
    PLACEMENT, and this is the only place it is written down. A boundary whose TRUE
    position lies within `stepIndexSnapSteps` BELOW a block edge is claimed by the
    LATER block (that is what the snapped ceiling does, deliberately). Its
    `blockOffsetForPpq` result is then very slightly NEGATIVE, and the hard clamp in
    `processBlock` emits it at offset 0 — up to ONE SAMPLE later than a different
    block carving, under which the same boundary falls mid-block and is placed
    exactly. So:

      - the index set is identical at every buffer size, ALWAYS;
      - a step is NEVER duplicated and NEVER skipped, ALWAYS;
      - a step's emitted sample position is identical at every buffer size EXCEPT
        for a boundary inside this window, which may land up to one sample late.

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

    WHAT MAKES THE LOOKAHEAD LEGITIMATE, AND WHAT PHASE 7 MUST NOT BREAK:
    `describeStep` is a PURE function of the global step index. That is what lets
    this node evaluate step k+1, k+2, … out of order with no cursor, no accumulator
    and no side effect — and it is the same property §9's offline MIDI drag-out
    depends on to render bit-identically to real time.

    PHASE 7.1 ADDS PROB AND COND (§12.2) TO `describeStep`. The lookahead stays
    correct ONLY while the result remains a pure function of the step index — i.e.
    while the probability roll is drawn from a seeded stream keyed by (step index,
    seed) rather than from a running per-step RNG cursor. §5.2's seed composition
    already requires exactly that, so this is a constraint to HONOUR, not a new one
    to invent. A COND whose result depends on the previous step's outcome (`PRE`,
    `NEI`) stays fine as long as that outcome is itself re-derivable from the index.
    If a future step's emission ever becomes cursor-dependent, this lookahead must be
    deleted, not patched — a mispredicted cutoff is a wrong note length.

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
    // `describeStep` reads the constants below.

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
    /** What one step boundary contributes to the output.
        ─── THE PIPELINE SEAM ──────────────────────────────────────────────────
        `describeStep()` is the ONE function each pipeline phase grows. Phase 6 filled
        it with L0+L1 (note pool → pattern core lanes); Phase 7 adds L2 (probability,
        trig conditions, ratchets, micro-timing), Phase 12+ adds L3 (the operator
        stack) and the constraint gate, and the return type grows into a small LIST of
        emissions each carrying a provenance bitmask (§5.4). The step WALK and the MIDI
        EMISSION either side of it do not change: that is the point of splitting them
        here. `gateFractionOfStep` is shaped like the LEN lane (§12.1: 1–400% of the
        step, >100% meaning tie/legato). */
    struct StepEmission
    {
        bool gate = false;                                ///< False ⇒ this step emits nothing.
        int channel = scaffoldChannel;                    ///< MIDI channel, 1..16.
        int note = scaffoldRootNote;                      ///< MIDI note number, 0..127.
        int velocity = scaffoldVelocity;                  ///< 1..127.
        double gateFractionOfStep = scaffoldGateFraction; ///< LEN, as a fraction of one step.
    };

    // RT-SAFE: audio thread. PURE FUNCTION of the global step index (plus the adopted
    // snapshot and the active pattern, both fixed for the whole block) — no cursor, no
    // per-step mutable state, so it cannot drift and is trivially reproducible offline
    // (§9 MIDI drag-out runs this same code path). THE PIPELINE SEAM: see StepEmission.
    /** Decides what global step `stepIndex` emits. */
    StepEmission describeStep (std::int64_t stepIndex) const noexcept;

    /** Hard ceiling on how many steps `cutoffForSamePitch` may look ahead.
        DERIVED, not chosen: §12.1 caps LEN at 400% of the step, so a note can
        overlap at most four following steps, and one more absorbs the ±1 sample of
        floor/round jitter between a boundary's sample and the note's due sample.
        The live bound is computed per note from its actual length (a 50% gate looks
        one step ahead, not five); this is only the defensive cap. A future LEN range
        change must re-derive it. */
    static constexpr int maxRetriggerLookaheadSteps = 5;

    // RT-SAFE: audio thread. Absolute sample at which step boundary `index` falls,
    // derived from THIS block's latched transport state but deliberately NOT clamped
    // into it — so it is valid for a boundary belonging to a LATER block, which is
    // what `cutoffForSamePitch` and the pattern-switch pre-flush both need.
    //
    // BUFFER-SIZE INDEPENDENT: `blockStartSample + (ppq - blockStartPpq) / ppqPerSample`
    // is the same absolute value however the timeline was carved (Transport.h: PPQ at
    // an absolute sample is bit-identical across carvings), and the residual is ~1e-6
    // samples worst case — two orders of magnitude under `sampleOffsetSnapSamples`,
    // which is applied here exactly as the step walk applies it.
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
    // BOUNDED: it scans forward only while the note is still sounding
    // (`boundary <= naturalDueSample`), so the loop runs ~ceil(LEN%) times and is
    // additionally capped at `maxRetriggerLookaheadSteps`. It stops short of a
    // RESOLVED pattern switch, because `describeStep` would describe those steps
    // under the OUTGOING pattern — the switch's own flush releases the note there.
    //
    // VALID ONLY BECAUSE `describeStep` IS PURE — see the Phase 7.1 constraint in
    // the class comment before adding anything cursor-dependent to it.
    std::int64_t cutoffForSamePitch (std::int64_t stepIndex,
                                     int channel,
                                     int note,
                                     std::int64_t onSample,
                                     std::int64_t naturalDueSample,
                                     double stepPpq,
                                     std::int64_t blockStartSample,
                                     int lookaheadSteps) const noexcept;

    // RT-SAFE: audio thread. Applies the same-pitch retrigger policy (§5.5), emits
    // the note-on and registers its scheduled note-off with the table.
    //
    // `blockStartSample` and `numSamples` describe THIS block and are passed
    // explicitly even though `blockStartSample == onSample - offset` recovers one
    // from the others: the retrigger path places its note-off through
    // `SoundingNoteTable::retireNoLaterThan`, which works in absolute samples, and
    // an implicitly reconstructed block origin in determinism-critical code is a
    // coupling waiting to be broken by a future edit to the offset arithmetic.
    // `stepIndex` and `stepPpq` are what the same-pitch lookahead needs to describe
    // the FOLLOWING steps.
    void emitStep (const StepEmission& emission,
                   juce::MidiBuffer& midi,
                   std::int64_t stepIndex,
                   int offset,
                   std::int64_t onSample,
                   std::int64_t blockStartSample,
                   int numSamples,
                   double stepPpq,
                   double samplesPerStep) noexcept;

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

    /** Last observed `Transport::stopGeneration()`. Compared every block so a stop
        whose latched edge this node MISSED (async graph insertion) still triggers a
        flush. Seeded on the first block so joining a session that has already been
        stopped does not fire a spurious flush. */
    std::uint64_t lastSeenStopGeneration = 0;
    bool stopGenerationSeeded = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SequencerProcessor)
};
} // namespace arpbox::engine
