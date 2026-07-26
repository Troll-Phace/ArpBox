#include "Transport.h"

#include <cmath>

namespace arpbox::engine
{
namespace
{
    // Seconds per minute — spelled out so the ppqPerSample derivation reads as the
    // musical identity it is: quarter notes per second = bpm / 60.
    constexpr double secondsPerMinute = 60.0;
} // namespace

Transport::Transport ()
{
    // Derive the initial rate from the defaults so the transport is usable (and its
    // latched view self-consistent) even before prepare() runs.
    ppqPerSampleValue = currentBpm / (secondsPerMinute * currentSampleRate);
    latchBlockStart (0);
}

// MESSAGE-THREAD ONLY:
void Transport::prepare (double sampleRate) noexcept
{
    const double newRate = sampleRate > 0.0 ? sampleRate : 44100.0;

    // Preserve the MUSICAL position across the rate change: read PPQ at the old
    // rate FIRST, then re-derive the sample position from it at the new rate. A
    // device switch (48k → 44.1k) therefore keeps the playhead where the user left
    // it instead of teleporting it by the ratio of the two rates.
    const double ppqNow = ppqAtSample (samplePosition);

    currentSampleRate = newRate;
    ppqPerSampleValue = currentBpm / (secondsPerMinute * currentSampleRate);

    locateToPpq (ppqNow);

    // A prepare is not a musical event: do not leave a spurious jump edge for the
    // sequencer (audio is stopped here anyway).
    positionJumpEdge = false;

    // Publish a consistent zero-length latched view so any getter read before the
    // first beginBlock() returns sane values rather than pre-prepare state.
    latchBlockStart (0);
}

// RT-SAFE:
void Transport::reanchor (double newBpm) noexcept
{
    // Re-anchor AT the current sample so the PPQ already reached is carried over
    // exactly. Note that two reanchors at the same `samplePosition` produce the
    // identical `anchorPpq` (the sample delta is 0), so several tempo commands in
    // one block cost no extra rounding.
    const double ppqNow = ppqAtSample (samplePosition);

    anchorSample = samplePosition;
    anchorPpq = ppqNow;

    currentBpm = juce::jlimit (minBpm, maxBpm, newBpm);
    ppqPerSampleValue = currentBpm / (secondsPerMinute * currentSampleRate);
}

// RT-SAFE:
void Transport::locateToPpq (double targetPpq) noexcept
{
    // Derive the integer sample position from the musical target, then anchor the
    // segment exactly there so ppqAtSample(samplePosition) == targetPpq with no
    // residual error.
    //
    // KNOWN SEAM (issue #40) — single-segment PPQ→sample mapping. The division below
    // scales the target PPQ by the CURRENT tempo, i.e. as though that tempo had held
    // since PPQ 0. PPQ is exact either way (the anchor pair below pins it), and the
    // determinism contract (§1.2) rides on PPQ only — but after a session containing
    // tempo changes, the `timeInSamples`/`timeInSeconds` fields reported to hosted
    // plugins across a locate are a single-segment APPROXIMATION. Acceptable for the
    // MVP (§8.1's `transport` carries one `bpm`). When the post-MVP tempo map lands,
    // this must walk the segment list instead of assuming one segment.
    samplePosition =
        ppqPerSampleValue > 0.0 ? static_cast<std::int64_t> (std::llround (targetPpq / ppqPerSampleValue)) : 0;
    anchorSample = samplePosition;
    anchorPpq = targetPpq;
}

// RT-SAFE:
void Transport::latchBlockStart (int numSamples) noexcept
{
    const int length = juce::jmax (0, numSamples);

    latchedPpq = ppqAtSample (samplePosition);
    latchedSamples = samplePosition;
    latchedSeconds = currentSampleRate > 0.0 ? static_cast<double> (samplePosition) / currentSampleRate : 0.0;
    latchedBpm = currentBpm;
    latchedPlaying = playing;
    latchedBlockLength = length;
    latchedAdvance = playing ? length : 0;

    // Fixed 4/4 (§ class comment): the bar grid is a multiple of quarterNotesPerBar
    // from PPQ 0. std::floor keeps it correct for the (rejected today, but cheap to
    // stay honest about) negative case.
    latchedBarStartPpq = std::floor (latchedPpq / quarterNotesPerBar) * quarterNotesPerBar;

    // Consume the one-shot edges: each is visible for EXACTLY the block whose drain
    // raised it.
    latchedStopped = stopEdge;
    latchedPositionJumped = positionJumpEdge;
    stopEdge = false;
    positionJumpEdge = false;
}

// RT-SAFE:
void Transport::beginBlock (int numSamples) noexcept
{
    // Order matters and mirrors ARCHITECTURE §4 step 2. Commands (including tempo
    // changes and locates) were already applied by applyCommand during the head
    // node's drain, which ran immediately before this call and before ANY node
    // rendered — that is what makes every tempo change land on a block boundary.
    latchBlockStart (numSamples);

    // Advance for the NEXT block. Integer addition on an int64 counter: exact, and
    // the only per-block mutation of position — PPQ is always re-derived from it, so
    // nothing accumulates in floating point.
    if (playing)
        samplePosition += static_cast<std::int64_t> (latchedBlockLength);
}

// RT-SAFE:
void Transport::applyCommand (const EngineCommand& command) noexcept
{
    switch (command.type)
    {
    case EngineCommandType::transportPlay:
        playing = true;
        break;

    case EngineCommandType::transportStop:
        // Stop AND rewind to the start (groovebox convention — there is no
        // pause in the MVP; see the class comment). The edges below are what
        // the sequencer node uses to flush its sounding-note table in THIS
        // block (§5.5). The stop counter is bumped UNCONDITIONALLY, even for a
        // redundant stop while already stopped: a re-requested flush costs one
        // CC123 sweep, whereas suppressing it risks a hung note, and hung notes
        // are the worse failure by a wide margin.
        playing = false;
        stopEdge = true;
        ++stopCounter;
        locateToPpq (0.0);
        positionJumpEdge = true;
        break;

    case EngineCommandType::transportLocate:
        // value.d = target PPQ. Reject non-finite and negative targets: a NaN
        // would poison every subsequent ppqAtSample() (and jlimit(NaN) is
        // ill-defined), and the timeline starts at 0. Same defensive posture as
        // MasterProcessor::applyCommand after issue #3.
        if (std::isfinite (command.value.d) && command.value.d >= 0.0)
        {
            locateToPpq (command.value.d);
            positionJumpEdge = true;
        }
        break;

    case EngineCommandType::setTempoBpm:
        // value.d = target BPM. Drop a non-finite value (keep the current
        // tempo); otherwise clamp into [minBpm, maxBpm] before re-anchoring.
        if (std::isfinite (command.value.d))
            reanchor (command.value.d);
        break;

    // ── NOT OURS — the fan-out no-op arm (ICommandSink.h dispatch contract) ─────
    // Every enumerator this sink does not own is listed EXPLICITLY and there is no
    // `default:`. Runtime behaviour is identical to the `default: break;` this
    // replaced (a value outside the enum simply matches no arm and leaves the
    // switch — still a no-op), but the compile-time behaviour is not: a new
    // `EngineCommandType` matches nothing here, and clang reports it twice over —
    // `-Wswitch-enum` (from juce_recommended_warning_flags) AND `-Wswitch` (inside
    // -Wall) — which `lint.sh warnings` turns into a build failure (#70, #79). The
    // author of the new command then has to decide, per sink, whether Transport
    // cares. That decision being forced is the whole point; a `default:` arm makes
    // it silently for you.
    case EngineCommandType::none:
    case EngineCommandType::setMasterGainDb:
    case EngineCommandType::setLimiterEnabled:
    case EngineCommandType::setTestToneEnabled:
    case EngineCommandType::setTestToneFrequency:
    case EngineCommandType::queuePatternSwitch:
    case EngineCommandType::setFillHeld:
        break;
    }
}
} // namespace arpbox::engine
