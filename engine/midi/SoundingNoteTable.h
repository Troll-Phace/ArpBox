#pragma once

#include "../EngineGuiGuard.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cstdint>

namespace arpbox::engine
{
/** The engine's authority on every note it has sounded (ARCHITECTURE §5.5: "Engine
    owns every note-off; sounding-note table tracks all live notes"). v1 — Phase 8
    extends it (pool-change flush, panic path, THRU/SELF voice tracking); this
    version nails the INVARIANTS the later features are built on.

    ── THE INVARIANT ───────────────────────────────────────────────────────────
    Every note-on the engine emits is registered here with the absolute sample at
    which its note-off is due, and the note-off is emitted by this table — either
    when it comes due, or by `flush()` at a discontinuity. There is no other exit
    path, so there is no way to leak a sounding note. After every flush point
    `isEmpty()` must be true (asserted by the MIDI-conformance suite).

    ── SCHEDULING UNIT: ABSOLUTE SAMPLES, NOT PPQ ──────────────────────────────
    A note-off is scheduled at an absolute sample on the transport's `std::int64_t`
    timeline. The alternative (absolute PPQ) keeps note lengths musically
    proportional through a tempo change, but pays for it in exactly the currency
    §5.5 cares about:

      - The due test becomes exact integer arithmetic:
        `dueOffSample < blockStartSample + numSamples`, and the emitted offset is
        `int (dueOffSample - blockStartSample)`. No float comparison sits between a
        note-on and its note-off, and the chosen offset is IDENTICAL at every buffer
        size — the same property the transport gives PPQ, obtained here for free.
      - A tempo jump cannot move a scheduled off. With PPQ scheduling, a jump from
        300 → 20 BPM stretches a pending note by 15x (audibly a hung note), and a
        jump the other way can put the off in the PAST, which then either fires late
        or, worse, is skipped by an ordering assumption.

    The cost is that a note already sounding keeps its PHYSICAL length across a
    tempo change instead of its musical one. That is the right trade for the MVP:
    Phase 6's LEN lane is a percentage OF THE STEP, resolved to samples at note-on
    time from the then-current tempo, so musical proportionality is correct for
    every note the user hears start after the change.

    NOTE that the sample timeline is NOT monotonic across a locate (the transport
    re-derives its sample position from the target PPQ). That is precisely why a
    position jump is a flush point in the caller — see `SequencerProcessor`.

    ── CAPACITY & OVERFLOW ─────────────────────────────────────────────────────
    Fixed `capacity` entries, allocated inside the object (a `std::array`); this
    class never allocates, at any point in its life. The bound covers 16 MIDI
    channels x 16 simultaneous notes. Realistic worst case for the MVP is far
    lower: one channel, a note pool bounded by the held chord, up to 8 ratchet
    children per step (§12.1) and LEN up to 400% allowing ~4 steps of overlap —
    order 64 notes. `capacity` leaves ~4x headroom.

    On overflow `add()` returns FALSE and the caller must NOT emit the note-on. The
    asymmetry is deliberate and load-bearing: dropping a note-ON costs one missing
    note, dropping a note-OFF costs a hung note for the rest of the session. Drops
    are counted (`droppedNoteOnCount()`) so a test or the X-RAY view can see them.

    ── THREADING ───────────────────────────────────────────────────────────────
    AUDIO-THREAD-OWNED. Every method below is called from `processBlock` and is
    allocation-free, lock-free, logging-free. The only message-thread access is the
    const observation accessors, used by headless tests (see
    `SequencerProcessor::soundingNotes`). */
class SoundingNoteTable
{
public:
    /** Maximum simultaneously sounding notes (16 channels x 16 notes). See the
        capacity discussion in the class comment. */
    static constexpr int capacity = 256;

    /** Constructs an empty table. */
    SoundingNoteTable () = default;

    // ── Observation (audio thread; also message thread for headless tests) ────

    /** True when no note is sounding. MUST hold after every flush point (§5.5). */
    bool isEmpty () const noexcept { return count == 0; }

    /** Number of currently sounding notes. */
    int size () const noexcept { return count; }

    /** Note-ons refused because the table was full (see the overflow policy). */
    std::uint32_t droppedNoteOnCount () const noexcept { return droppedNoteOns; }

    // ── Mutation (RT-SAFE: audio thread only) ────────────────────────────────

    // RT-SAFE: audio thread. Linear scan over `size()` entries; no allocation.
    /** Index of the sounding entry for (`channel`, `note`), or -1 if that pitch is
        not currently sounding. Used to implement the same-pitch retrigger policy. */
    int find (int channel, int note) const noexcept;

    // RT-SAFE: audio thread.
    /** Registers a sounding note whose note-off is due at `dueOffSample`
        (absolute samples). Does NOT emit the note-on — the caller does, and MUST
        NOT emit it if this returns false.

        @returns false if the table is full (the note-on must be suppressed). */
    bool add (int channel, int note, std::int64_t dueOffSample) noexcept;

    // RT-SAFE: audio thread.
    /** Emits the note-off for entry `index` at `offset` and removes the entry.
        `index` must come from `find()`. Out-of-range indices are ignored. */
    void retireAt (int index, juce::MidiBuffer& midi, int offset) noexcept;

    // RT-SAFE: audio thread.
    /** Emits note-offs for every note due before `blockStartSample + numSamples`,
        each at its exact offset within the block, and removes those entries.

        A note whose due sample already passed (possible only after a discontinuity
        the caller failed to flush) is emitted at offset 0 rather than dropped. */
    void emitDueNoteOffs (juce::MidiBuffer& midi, std::int64_t blockStartSample, int numSamples) noexcept;

    // RT-SAFE: audio thread.
    /** Emits note-offs for ALL sounding notes at `offset`, then a CC123
        (all-notes-off) on every channel that had a note sounding, and empties the
        table. `isEmpty()` is true on return.

        WHY EXPLICIT PER-NOTE OFFS: §5.5 requires them — hosted plugins honour
        CC123 inconsistently, so the per-note offs are the real mechanism and CC123
        is belt and braces for a synth that lost one.

        WHY THE SWEEP IS RESTRICTED TO CHANNELS WE SOUNDED ON: this table lives in a
        node that sits BETWEEN the MIDI-in node and the synth, so live THRU notes
        (QWERTY, hardware) flow through the same buffer. An unconditional 16-channel
        sweep would kill notes the user is physically holding — a real, audible bug.
        Restricting the sweep to channels this table actually sounded on covers every
        channel we could have left a note on and touches nothing else. (An
        unconditional sweep still exists where it belongs: `EngineGraph::allNotesOff`
        on the MIDI-in path, for plugin swaps.)

        WHY NO CC64 (sustain-off) DESPITE §5.5 SAYING "sustain-off": this node holds
        no sustain state in Phase 5 — the sustain latch is the Phase-8 note pool's,
        and the pool releases its own latch internally rather than squirting CC64
        downstream. Emitting CC64 0 here would instead cancel a pedal the user is
        physically holding, since that pedal message passes through this node on the
        THRU path. Phase 8 owns the sustain half of the flush. */
    void flush (juce::MidiBuffer& midi, int offset) noexcept;

    // RT-SAFE: audio thread. Forgets every entry WITHOUT emitting anything.
    /** Discards all tracked notes silently. For teardown paths only (release), never
        for a musical discontinuity — those must go through `flush()` so the synth
        actually stops sounding. */
    void reset () noexcept;

private:
    /** One sounding note. 16 bytes; `capacity` of them is 4 KB, held by value. */
    struct Entry
    {
        std::int64_t dueOffSample = 0; ///< Absolute sample at which the note-off is due.
        std::uint8_t channel = 0;      ///< MIDI channel, 1..16.
        std::uint8_t note = 0;         ///< MIDI note number, 0..127.
    };

    // RT-SAFE: emits a 3-byte note-off from raw bytes (never constructs a
    // juce::MidiMessage — that could heap-allocate on the audio thread).
    static void emitNoteOff (juce::MidiBuffer& midi, const Entry& entry, int offset) noexcept;

    // RT-SAFE: removes entry `index` by swapping the last entry into its slot. Order
    // is not preserved — juce::MidiBuffer sorts on insertion, so emission order does
    // not depend on storage order.
    void removeAt (int index) noexcept;

    std::array<Entry, static_cast<std::size_t> (capacity)> entries {};
    int count = 0;                    ///< Live entries in `entries[0 .. count-1]`.
    std::uint32_t droppedNoteOns = 0; ///< Overflow diagnostics (see class comment).

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundingNoteTable)
};
} // namespace arpbox::engine
