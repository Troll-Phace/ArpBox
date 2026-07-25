#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// NoteLifecycleCheck — the reusable "no orphan note-ons, no orphan note-offs"
// invariant checker (ARCHITECTURE §5.5 "Engine owns every note-off"; INSTRUCTIONS
// Phase 5.3e, and the SEED OF PHASE 8.3's hanging-note fuzzer).
//
// WHY A CHECKER AND NOT HAND-PAIRED ASSERTIONS. Hand-pairing "event[0] is a
// note-on, event[1] is its note-off" only works for a stream the test already knows
// the shape of. The interesting streams — a churned transport, a pattern switch, a
// 10k-event fuzz run — have no known shape, and their invariant is not "these two
// events pair up" but "the multiset of ons and offs balances, per (channel, note),
// with no off ever preceding its on". That is a running fold over the stream, which
// is what this class is. Phase 8.3 reuses it verbatim: `observeAll(render)` then
// `REQUIRE (tracker.balanced())`.
//
// CC123 IS DELIBERATELY IGNORED. §5.5 makes the PER-NOTE offs the real mechanism
// ("hosted plugins honour CC123 inconsistently"), with the all-notes-off sweep as
// belt and braces. If this tracker let a CC123 zero its counts it would report a
// balanced stream for a flush that emitted ONLY the sweep — i.e. it would be blind
// to precisely the bug it exists to catch. So CC123 is counted for diagnostics and
// changes nothing about the balance.
//
// VELOCITY-0 NOTE-ONS count as note-offs (`juce::MidiMessage::isNoteOff()` default),
// because that is what every synth does with them; a stream that released notes that
// way would otherwise look like it leaked.
//
// This is a test helper: it allocates its fixed 16x128 count table on construction
// and nothing afterwards, so it may be fed from inside a render loop, but never from
// a real audio thread.
// ─────────────────────────────────────────────────────────────────────────────

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cstdint>

namespace arpbox::testing
{
/** Running note-on/note-off balance per (channel, note), plus the two failure
    counters that matter: note-ons never released (`outstanding()`) and note-offs
    for a pitch that was not sounding (`orphanNoteOffs()`).

    Typical use:
    @code
        NoteLifecycleTracker tracker;
        tracker.observeAll (render);              // any range of {.message}
        INFO (tracker.describe ());
        REQUIRE (tracker.balanced ());            // 0 outstanding AND 0 orphans
    @endcode

    `balanced()` is only meaningful at a point where the engine claims every note is
    released — after a flush point (stop / locate / panic), or at the end of a run
    that finished with one. Mid-gate it is expected to be non-zero. */
class NoteLifecycleTracker
{
public:
    /** MIDI channels tracked (1..16 mapped to 0..15). */
    static constexpr int numChannels = 16;
    /** MIDI note numbers tracked (0..127). */
    static constexpr int numNotes = 128;

    /** Constructs an empty tracker (nothing sounding, no orphans). */
    NoteLifecycleTracker () = default;

    // ── Feeding ──────────────────────────────────────────────────────────────

    /** Folds one message into the balance. Note-ons increment, note-offs (including
        velocity-0 note-ons) decrement — or, when nothing was sounding for that
        pitch, count as an orphan note-off and leave the balance at zero. Everything
        else is ignored except CC123, which is counted for diagnostics only. */
    void observe (const juce::MidiMessage& message) noexcept
    {
        const int channel = message.getChannel ();
        if (channel < 1 || channel > numChannels)
        {
            if (message.isControllerOfType (allNotesOffController))
                ++allNotesOffSweeps;
            return;
        }

        if (message.isControllerOfType (allNotesOffController))
        {
            // Counted, never applied — see the header note on why.
            ++allNotesOffSweeps;
            return;
        }

        const int note = message.getNoteNumber ();
        if (note < 0 || note >= numNotes)
            return;

        // isNoteOff() FIRST: a velocity-0 note-on is a release, and isNoteOn()
        // (default) already agrees by returning false for it.
        if (message.isNoteOff ())
        {
            ++noteOffs;
            int& live = at (channel, note);
            if (live > 0)
                --live;
            else
                ++orphans;
        }
        else if (message.isNoteOn ())
        {
            ++noteOns;
            ++at (channel, note);
        }
    }

    /** Folds every message in a `juce::MidiBuffer` (one rendered block). */
    void observeBuffer (const juce::MidiBuffer& midi) noexcept
    {
        for (const auto meta : midi)
            observe (meta.getMessage ());
    }

    /** Folds any range whose elements expose `.message` — in particular a
        `MidiRenderResult` or a `std::vector<TimedMidiEvent>` from
        `support/MidiRenderHarness.h`. Templated so this header stays independent of
        the render harness. */
    template <typename EventRange>
    void observeAll (const EventRange& events)
    {
        for (const auto& event : events)
            observe (event.message);
    }

    // ── The invariant ────────────────────────────────────────────────────────

    /** Total note-ons not yet released, summed over every (channel, note). */
    int outstanding () const noexcept
    {
        int total = 0;
        for (const int live : counts)
            total += live;
        return total;
    }

    /** Note-ons not yet released for one specific pitch (0 if out of range). */
    int outstandingFor (int channel, int note) const noexcept
    {
        if (channel < 1 || channel > numChannels || note < 0 || note >= numNotes)
            return 0;
        return counts[index (channel, note)];
    }

    /** Note-offs seen for a pitch that was not sounding. Non-zero means the engine
        released a note it never started — the mirror-image bug of a hung note. */
    std::int64_t orphanNoteOffs () const noexcept { return orphans; }

    /** True when nothing is sounding AND no orphan note-off was ever seen. THE
        assertion; see the class comment on when it is meaningful. */
    bool balanced () const noexcept { return outstanding () == 0 && orphans == 0; }

    // ── Diagnostics ──────────────────────────────────────────────────────────

    /** Note-ons folded in so far. Assert this is non-zero before trusting a
        `balanced()` pass: an empty stream is trivially balanced. */
    std::int64_t noteOnsSeen () const noexcept { return noteOns; }

    /** Note-offs folded in so far. */
    std::int64_t noteOffsSeen () const noexcept { return noteOffs; }

    /** CC123 all-notes-off messages seen (diagnostic only — never applied). */
    std::int64_t allNotesOffSeen () const noexcept { return allNotesOffSweeps; }

    /** One-line counts plus, when the stream is unbalanced, the offending pitches —
        which is what a failing fuzz seed needs to be diagnosable. */
    juce::String describe (int maxPitches = 16) const
    {
        juce::String text;
        text << "note lifecycle: " << juce::String (noteOns) << " on, " << juce::String (noteOffs)
             << " off, " << juce::String (allNotesOffSweeps) << " CC123, outstanding "
             << juce::String (outstanding ()) << ", orphan offs " << juce::String (orphans);

        int shown = 0;
        for (int channel = 1; channel <= numChannels && shown < maxPitches; ++channel)
        {
            for (int note = 0; note < numNotes && shown < maxPitches; ++note)
            {
                const int live = counts[index (channel, note)];
                if (live == 0)
                    continue;

                text << "\n  STILL SOUNDING ch" << juce::String (channel) << " note "
                     << juce::String (note) << " x" << juce::String (live);
                ++shown;
            }
        }
        return text;
    }

    /** Clears every count and counter. */
    void reset () noexcept
    {
        counts.fill (0);
        noteOns = 0;
        noteOffs = 0;
        orphans = 0;
        allNotesOffSweeps = 0;
    }

private:
    static constexpr int allNotesOffController = 123;

    static std::size_t index (int channel, int note) noexcept
    {
        return static_cast<std::size_t> ((channel - 1) * numNotes + note);
    }

    int& at (int channel, int note) noexcept { return counts[index (channel, note)]; }

    std::array<int, static_cast<std::size_t> (numChannels * numNotes)> counts {};
    std::int64_t noteOns = 0;
    std::int64_t noteOffs = 0;
    std::int64_t orphans = 0;
    std::int64_t allNotesOffSweeps = 0;
};
} // namespace arpbox::testing
