#include "SequencerProcessor.h"

#include <cmath>

namespace arpbox::engine
{
namespace
{
    // ── SCAFFOLD PATTERN DATA (Phase 6 DELETES THIS) ─────────────────────────
    // Semitone offsets from `scaffoldRootNote`, one per 16th-note step: one octave of
    // C major ascending (0 2 4 5 7 9 11 12), played twice across the 16-step bar. It
    // is deliberately synthetic — a rising scale is instantly recognisable by ear, so
    // a timing or note-off fault is audible without instrumentation.
    //
    // NOTE (§5.5 same-pitch retrigger): no two consecutive steps share a pitch, and a
    // pitch recurs only 8 steps later — far beyond the 50% gate — so the retrigger
    // path in `emitStep` is never exercised by this scaffold. It is implemented
    // anyway, because Phase 6's PITCH lane will exercise it immediately.
    //
    // Phase 6 replaces all of this with the adopted `PatternSnapshot`: 11 lanes over
    // up to 64 steps, per-lane length and clock division (§5.1 L1, §12.1).
    constexpr int scaffoldSemitoneOffsets[SequencerProcessor::scaffoldNumSteps] = { 0, 2, 4, 5, 7, 9, 11, 12,
                                                                                    0, 2, 4, 5, 7, 9, 11, 12 };

    // Reserved capacity (bytes) for the graph-owned OUTGOING MidiBuffer, ensured
    // unconditionally every block (ensureSize early-returns once satisfied). Sized for
    // this node's deterministic worst case: a full-table flush is
    // SoundingNoteTable::capacity note-offs plus up to 16 CC123 messages, and
    // juce::MidiBuffer stores a 3-byte event as 4 (position) + 2 (length) + 3 = 9
    // bytes → 272 * 9 ≈ 2.4 KB. 4 KB leaves room for the same block's note-ons and
    // for the upstream MIDI-in traffic already in the buffer.
    //
    // UNCONDITIONAL on purpose, exactly as MidiInputProcessor does it: the graph can
    // hand this node a FRESH, small pool buffer after a render-sequence rebuild (a
    // synth swap, or this node's own async insertion), and re-checking every block
    // re-warms that buffer — a first-block-only flag would miss it.
    constexpr int outgoingWarmupBytes = 4096;

    // ── DETERMINISM SNAPS (see the extended note in SequencerProcessor.h) ─────

    // Step-index snap, in units of one STEP. Absorbs the ulp-level disagreement
    // between block k's `blockEndPpq()` and block k+1's `blockStartPpq()`, which are
    // equal only in exact arithmetic. At the default 16th-note grid this is 1e-6 of a
    // step — ~0.006 samples at 120 BPM / 48 kHz, i.e. thousands of times larger than
    // the ~1e-10-step error it absorbs and thousands of times smaller than one sample.
    constexpr double stepIndexSnapSteps = 1.0e-6;

    // Sample-offset snap, in SAMPLES. `blockOffsetForPpq` subtracts two PPQ values
    // whose absolute magnitude grows with the timeline, so an offset that is
    // mathematically an exact integer can arrive as `integer - 1e-7`. Flooring that
    // would place the event one sample early, and only at some buffer sizes. 1e-4
    // samples (~2 ns at 48 kHz) is far above the residual error and far below one
    // sample, so the snap can never move an event to a different sample.
    constexpr double sampleOffsetSnapSamples = 1.0e-4;

    // RT-SAFE: index of the first step boundary at or after `ppq`, snapped. Applied to
    // BOTH ends of the half-open block interval so consecutive blocks agree exactly.
    std::int64_t snappedStepCeiling (double ppq, double stepPpq) noexcept
    {
        return static_cast<std::int64_t> (std::ceil (ppq / stepPpq - stepIndexSnapSteps));
    }

    // RT-SAFE: `stepIndex` reduced into [0, numSteps) for any sign of index.
    int wrapStepIndex (std::int64_t stepIndex, int numSteps) noexcept
    {
        const auto span = static_cast<std::int64_t> (numSteps);
        auto wrapped = stepIndex % span;
        if (wrapped < 0)
            wrapped += span;
        return static_cast<int> (wrapped);
    }
} // namespace

SequencerProcessor::SequencerProcessor ()
    : juce::AudioProcessor (BusesProperties ()) // MIDI-only: no audio buses
{
}

// MESSAGE-THREAD ONLY:
void SequencerProcessor::setSharedState (const Transport* transportToFollow) noexcept
{
    transport = transportToFollow;
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
}

// MESSAGE-THREAD ONLY:
void SequencerProcessor::releaseResources ()
{
    sounding.reset ();
    stopGenerationSeeded = false;
}

// RT-SAFE:
SequencerProcessor::StepEmission SequencerProcessor::describeStep (std::int64_t stepIndex) const noexcept
{
    // ── THE PHASE 6 SEAM (see StepEmission) ──────────────────────────────────
    // Everything below is scaffold. Phase 6 replaces the body with the §5.1
    // pipeline: note pool → pattern core lanes → step logic → operator stack →
    // constraint gate, reading the adopted PatternSnapshot. The SIGNATURE — a pure
    // function of the global step index — is what stays, because it is what makes the
    // offline render path (§9 MIDI drag-out) bit-identical to the real-time one.
    const int step = wrapStepIndex (stepIndex, scaffoldNumSteps);

    StepEmission emission;
    emission.gate = true; // scaffold: every step trigs (Phase 6: the GATE lane)
    emission.channel = scaffoldChannel;
    emission.note = scaffoldRootNote + scaffoldSemitoneOffsets[step];
    emission.velocity = scaffoldVelocity;
    emission.gateFractionOfStep = scaffoldGateFraction;

    return emission;
}

// RT-SAFE:
void SequencerProcessor::emitStep (const StepEmission& emission,
                                   juce::MidiBuffer& midi,
                                   int offset,
                                   std::int64_t onSample,
                                   double samplesPerStep) noexcept
{
    if (! emission.gate)
        return;

    const int channel = juce::jlimit (1, 16, emission.channel);
    const int note = juce::jlimit (0, 127, emission.note);
    const int velocity = juce::jlimit (1, 127, emission.velocity);

    // §5.5 overlap policy — same-pitch retrigger: note-off THEN note-on, with a
    // 1-sample gap. At offset 0 the gap cannot exist (there is no earlier sample in
    // this block), so both land on the same sample; juce::MidiBuffer preserves
    // insertion order among equal timestamps, so the off still precedes the on.
    if (const int existing = sounding.find (channel, note); existing >= 0)
        sounding.retireAt (existing, midi, juce::jmax (0, offset - 1));

    // Note length from the LEN-shaped gate fraction, resolved to samples at the
    // CURRENT tempo (see SoundingNoteTable.h on why the schedule is in samples). At
    // least one sample so a note can never be zero-length.
    const double lengthInSamples = emission.gateFractionOfStep * samplesPerStep;
    const std::int64_t lengthSamples =
        juce::jmax<std::int64_t> (1, static_cast<std::int64_t> (std::llround (lengthInSamples)));

    // Register BEFORE emitting: a full table must suppress the note-on, never leave an
    // untracked note sounding (SoundingNoteTable overflow policy).
    if (! sounding.add (channel, note, onSample + lengthSamples))
        return;

    const juce::uint8 bytes[3] = { static_cast<juce::uint8> (0x90 | ((channel - 1) & 0x0F)),
                                   static_cast<juce::uint8> (note & 0x7F),
                                   static_cast<juce::uint8> (velocity & 0x7F) };
    midi.addEvent (bytes, 3, offset);
}

// RT-SAFE:
bool SequencerProcessor::handleDiscontinuities (juce::MidiBuffer& midi) noexcept
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

    sounding.flush (midi, 0);
    jassert (sounding.isEmpty ()); // §5.5: the table MUST be empty after a flush point
    return true;
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

    // 1. Discontinuities first: flush at the block head before anything new is emitted.
    handleDiscontinuities (midi);

    // 2. Step boundaries inside this block. STATELESS: the global step index range is
    //    re-derived from the transport's latched PPQ span every block, which is what
    //    makes the emitted absolute sample positions identical at every buffer size
    //    (see the extended note in the header).
    const double ppqPerSample = transport->ppqPerSample ();
    const std::int64_t blockStartSample = transport->blockStartTimeInSamples ();

    // A stopped transport reports blockEndPpq() == blockStartPpq(), so the walk below
    // would produce an empty range anyway; the explicit guard just says so out loud.
    if (transport->isPlaying () && ppqPerSample > 0.0)
    {
        const double samplesPerStep = scaffoldStepPpq / ppqPerSample;

        // Half-open [firstIndex, endIndex) with the SAME snapped ceiling applied to
        // both ends, so block k's endIndex equals block k+1's firstIndex exactly.
        const std::int64_t firstIndex = snappedStepCeiling (transport->blockStartPpq (), scaffoldStepPpq);
        const std::int64_t endIndex = snappedStepCeiling (transport->blockEndPpq (), scaffoldStepPpq);

        for (std::int64_t index = firstIndex; index < endIndex; ++index)
        {
            const double boundaryPpq = static_cast<double> (index) * scaffoldStepPpq;

            // Snap up before flooring (see sampleOffsetSnapSamples), then clamp hard
            // into [0, numSamples). An out-of-range addEvent offset is a real hazard
            // when a boundary lands on the block edge, and juce::MidiBuffer does not
            // validate it — so the clamp is explicit rather than assumed.
            const double rawOffset = transport->blockOffsetForPpq (boundaryPpq);
            const auto snapped = static_cast<std::int64_t> (std::floor (rawOffset + sampleOffsetSnapSamples));
            const auto offset =
                static_cast<int> (juce::jlimit<std::int64_t> (0, static_cast<std::int64_t> (numSamples) - 1, snapped));

            emitStep (describeStep (index),
                      midi,
                      offset,
                      blockStartSample + static_cast<std::int64_t> (offset),
                      samplesPerStep);
        }
    }

    // 3. Note-offs that come due inside this block — INCLUDING notes started above,
    //    whose gate can be shorter than one block at fast tempos / large buffers.
    //    Emitted after the note-ons, which is fine: juce::MidiBuffer inserts in sorted
    //    sample order, so the buffer handed downstream is strictly sample-sorted (§5.5).
    sounding.emitDueNoteOffs (midi, blockStartSample, numSamples);
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
