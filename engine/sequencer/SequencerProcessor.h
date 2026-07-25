#pragma once

#include "../EngineGuiGuard.h"
#include "../graph/Transport.h"
#include "../midi/SoundingNoteTable.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <cstdint>

namespace arpbox::engine
{
/** The ARP ENGINE node (ARCHITECTURE §3.3 "[ARP ENGINE node]", §4 step 4). A
    MIDI-only `AudioProcessor` sitting between the MIDI-in node and the hosted synth:
    `Transport → MidiIn → Seq → Synth → Master`.

    PHASE 5.2 IS A SHELL. It contains exactly two real things — the step-boundary
    walk (which must be right, because every Phase-6+ golden MIDI file is built on
    it) and note-off ownership via `SoundingNoteTable` (§5.5). The material it plays
    is a hardcoded scaffold pattern that Phase 6 DELETES. There is deliberately no
    pattern model, no lane storage, no step logic and no operator stack here: those
    are Phases 6/7/12+ and building them now would be work thrown away.

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
    blocks and every step index is produced exactly once, in the same place. A
    stateful cursor would instead be a place for a missed or duplicated boundary to
    hide, which is why there isn't one.

    Two SNAPS make that exact in floating point. Both are orders of magnitude larger
    than the accumulated rounding they absorb and orders of magnitude smaller than
    anything musically observable; without them, one specific and very common family
    of configurations breaks:

      1. `stepIndexSnapSteps` — consecutive blocks share an edge only
         MATHEMATICALLY: `blockEndPpq()` of block k evaluates
         `ppq_k + advance * pps` while `blockStartPpq()` of block k+1 evaluates
         `anchorPpq + (s_k + advance - anchor) * pps`. Equal in exact arithmetic,
         but they can differ by an ulp. A boundary landing inside that disagreement
         would then be emitted twice, or not at all. Applying the SAME snapped ceiling
         to both ends of the half-open interval makes block k's `endIndex` and block
         k+1's `firstIndex` the same integer by construction.
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

    ── NOTE-OFF OWNERSHIP (§5.5) ───────────────────────────────────────────────
    Every note-on emitted here is registered in `SoundingNoteTable` with the absolute
    sample its off is due (see SoundingNoteTable.h for why samples, not PPQ). The
    table is the only thing that emits note-offs, so a note cannot leak.

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
      Pattern switch, pool change and plugin swap are Phase 6/8/9 flush points.

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
class SequencerProcessor : public juce::AudioProcessor
{
public:
    // ── SCAFFOLD PATTERN (Phase 6 DELETES ALL OF THIS) ───────────────────────
    // Replaced by the immutable `PatternSnapshot` published by `PatternDocument`
    // (§4, §5.1 L1): 11 lanes over up to 64 steps with per-lane length and clock
    // division. Nothing here is meant to generalise — it exists so the transport,
    // the graph splice and the note-off machinery can be heard and tested.

    /** Scaffold pattern length in steps. */
    static constexpr int scaffoldNumSteps = 16;

    /** Scaffold step grid: a 16th note (quarter notes per step). Phase 6 takes this
        from the pattern's grid setting (1/32..1/4, triplet/dotted, §2.1). */
    static constexpr double scaffoldStepPpq = 0.25;

    /** Scaffold root note (middle C). Phase 6 takes pitch from the note pool. */
    static constexpr int scaffoldRootNote = 60;

    /** Scaffold velocity. Phase 6 takes this from the VEL lane (§12.1). */
    static constexpr int scaffoldVelocity = 100;

    /** Scaffold gate length as a fraction of the step. Phase 6 takes this from the
        LEN lane (1–400% of step, §12.1), where >100% means tie/legato. */
    static constexpr double scaffoldGateFraction = 0.5;

    /** Scaffold MIDI output channel. Phase 6+ takes this from the synth slot config. */
    static constexpr int scaffoldChannel = 1;

    /** Constructs the node with NO audio buses (MIDI-only). */
    SequencerProcessor ();

    /** ~SequencerProcessor. */
    ~SequencerProcessor () override = default;

    // MESSAGE-THREAD ONLY: wiring. Injects the graph-owned transport this node reads
    // its musical position from. Call once, before the node joins the graph and
    // before playback. The pointer is non-owning and must outlive this node —
    // `EngineGraph` guarantees that by member declaration order.
    /** Sets the transport whose latched block-start position drives the step walk. */
    void setSharedState (const Transport* transportToFollow) noexcept;

    // MESSAGE-THREAD ONLY (observation): the table is AUDIO-THREAD-OWNED state, so a
    // message-thread read can race a concurrent block. Exposed for HEADLESS TESTS
    // that drive the graph themselves and assert the §5.5 invariant
    // ("table empty after every flush point"); the UI must never read it.
    /** The sounding-note table. Test/observation only — see the threading note. */
    const SoundingNoteTable& soundingNotes () const noexcept { return sounding; }

    // ── AudioProcessor overrides ─────────────────────────────────────────────

    /** Returns the node's display name. */
    const juce::String getName () const override { return "ARPBOX Sequencer"; }

    // MESSAGE-THREAD ONLY: called with audio stopped. Never on the audio thread.
    /** Drops any stale tracked notes (a fresh prepare means the previous graph
        configuration's notes can no longer be released) and re-seeds the
        stop-generation watermark. Nothing is allocated — the table is fixed-size. */
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;

    // MESSAGE-THREAD ONLY: release. Nothing heap-held.
    /** Releases resources and forgets tracked notes (audio has stopped; there is no
        buffer left to emit their offs into). */
    void releaseResources () override;

    // RT-SAFE: audio thread. Allocation-free, lock-free, no logging, no juce::String.
    /** Passes incoming MIDI through untouched, flushes on a transport discontinuity,
        emits the scaffold pattern's note-ons for every step boundary inside this
        block, and emits every note-off that comes due. Renders no audio. */
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

private:
    /** What one step boundary contributes to the output.
        ─── THE PHASE 6 SEAM ───────────────────────────────────────────────────
        `describeStep()` is the ONE function Phase 6 replaces. It becomes the
        four-layer pipeline of §5.1 — note pool → pattern core lanes → step logic →
        operator stack → constraint gate — reading the adopted `PatternSnapshot`
        instead of a `constexpr` table, and returning (eventually a small list of)
        emissions carrying a provenance bitmask (§5.4). The step WALK and the MIDI
        EMISSION either side of it do not change: that is the point of splitting them
        here. `gateFractionOfStep` is already shaped like the LEN lane (§12.1: 1–400%
        of the step, >100% meaning tie/legato) so Phase 6 does not have to reshape the
        contract to grow into it. */
    struct StepEmission
    {
        bool gate = false;                    ///< False ⇒ this step emits nothing.
        int channel = scaffoldChannel;        ///< MIDI channel, 1..16.
        int note = scaffoldRootNote;          ///< MIDI note number, 0..127.
        int velocity = scaffoldVelocity;      ///< 1..127.
        double gateFractionOfStep = scaffoldGateFraction; ///< LEN, as a fraction of one step.
    };

    // RT-SAFE: audio thread. Pure function of the global step index — no state, so it
    // cannot drift and is trivially reproducible offline (§9 MIDI drag-out uses the
    // same code path). THE PHASE 6 SEAM: see StepEmission.
    /** Decides what global step `stepIndex` emits. */
    StepEmission describeStep (std::int64_t stepIndex) const noexcept;

    // RT-SAFE: audio thread. Applies the same-pitch retrigger policy (§5.5), emits
    // the note-on and registers its scheduled note-off with the table.
    void emitStep (const StepEmission& emission,
                   juce::MidiBuffer& midi,
                   int offset,
                   std::int64_t onSample,
                   double samplesPerStep) noexcept;

    // RT-SAFE: audio thread. Flushes the sounding-note table if this block carries a
    // transport discontinuity. Returns true if a flush happened.
    bool handleDiscontinuities (juce::MidiBuffer& midi) noexcept;

    // Injected, non-owning (graph-owned) shared state.
    const Transport* transport = nullptr; ///< Musical clock; read-only from here.

    // Audio-thread-owned note lifecycle authority (§5.5).
    SoundingNoteTable sounding;

    /** Last observed `Transport::stopGeneration()`. Compared every block so a stop
        whose latched edge this node MISSED (async graph insertion) still triggers a
        flush. Seeded on the first block so joining a session that has already been
        stopped does not fire a spurious flush. */
    std::uint64_t lastSeenStopGeneration = 0;
    bool stopGenerationSeeded = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SequencerProcessor)
};
} // namespace arpbox::engine
