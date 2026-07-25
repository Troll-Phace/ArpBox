#include "SequencerProcessor.h"

#include "../midi/NotePool.h"

#include <cmath>
#include <cstddef>

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

    // RT-SAFE: pure indexing. The `PatternData` sibling of `laneOf (PatternState&…)`
    // in PatternTypes.h — a snapshot's lanes are the same array in a different struct.
    const LaneState& laneOf (const PatternData& data, LaneId lane) noexcept
    {
        return data.lanes[static_cast<std::size_t> (lane)];
    }
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
    //   permanently SILENT after the user switches audio device. Holding a ~100 KB
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

    default:
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
SequencerProcessor::StepEmission SequencerProcessor::describeStep (std::int64_t stepIndex) const noexcept
{
    // ── L0 NOTE POOL → L1 PATTERN CORE (§5.1) ────────────────────────────────
    // A pure function of `stepIndex` plus two values fixed for the whole block
    // (`activeSnapshot`, `activePatternIndex`). No cursor, no accumulator: that is
    // what makes §9's offline drag-out render bit-identical to real time, and what
    // lets the transport be located anywhere without the arpeggio shifting.
    //
    // LANES READ HERE (Phase 6): GATE, PITCH, OCT, VEL, LEN.
    // LANES STORED BUT NOT READ, and by whom they will be:
    //   RATCHET, MICRO → Phase 7.2 (ratchet subdivision, micro-timing + swing)
    //   PROB, COND     → Phase 7.1 (probability roll, §12.2 trig conditions)
    //   MOD A, MOD B   → Phase 14.1 (mod-matrix per-step sources)
    // The omission is deliberate, not an oversight — see LaneId in PatternTypes.h,
    // which carries the same per-lane phase annotations.

    if (activeSnapshot == nullptr)
        return {}; // nothing adopted yet ⇒ silence (gate defaults to false)

    const PatternData& data = activeSnapshot->pattern (activePatternIndex);

    // GATE: a true clock divider (`isLaneTick`) AND a non-zero held value. This is the
    // SAME predicate `gatePrefixPulses` was summed from, so the fired steps and the
    // pool ordinal cannot drift apart.
    if (! PatternSnapshot::isGated (data, stepIndex))
        return {};

    // The pool size is clamped even though `PatternDocument::setPool` already clamps:
    // this dereferences a fixed-size array on the audio thread from data that arrived
    // through a pointer swap, and a guard is cheaper than a corrupted read.
    const int poolSize = juce::jmin (maxPoolSize, static_cast<int> (activeSnapshot->pool.size));

    // Which pool DEGREE this step lands on: the gated ordinal run through the
    // pre-built traversal table for the active direction mode (§12.3).
    const int poolIndex = activeSnapshot->poolIndexAt (data, stepIndex, poolSize);

    if (poolIndex < 0)
        return {}; // empty pool / degenerate traversal — the caller must not emit

    // PITCH is a DEGREE offset, not a semitone offset (§12.1): `poolNoteAtDegree`
    // wraps it through the pool with octave carry, so the pattern keeps working when
    // the held chord changes size. OCT is the straight ±4-octave transpose on top.
    const int degree = poolIndex + laneValueAt (laneOf (data, LaneId::pitch), stepIndex);
    const int poolNote = poolNoteAtDegree (poolNotes (activeSnapshot->pool, data.asPlayedView), poolSize, degree);

    StepEmission emission;
    emission.gate = true;
    emission.channel = activeSnapshot->outputChannel;

    // Neither of these is range-clamped here: `poolNoteAtDegree` deliberately leaves
    // fold-vs-clamp to the constraint gate (Phase 12.3), and `emitStep` applies the
    // hard 0..127 / 1..127 MIDI limits as the last line of defence.
    emission.note = poolNote + 12 * laneValueAt (laneOf (data, LaneId::oct), stepIndex);
    emission.velocity = laneValueAt (laneOf (data, LaneId::vel), stepIndex);

    // LEN is stored as a PERCENTAGE of the step, 1..400 (§12.1); >100% ⇒ tie/legato.
    emission.gateFractionOfStep = static_cast<double> (laneValueAt (laneOf (data, LaneId::len), stepIndex)) / 100.0;

    return emission;
}

// RT-SAFE:
std::int64_t SequencerProcessor::stepBoundarySample (std::int64_t index,
                                                     double stepPpq,
                                                     std::int64_t blockStartSample) const noexcept
{
    // THE SAME two operations the step walk performs on a boundary — snap up, then
    // floor — with the walk's block clamp deliberately omitted, because the point of
    // this function is to describe boundaries OUTSIDE the current block. Callers must
    // not use it to place an event; the walk owns in-block placement.
    const double boundaryPpq = static_cast<double> (index) * stepPpq;
    const double rawOffset = transport->blockOffsetForPpq (boundaryPpq);

    return blockStartSample + static_cast<std::int64_t> (std::floor (rawOffset + sampleOffsetSnapSamples));
}

// RT-SAFE:
std::int64_t SequencerProcessor::cutoffForSamePitch (std::int64_t stepIndex,
                                                     int channel,
                                                     int note,
                                                     std::int64_t onSample,
                                                     std::int64_t naturalDueSample,
                                                     double stepPpq,
                                                     std::int64_t blockStartSample,
                                                     int lookaheadSteps) const noexcept
{
    if (activeSnapshot == nullptr || transport == nullptr || stepPpq <= 0.0)
        return naturalDueSample;

    for (int ahead = 1; ahead <= lookaheadSteps; ++ahead)
    {
        const std::int64_t index = stepIndex + ahead;

        // STOP AT A RESOLVED PATTERN SWITCH. `describeStep` reads
        // `activePatternIndex`, so from the adopt step onwards it would describe the
        // OUTGOING pattern's lanes for steps the INCOMING pattern will play. Nothing
        // is lost by stopping: `flushForPatternSwitch` releases this note at
        // `adoptSample - 1` anyway, which is at or before any cutoff we could have
        // found beyond the switch.
        if (pendingResolved && index >= adoptStepIndex)
            break;

        const std::int64_t boundary = stepBoundarySample (index, stepPpq, blockStartSample);

        // THE BOUND. Boundaries increase monotonically, so once one lies past the
        // note's natural end no later one can shorten it. This is what makes the scan
        // cost ~ceil(LEN%) iterations rather than unbounded.
        if (boundary > naturalDueSample)
            break;

        const StepEmission next = describeStep (index);

        if (! next.gate)
            continue;

        // The SAME clamps `emitStep` applies below, so the comparison is against the
        // pitch/channel that will actually be emitted rather than the raw lane value.
        if (juce::jlimit (1, 16, next.channel) != channel || juce::jlimit (0, 127, next.note) != note)
            continue;

        // §5.5's 1-sample gap, on the absolute timeline. `jmax` guards only the
        // degenerate `samplesPerStep <= 1` case (unreachable: the coarsest supported
        // combination, 1/32-triplet at 300 BPM / 44.1 kHz, still gives 735 samples),
        // where it would otherwise schedule an off before its own on.
        return juce::jmax (onSample, boundary - 1);
    }

    return naturalDueSample;
}

// RT-SAFE:
void SequencerProcessor::emitStep (const StepEmission& emission,
                                   juce::MidiBuffer& midi,
                                   std::int64_t stepIndex,
                                   int offset,
                                   std::int64_t onSample,
                                   std::int64_t blockStartSample,
                                   int numSamples,
                                   double stepPpq,
                                   double samplesPerStep) noexcept
{
    if (! emission.gate)
        return;

    const int channel = juce::jlimit (1, 16, emission.channel);
    const int note = juce::jlimit (0, 127, emission.note);
    const int velocity = juce::jlimit (1, 127, emission.velocity);

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
    if (const int existing = sounding.find (channel, note); existing >= 0)
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
    const double lengthInSamples = emission.gateFractionOfStep * samplesPerStep;
    const std::int64_t lengthSamples =
        juce::jmax<std::int64_t> (1, static_cast<std::int64_t> (std::llround (lengthInSamples)));

    // How far the lookahead has to reach, DERIVED FROM THIS NOTE rather than fixed:
    // a note spanning L samples can only be cut short by a step inside those L
    // samples, i.e. by roughly L / samplesPerStep steps. Computed in integers (the
    // floor of `samplesPerStep` only ever makes the estimate larger, never smaller)
    // so no float edge case can under-reach, then capped at
    // `maxRetriggerLookaheadSteps`. See that constant for where 5 comes from.
    const auto stepSamplesFloor = static_cast<std::int64_t> (samplesPerStep);
    const int lookaheadSteps =
        stepSamplesFloor >= 1
            ? static_cast<int> (
                  juce::jlimit<std::int64_t> (1, maxRetriggerLookaheadSteps, lengthSamples / stepSamplesFloor + 1))
            : maxRetriggerLookaheadSteps;

    // THE ISSUE #46 FIX (see "EVERY NOTE-OFF IS SCHEDULED" in the header): decide the
    // off's ABSOLUTE sample now — `min (natural end, next same-pitch on - 1)` — and
    // let `SoundingNoteTable::emitDueNoteOffs` place it in whichever block contains
    // it, including the block BEFORE this one when the retrigger lands on a block
    // head. That is the placement `offset - 1` structurally could not express.
    const std::int64_t dueOffSample = cutoffForSamePitch (stepIndex,
                                                          channel,
                                                          note,
                                                          onSample,
                                                          onSample + lengthSamples,
                                                          stepPpq,
                                                          blockStartSample,
                                                          lookaheadSteps);

    // Register BEFORE emitting: a full table must suppress the note-on, never leave an
    // untracked note sounding (SoundingNoteTable overflow policy).
    if (! sounding.add (channel, note, dueOffSample))
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
    //    in-block readers of the snapshot are `describeStep` and the grid, and both sit
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
    if (transport->isPlaying () && ppqPerSample > 0.0)
    {
        const double samplesPerStep = curStepPpq / ppqPerSample;

        // Half-open [firstIndex, endIndex) with the SAME snapped ceiling applied to
        // both ends, so block k's endIndex equals block k+1's firstIndex exactly.
        const std::int64_t firstIndex = snappedStepCeiling (transport->blockStartPpq (), curStepPpq);
        const std::int64_t endIndex = snappedStepCeiling (transport->blockEndPpq (), curStepPpq);

        for (std::int64_t index = firstIndex; index < endIndex; ++index)
        {
            const double boundaryPpq = static_cast<double> (index) * curStepPpq;

            // Snap up before flooring (see sampleOffsetSnapSamples), then clamp hard
            // into [0, numSamples). An out-of-range addEvent offset is a real hazard
            // when a boundary lands on the block edge, and juce::MidiBuffer does not
            // validate it — so the clamp is explicit rather than assumed.
            //
            // THE LOWER CLAMP IS ALSO THE #37 WINDOW. A boundary within
            // `stepIndexSnapSteps` below this block's start was claimed by THIS block
            // by the snapped ceiling, so `rawOffset` comes back slightly negative and
            // the clamp emits it at 0 — up to one sample later than a carving that put
            // it mid-block. That is the documented exception, not a defect to "fix" by
            // deleting the clamp; see "THE SNAP-BOUNDARY WINDOW" in the header.
            const double rawOffset = transport->blockOffsetForPpq (boundaryPpq);
            const auto snapped = static_cast<std::int64_t> (std::floor (rawOffset + sampleOffsetSnapSamples));
            const auto offset =
                static_cast<int> (juce::jlimit<std::int64_t> (0, static_cast<std::int64_t> (numSamples) - 1, snapped));

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
            // THE TEST IS ON THE INDEX, NOT ON THE EMISSION: the switch must fire even
            // when the adopt step is not gated, or a pattern whose target step happens
            // to be a rest would never switch.
            const std::int64_t onSample = blockStartSample + static_cast<std::int64_t> (offset);

            if (pendingResolved && index == adoptStepIndex)
            {
                // Normally a no-op by now: the pre-flush below, or the switch-aware
                // bound in `cutoffForSamePitch`, has already retired everything the
                // outgoing pattern was holding. It still runs — the table, not an
                // inference about the table, is what §5.5 requires to be empty.
                flushForPatternSwitch (midi, onSample, blockStartSample, numSamples);
                activePatternIndex = pendingPatternIndex;
                clearPendingSwitch ();
            }

            emitStep (describeStep (index),
                      midi,
                      index,
                      offset,
                      onSample,
                      blockStartSample,
                      numSamples,
                      curStepPpq,
                      samplesPerStep);
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
