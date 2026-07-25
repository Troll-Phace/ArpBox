#pragma once

#include "../EngineGuiGuard.h"
#include "EngineCommand.h"
#include "ICommandSink.h"

#include <juce_core/juce_core.h>

#include <cstdint>

namespace arpbox::engine
{
/** The engine's sample-accurate musical clock (ARCHITECTURE §3.3, §4 step 2;
    INSTRUCTIONS Phase 5.1). Everything downstream — the sequencer node's step
    walk, the `TransportPlayHead` that hosted plugins read, the `EngineSnapshot`
    transport readout — derives its notion of time from this object.

    ── DETERMINISM: HOW PPQ IS COMPUTED (the load-bearing detail) ───────────────
    The determinism contract (§1.2: "same (pattern, seeds, N bars) ⇒ byte-identical
    MIDI, forever") starts here. A PPQ value that drifts by an ulp per block would
    silently move a step boundary across a block edge after a few thousand blocks
    and poison every Phase-6+ golden MIDI file. So PPQ is NEVER accumulated
    per-block. Instead:

      - `samplePosition` is an EXACT `std::int64_t` sample counter — the only piece
        of state that advances per block, and it advances by integer addition, so it
        cannot drift.
      - A tempo SEGMENT is anchored by the pair (`anchorSample`, `anchorPpq`) plus
        the segment's `ppqPerSample`. PPQ at any sample is then ONE multiply and
        ONE add from an exact integer difference:

            ppqAtSample (s) = anchorPpq + double (s - anchorSample) * ppqPerSample

    Consequences that matter:
      - Rounding error is bounded by ~1 ulp for the whole segment, no matter how
        many blocks elapse — it does not compound.
      - The PPQ at absolute sample `s` is BIT-IDENTICAL regardless of how the
        blocks were carved up, which is what makes the buffer-size-independence
        test (Phase 5.3: identical event sample positions at 32…2048 samples)
        pass by construction rather than by luck.
      - A tempo change re-anchors (one new rounding event per tempo change, at a
        known sample), so a tempo map of N segments costs N roundings total.

    ── BLOCK-START LATCHING ────────────────────────────────────────────────────
    `beginBlock()` is called ONCE per block by the transport head node, BEFORE any
    other graph node renders. It latches the block-START state (ppq, bpm,
    isPlaying, timeInSamples/Seconds, ppqOfLastBarStart, block length) into
    separate members, and only THEN advances `samplePosition` for the next block.
    Every getter returns the LATCHED block-start value, so all consumers within one
    block agree on a single, consistent view of time no matter where in the render
    sequence they sit. The sequencer converts within the block using
    `ppqPerSample()` / `ppqAtBlockOffset()` / `blockOffsetForPpq()`.

    ── COMMANDS (this class is an ICommandSink) ─────────────────────────────────
    Consumes `transportPlay`, `transportStop`, `transportLocate`, `setTempoBpm`
    and ignores everything else. `applyCommand` runs during the head node's drain,
    which is strictly BEFORE `beginBlock()` and before any node renders — so a
    command lands on the very block it was drained in, and every tempo change
    therefore lands exactly on a BLOCK BOUNDARY (INSTRUCTIONS 5.1), structurally
    rather than by convention. Non-finite / out-of-range payloads are rejected
    (same defensive posture as `MasterProcessor::applyCommand` after issue #3).

    ── STOP SEMANTICS (Phase 5.2 depends on this) ──────────────────────────────
    `transportStop` stops AND REWINDS to PPQ 0 (groovebox convention; there is no
    "pause" in the MVP feature set §2.1 — `transportLocate` covers arbitrary
    positioning). Three signals let the sequencer react deterministically:
      - `stoppedThisBlock()`     — latched edge, true for EXACTLY the block whose
                                   drain consumed the stop. THE primary flush
                                   trigger (§5.5 "flush on transport stop"): it is
                                   visible to every node that renders after the
                                   head node, in the SAME block, with no polling.
      - `positionJumpedThisBlock()` — latched edge, true when a locate (including
                                   the stop's implicit rewind) moved the playhead
                                   this block. Also a flush point: a PPQ jump
                                   orphans scheduled note-offs.
      - `stopGeneration()`       — monotonic counter bumped on every stop. Belt and
                                   braces for a consumer that cannot guarantee it
                                   observes every block (e.g. a node spliced in by
                                   an async graph rebuild that missed a block): it
                                   can compare against its own last-seen value and
                                   still detect that a stop happened.

    ── THREADING ───────────────────────────────────────────────────────────────
    AUDIO-THREAD-OWNED state, deliberately NOT atomic. It is written only by
    `beginBlock()` and `applyCommand()` — both audio thread — and read only by
    audio-thread code (`TransportPlayHead::getPosition`, the sequencer node,
    `MasterProcessor`'s snapshot write), all inside the same device callback.
    `prepare()` is the sole message-thread entry point and runs with audio stopped.
    No allocation, no locks, no logging anywhere in this class. */
class Transport final : public ICommandSink
{
public:
    /** Minimum supported tempo (BPM) — INSTRUCTIONS 5.1 / §8.1. */
    static constexpr double minBpm = 20.0;
    /** Maximum supported tempo (BPM) — INSTRUCTIONS 5.1 / §8.1. */
    static constexpr double maxBpm = 300.0;
    /** Tempo used until a `setTempoBpm` command arrives. */
    static constexpr double defaultBpm = 120.0;
    /** Quarter notes per bar. Fixed 4/4 for the MVP (a real time-signature model
        arrives with the tempo/meter map post-MVP); used for `ppqOfLastBarStart`. */
    static constexpr double quarterNotesPerBar = 4.0;

    /** Constructs a stopped transport at PPQ 0, `defaultBpm`, 44.1 kHz. */
    Transport ();

    /** ~Transport. */
    ~Transport () override = default;

    // MESSAGE-THREAD ONLY: called with audio stopped (from the transport node's
    // prepareToPlay, driven by the graph). Never on the audio thread.
    /** Caches the sample rate and recomputes the derived PPQ rate.

        The MUSICAL position is preserved across a sample-rate change: the current
        PPQ is read at the old rate, then `samplePosition` is re-derived from it at
        the new rate, so a mid-session device/SR change does not teleport the
        playhead. A non-positive rate is clamped to 44100 (same defensive default
        as `MidiInputProcessor::prepareToPlay`). */
    void prepare (double sampleRate) noexcept;

    // RT-SAFE: audio thread. Called ONCE per block by TransportProcessor, before
    // any other node renders. Allocation-free, lock-free.
    /** Latches the block-start transport state, then advances the position by
        `numSamples` if playing. All getters below return the latched values.

        @param numSamples this block's length in samples (negative is clamped to 0). */
    void beginBlock (int numSamples) noexcept;

    // ── Latched block-start read API (what downstream nodes consume) ──────────
    // The sequencer node walks step boundaries with:
    //     for (ppq = firstBoundaryAtOrAfter (blockStartPpq ());
    //          ppq < blockEndPpq ();
    //          ppq += stepPpq)
    //         emitAt (int (blockOffsetForPpq (ppq)));
    // which is exact (see the PPQ note in the class comment) and buffer-size
    // independent. That is TRANSPORT-side exactness: the PPQ this class reports at a
    // given absolute sample does not depend on how the blocks were carved, full stop.
    // The SEQUENCER's placement of a boundary carries one bounded exception on top of
    // it — a boundary inside the step-index snap window just below a block edge can be
    // emitted up to one sample late. It belongs to the sequencer's snapping, not to
    // anything here; see "THE SNAP-BOUNDARY WINDOW" in SequencerProcessor.h (issue
    // #37) before quoting the line above as an unqualified guarantee.

    // RT-SAFE: latched.
    /** PPQ position at the FIRST sample of this block. */
    double blockStartPpq () const noexcept { return latchedPpq; }

    // RT-SAFE: latched.
    /** PPQ position one sample PAST the last sample of this block (exclusive end).
        A step boundary is inside this block iff
        `blockStartPpq() <= ppq < blockEndPpq()`. Equals `blockStartPpq()` when the
        transport is stopped or the block is empty. */
    double blockEndPpq () const noexcept
    {
        return latchedPpq + static_cast<double> (latchedAdvance) * ppqPerSampleValue;
    }

    // RT-SAFE: latched.
    /** Quarter notes per sample for this block (exact conversion factor). */
    double ppqPerSample () const noexcept { return ppqPerSampleValue; }

    // RT-SAFE: latched.
    /** This block's length in samples. */
    int blockLength () const noexcept { return latchedBlockLength; }

    // RT-SAFE: latched.
    /** PPQ at sample offset `offset` within this block. */
    double ppqAtBlockOffset (int offset) const noexcept
    {
        return latchedPpq + static_cast<double> (offset) * ppqPerSampleValue;
    }

    // RT-SAFE: latched.
    /** Fractional sample offset within this block at which `ppq` occurs. The
        caller floors/clamps into `[0, blockLength())` — returning the raw
        fractional value keeps micro-timing and swing rounding decisions with the
        sequencer instead of hiding them here. Returns 0 if the PPQ rate is
        degenerate (rate <= 0, only possible before `prepare()`). */
    double blockOffsetForPpq (double ppq) const noexcept
    {
        return ppqPerSampleValue > 0.0 ? (ppq - latchedPpq) / ppqPerSampleValue : 0.0;
    }

    // RT-SAFE: latched.
    /** True if the transport was running at the start of this block. */
    bool isPlaying () const noexcept { return latchedPlaying; }

    // RT-SAFE: latched.
    /** Tempo in effect for this block (BPM). */
    double bpm () const noexcept { return latchedBpm; }

    // RT-SAFE: latched.
    /** Timeline position of this block's first sample, in samples. */
    std::int64_t blockStartTimeInSamples () const noexcept { return latchedSamples; }

    // RT-SAFE: latched.
    /** Timeline position of this block's first sample, in seconds. */
    double blockStartTimeInSeconds () const noexcept { return latchedSeconds; }

    // RT-SAFE: latched.
    /** PPQ of the most recent bar start at or before `blockStartPpq()` (4/4). */
    double ppqOfLastBarStart () const noexcept { return latchedBarStartPpq; }

    // ── Discontinuity signals for the sequencer (§5.5 flush points) ───────────

    // RT-SAFE: latched.
    /** True for EXACTLY the block whose drain consumed a `transportStop`. The
        sequencer's primary flush trigger. */
    bool stoppedThisBlock () const noexcept { return latchedStopped; }

    // RT-SAFE: latched.
    /** True for EXACTLY the block in which the playhead was repositioned (an
        explicit locate, or the implicit rewind that `transportStop` performs). */
    bool positionJumpedThisBlock () const noexcept { return latchedPositionJumped; }

    // RT-SAFE: monotonic counter, not latched (bumped the instant a stop is
    // consumed). For consumers that cannot guarantee they see every block.
    /** Number of stops consumed since construction. Monotonically increasing. */
    std::uint64_t stopGeneration () const noexcept { return stopCounter; }

    // RT-SAFE: configuration read.
    /** Sample rate the transport was last prepared at (Hz). */
    double sampleRate () const noexcept { return currentSampleRate; }

    // ── ICommandSink ─────────────────────────────────────────────────────────

    // RT-SAFE: audio thread, from the transport head node's drain. Ignores every
    // command type this class does not own.
    /** Applies `transportPlay` / `transportStop` / `transportLocate` /
        `setTempoBpm`; ignores all other types. */
    void applyCommand (const EngineCommand& command) noexcept override;

private:
    // RT-SAFE: recomputes the derived rate and re-anchors the tempo segment at the
    // CURRENT sample position, preserving the PPQ already reached. One rounding
    // event per call, at a known sample.
    void reanchor (double newBpm) noexcept;

    // RT-SAFE: moves the playhead to `targetPpq` (assumed finite and >= 0),
    // re-deriving `samplePosition` from it and re-anchoring the segment there.
    void locateToPpq (double targetPpq) noexcept;

    // RT-SAFE: exact PPQ at an absolute sample position within the current segment.
    double ppqAtSample (std::int64_t sample) const noexcept
    {
        return anchorPpq + static_cast<double> (sample - anchorSample) * ppqPerSampleValue;
    }

    // RT-SAFE: copies the live state into the latched block-start members and
    // consumes the pending edge flags. Does NOT advance the position.
    void latchBlockStart (int numSamples) noexcept;

    // ── Live state (advances / mutates; audio thread, plus prepare()) ─────────
    std::int64_t samplePosition = 0;    ///< EXACT timeline position of the NEXT block's first sample.
    std::int64_t anchorSample = 0;      ///< Sample at which the current tempo segment starts.
    double anchorPpq = 0.0;             ///< PPQ at `anchorSample`.
    double ppqPerSampleValue = 0.0;     ///< Quarter notes per sample for the current segment.
    double currentBpm = defaultBpm;     ///< Current tempo (clamped to [minBpm, maxBpm]).
    double currentSampleRate = 44100.0; ///< Set by prepare().
    bool playing = false;               ///< Transport run state.

    // One-shot edges raised by applyCommand, consumed by the next latchBlockStart.
    bool stopEdge = false;         ///< A stop was consumed since the last latch.
    bool positionJumpEdge = false; ///< A locate/rewind was applied since the last latch.
    std::uint64_t stopCounter = 0; ///< Monotonic stop count (see stopGeneration()).

    // ── Latched block-start state (what every getter returns) ─────────────────
    double latchedPpq = 0.0;
    double latchedBpm = defaultBpm;
    double latchedSeconds = 0.0;
    double latchedBarStartPpq = 0.0;
    std::int64_t latchedSamples = 0;
    int latchedBlockLength = 0;
    /** Samples the position actually advanced by this block: `latchedBlockLength`
        when playing, 0 when stopped. `blockEndPpq()` uses it so a stopped
        transport reports a zero-length musical span. */
    int latchedAdvance = 0;
    bool latchedPlaying = false;
    bool latchedStopped = false;
    bool latchedPositionJumped = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Transport)
};
} // namespace arpbox::engine
