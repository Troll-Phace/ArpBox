#include "PatternDocument.h"

// EngineGuiGuard.h is pulled in FIRST by PatternDocument.h (before any JUCE
// include), which is where the tripwire needs to sit; repeating it here would be
// a no-op. Same convention as sequencer/DirectionModes.cpp.
#include "Euclid.h"
#include "PatternSnapshot.h"
#include "PatternTypes.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>

namespace arpbox::engine
{
namespace
{
    /** The Phase 6 stub note pool: C major, one octave, ascending — the same
        pitches the Phase-5.2 scaffold hardcoded as semitone offsets from note 60
        ({0,2,4,5,7,9,11,12}). Phase 8.1 replaces this with the live THRU/SELF pool;
        nothing downstream changes, because everything already reads
        `PatternSetState::pool`. */
    constexpr std::uint8_t stubPoolNotes[] = { 60, 62, 64, 65, 67, 69, 71, 72 };
    constexpr int stubPoolSize = static_cast<int> (std::size (stubPoolNotes));
    static_assert (stubPoolSize == 8, "The scaffold-equivalent stub pool is one octave of C major.");

    /** Default active length for every lane, matching the scaffold's 16-step bar. */
    constexpr int defaultLaneLength = 16;

    /** True when `lane` names a real lane (not the `count` sentinel). */
    constexpr bool isRealLane (LaneId lane) noexcept
    {
        return static_cast<int> (lane) >= 0 && static_cast<int> (lane) < numLanes;
    }
} // namespace

// MESSAGE-THREAD ONLY:
PatternDocument::PatternDocument ()
{
    current.gridStepPpq = 0.25;
    current.startPatternIndex = 0;
    current.outputChannel = 1;

    current.pool.size = static_cast<std::uint8_t> (stubPoolSize);

    for (int i = 0; i < stubPoolSize; ++i)
    {
        // `sorted` == `asPlayed`: a stub pool has no arrival history, so the
        // as-played view degenerates to ascending (see NotePool.h).
        current.pool.sorted[static_cast<std::size_t> (i)] = stubPoolNotes[i];
        current.pool.asPlayed[static_cast<std::size_t> (i)] = stubPoolNotes[i];
    }

    for (auto& pattern : current.patterns)
    {
        pattern.direction = DirectionMode::up;
        pattern.euclid = EuclidParams {};
        pattern.masterSeed = 0;

        for (int laneIdx = 0; laneIdx < numLanes; ++laneIdx)
        {
            const auto laneId = static_cast<LaneId> (laneIdx);
            LaneState& lane = pattern.lanes[static_cast<std::size_t> (laneIdx)];

            lane.length = static_cast<std::uint8_t> (defaultLaneLength);
            lane.division = 1;

            // NEVER `{}` — see the musically-invalid-zero note on LaneState.
            lane.values.fill (laneDefault (laneId));
        }
    }

    // Pattern 0 is the audible one: GATE on across its active length. Patterns
    // 1–15 keep `laneDefault (gate)` == 0 and are therefore silent.
    LaneState& gate = laneOf (current.patterns[0], LaneId::gate);

    for (int step = 0; step < defaultLaneLength; ++step)
        gate.values[static_cast<std::size_t> (step)] = 1;
}

// ── EDIT PLUMBING ────────────────────────────────────────────────────────────

void PatternDocument::beginEdit ()
{
    if (transactionDepth > 0)
    {
        // The transaction already captured the pre-gesture state; just record that
        // the gesture did something, so an all-no-op transaction pushes nothing.
        transactionChanged = true;
        return;
    }

    pendingUndo = current;
}

void PatternDocument::endEdit ()
{
    if (transactionDepth > 0)
        return;

    pushUndo (std::move (pendingUndo));
    ++revision;
    republish ();
}

void PatternDocument::pushUndo (PatternSetState&& previous)
{
    // A new edit invalidates the redo branch — standard linear-history semantics.
    redoStack.clear ();

    undoStack.push_back (std::move (previous));

    while (static_cast<int> (undoStack.size ()) > maxUndoDepth)
        undoStack.pop_front ();
}

void PatternDocument::republish ()
{
    if (publishTarget != nullptr)
        publishTo (*publishTarget);
}

LaneState* PatternDocument::laneForEdit (int patternIndex, LaneId lane)
{
    if (patternIndex < 0 || patternIndex >= maxPatterns || ! isRealLane (lane))
    {
        jassertfalse;
        return nullptr;
    }

    return &laneOf (current.patterns[static_cast<std::size_t> (patternIndex)], lane);
}

// ── EDITS ────────────────────────────────────────────────────────────────────

// MESSAGE-THREAD ONLY:
bool PatternDocument::setLaneValue (int patternIndex, LaneId lane, int step, int value)
{
    LaneState* target = laneForEdit (patternIndex, lane);

    if (target == nullptr)
        return false;

    if (step < 0 || step >= maxSteps)
    {
        jassertfalse;
        return false;
    }

    const std::int16_t clamped = clampLaneValue (lane, value);
    auto& slot = target->values[static_cast<std::size_t> (step)];

    if (slot == clamped)
        return false;

    beginEdit ();
    slot = clamped;
    endEdit ();

    return true;
}

// MESSAGE-THREAD ONLY:
bool PatternDocument::setLaneLength (int patternIndex, LaneId lane, int length)
{
    LaneState* target = laneForEdit (patternIndex, lane);

    if (target == nullptr)
        return false;

    if (length < 1 || length > maxSteps)
    {
        jassertfalse;
        return false;
    }

    if (target->length == static_cast<std::uint8_t> (length))
        return false;

    beginEdit ();
    target->length = static_cast<std::uint8_t> (length);
    endEdit ();

    return true;
}

// MESSAGE-THREAD ONLY:
bool PatternDocument::setLaneDivision (int patternIndex, LaneId lane, int division)
{
    LaneState* target = laneForEdit (patternIndex, lane);

    if (target == nullptr)
        return false;

    if (division < 1 || division > maxLaneDivision)
    {
        jassertfalse;
        return false;
    }

    if (target->division == static_cast<std::uint8_t> (division))
        return false;

    beginEdit ();
    target->division = static_cast<std::uint8_t> (division);
    endEdit ();

    return true;
}

// MESSAGE-THREAD ONLY:
bool PatternDocument::setDirection (int patternIndex, DirectionMode direction)
{
    if (patternIndex < 0 || patternIndex >= maxPatterns || direction >= DirectionMode::count)
    {
        jassertfalse;
        return false;
    }

    PatternState& pattern = current.patterns[static_cast<std::size_t> (patternIndex)];

    if (pattern.direction == direction)
        return false;

    beginEdit ();
    pattern.direction = direction;
    endEdit ();

    return true;
}

// MESSAGE-THREAD ONLY:
bool PatternDocument::setMasterSeed (int patternIndex, std::uint64_t seed)
{
    if (patternIndex < 0 || patternIndex >= maxPatterns)
    {
        jassertfalse;
        return false;
    }

    PatternState& pattern = current.patterns[static_cast<std::size_t> (patternIndex)];

    if (pattern.masterSeed == seed)
        return false;

    beginEdit ();
    pattern.masterSeed = seed;
    endEdit ();

    return true;
}

// MESSAGE-THREAD ONLY:
bool PatternDocument::applyEuclid (int patternIndex, int steps, int pulses, int rotate)
{
    if (patternIndex < 0 || patternIndex >= maxPatterns)
    {
        jassertfalse;
        return false;
    }

    const int clampedSteps = juce::jlimit (1, maxSteps, steps);
    const int clampedPulses = juce::jlimit (0, clampedSteps, pulses);

    // The generator takes `rotate` modulo the necklace length itself and accepts
    // any sign, but int8 storage does not — reduce before narrowing.
    const int reducedRotate = static_cast<int> (stepFloorMod (rotate, clampedSteps));

    EuclidParams params;
    params.steps = static_cast<std::uint8_t> (clampedSteps);
    params.pulses = static_cast<std::uint8_t> (clampedPulses);
    params.rotate = static_cast<std::int8_t> (reducedRotate);
    params.enabled = true;

    // No cheap pre-check here: running the generator IS the comparison, and an
    // idempotent re-apply costing one undo slot is a better trade than a scratch
    // lane copy on every call. A user pressing the same euclid button twice
    // getting two undo entries is unsurprising behaviour.
    beginEdit ();

    PatternState& pattern = current.patterns[static_cast<std::size_t> (patternIndex)];
    pattern.euclid = params;
    euclid::applyToGateLane (params, laneOf (pattern, LaneId::gate));

    endEdit ();

    return true;
}

// MESSAGE-THREAD ONLY:
bool PatternDocument::setEuclidEnabled (int patternIndex, bool enabled)
{
    if (patternIndex < 0 || patternIndex >= maxPatterns)
    {
        jassertfalse;
        return false;
    }

    PatternState& pattern = current.patterns[static_cast<std::size_t> (patternIndex)];

    if (pattern.euclid.enabled == enabled)
        return false;

    beginEdit ();
    pattern.euclid.enabled = enabled;
    endEdit ();

    return true;
}

// MESSAGE-THREAD ONLY:
bool PatternDocument::setGrid (double stepPpq)
{
    // A non-positive or non-finite grid would divide the transport's PPQ by zero
    // (or by NaN) in the step-boundary walk — reject rather than clamp, because
    // there is no sane value to clamp a NaN grid to.
    if (! (stepPpq > 0.0) || ! std::isfinite (stepPpq))
    {
        jassertfalse;
        return false;
    }

    // EXACT equality is the right test and `juce::exactlyEqual` is how you say so
    // without tripping -Wfloat-equal: the question is "is this a no-op edit", i.e.
    // would the stored bits change, not "are these two grids musically close".
    if (juce::exactlyEqual (current.gridStepPpq, stepPpq))
        return false;

    beginEdit ();
    current.gridStepPpq = stepPpq;
    endEdit ();

    return true;
}

// MESSAGE-THREAD ONLY:
bool PatternDocument::setPool (const PoolSnapshot& pool)
{
    PoolSnapshot clamped = pool;

    if (clamped.size > static_cast<std::uint8_t> (maxPoolSize))
        clamped.size = static_cast<std::uint8_t> (maxPoolSize);

    // Compare only the LIVE prefix: entries at or beyond `size` are unspecified
    // (NotePool.h), so differing garbage in the tail is not a change.
    if (clamped.size == current.pool.size)
    {
        bool identical = true;

        for (int i = 0; i < static_cast<int> (clamped.size) && identical; ++i)
        {
            const auto index = static_cast<std::size_t> (i);
            identical = clamped.sorted[index] == current.pool.sorted[index] &&
                        clamped.asPlayed[index] == current.pool.asPlayed[index];
        }

        if (identical)
            return false;
    }

    beginEdit ();
    current.pool = clamped;
    endEdit ();

    return true;
}

// MESSAGE-THREAD ONLY:
bool PatternDocument::setOutputChannel (int channel)
{
    if (channel < 1 || channel > 16)
    {
        jassertfalse;
        return false;
    }

    if (current.outputChannel == channel)
        return false;

    beginEdit ();
    current.outputChannel = channel;
    endEdit ();

    return true;
}

// MESSAGE-THREAD ONLY:
bool PatternDocument::setStartPatternIndex (int patternIndex)
{
    if (patternIndex < 0 || patternIndex >= maxPatterns)
    {
        jassertfalse;
        return false;
    }

    if (current.startPatternIndex == patternIndex)
        return false;

    beginEdit ();
    current.startPatternIndex = patternIndex;
    endEdit ();

    return true;
}

// ── TRANSACTIONS ─────────────────────────────────────────────────────────────

// MESSAGE-THREAD ONLY:
void PatternDocument::beginTransaction ()
{
    if (transactionDepth == 0)
    {
        pendingUndo = current;
        transactionChanged = false;
    }

    ++transactionDepth;
}

// MESSAGE-THREAD ONLY:
void PatternDocument::endTransaction ()
{
    if (transactionDepth <= 0)
    {
        jassertfalse; // Unbalanced endTransaction — ignored rather than underflowed.
        return;
    }

    --transactionDepth;

    if (transactionDepth > 0)
        return;

    if (! transactionChanged)
        return; // Nothing happened: no undo slot, no snapshot build, no publish.

    transactionChanged = false;
    pushUndo (std::move (pendingUndo));
    ++revision;
    republish ();
}

// ── UNDO ─────────────────────────────────────────────────────────────────────

// MESSAGE-THREAD ONLY:
bool PatternDocument::undo ()
{
    if (transactionDepth > 0)
    {
        jassertfalse; // Undoing mid-gesture would discard half of it.
        return false;
    }

    if (undoStack.empty ())
        return false;

    redoStack.push_back (std::move (current));
    current = std::move (undoStack.back ());
    undoStack.pop_back ();

    ++revision;
    republish ();

    return true;
}

// MESSAGE-THREAD ONLY:
bool PatternDocument::redo ()
{
    if (transactionDepth > 0)
    {
        jassertfalse;
        return false;
    }

    if (redoStack.empty ())
        return false;

    undoStack.push_back (std::move (current));
    current = std::move (redoStack.back ());
    redoStack.pop_back ();

    ++revision;
    republish ();

    return true;
}

// MESSAGE-THREAD ONLY:
void PatternDocument::clearUndoHistory ()
{
    undoStack.clear ();
    redoStack.clear ();
}

// ── PUBLISHING ───────────────────────────────────────────────────────────────

// MESSAGE-THREAD ONLY:
void PatternDocument::publishTo (PatternChannel& channel)
{
    // Reclaim BEFORE publishing: this call is already on the message thread, so
    // draining here keeps memory bounded during a long edit session with no UI
    // tick, and lets a headless test run the full cycle unaided.
    channel.reclaim ();

    ++buildCounter;
    channel.publish (buildPatternSnapshot (current, buildCounter));
}
} // namespace arpbox::engine
