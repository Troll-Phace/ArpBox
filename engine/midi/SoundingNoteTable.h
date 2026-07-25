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

    ── THE PLACEMENT RULE (issues #36, #46, #48 — ONE FAILURE CLASS) ───────────
    NO NOTE-OFF IS EVER EMITTED LATER THAN ITS OWN SCHEDULED DUE SAMPLE. Every
    emission path below places its off at `min (entry.dueOffSample, whatever the
    caller asks for)`, converted to a block offset by the ONE private conversion
    (`offsetForSample`). A caller can only ever move an off EARLIER — cut a note
    short — and can never move one later.

    That rule is the whole of a bug family this class kept re-growing. Three
    separate defects, one shape each time: a note-off placed from a WITHIN-BLOCK
    offset (`offset - 1`, `adoptSample - 1`) instead of from its own absolute
    schedule. Because "which offset a musical sample lands on" is a property of the
    DEVICE BUFFER SIZE and not of the music, each one made the emitted MIDI
    buffer-size dependent — a §1.2 determinism violation:

      #36  same-pitch retrigger, entry ALREADY DUE     → routed to its own due sample
      #46  same-pitch retrigger, entry STILL SOUNDING  → scheduled at `min` on registration
      #48  pattern-switch flush                        → EVERY entry forced to `adoptSample - 1`

    The rule is enforced STRUCTURALLY rather than by three careful call sites:
    there is no public method that takes a raw within-block offset. `retireNoLaterThan`
    and `flush` both take ABSOLUTE samples plus the block they are being emitted
    into, and neither can express "place this off after its due sample". A fourth
    instance of the class is therefore not writable.

    ── THE ORDERING RULE (issue #51 — THE PLACEMENT RULE'S SIBLING) ────────────
    WHEN SEVERAL NOTE-OFFS LAND ON ONE SAMPLE, THEY ARE EMITTED IN REGISTRATION
    ORDER — oldest sounding note first. Storage order IS registration order, always,
    because `removeAt` is ORDER-PRESERVING; every emission path walks the array
    FORWARDS, so no path has to sort and no future path can forget to.

    Placement fixes WHICH SAMPLE an off lands on; this fixes WHICH ORDER offs that
    agree on a sample come out in. `juce::MidiBuffer` sorts on insertion by TIMESTAMP
    ONLY and is stable among equal timestamps, so storage order reaches the emitted
    stream verbatim — which is what made the old swap-with-last `removeAt` a
    determinism defect and not merely an implementation detail:

      storage order  ⟵ which entries were removed earlier
                     ⟵ which removals shared a block
                     ⟵ THE DEVICE BUFFER SIZE

    Observed (#51): 300 BPM / 48 kHz, 1/32 grid, LEN 150 %, stop at 61440 — the two
    notes the stop cuts short came out `64, 65` at nine swept block sizes and `65, 64`
    at 4096. Same events, same count, same CC123, different bytes. §1.2 says
    BYTE-IDENTICAL and `tests/golden/` compares emission order, so a golden covering a
    multi-note flush was passing on the luck of its bake carving.

    WHY REGISTRATION ORDER RATHER THAN A SORT ON `(sample, channel, note)`. Both are
    total and both are storage-independent, and on every case in the suite they agree
    (an ascending pool traversal registers ascending pitches). Registration order wins
    on three counts:

      - It removes the ROOT CAUSE instead of masking it. A sort at each emission point
        is per-call-site discipline, and this class has already lost that bet three
        times (#36/#46/#48 were three careful call sites that each got it wrong). The
        structural repair is one line in `removeAt`.
      - It is FREE at emission. A sort would run inside `emitDueNoteOffs`, which
        executes every block; the O(n) shift runs only on a removal.
      - It is CONSISTENT WITH THE NOTE-ONS. A note is registered as its note-on is
        emitted, so simultaneous ons and their eventual simultaneous offs come out in
        the same relative order. A `(sample, channel, note)` sort would order the offs
        of a chord differently from its ons.

    Registration order is carving-independent because the ADD SEQUENCE is: the step
    walk that emits note-ons is itself buffer-size independent (the property the rest
    of this header defends), and under order-preserving removal the surviving array is
    always the add-order subsequence of the live set — independent of when or in what
    order the removals happened.

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

    // RT-SAFE: audio thread. Pure query; no allocation.
    /** True when entry `index`'s scheduled note-off is due at or before absolute
        `sample` — i.e. the off has ALREADY come due and this table simply has not
        emitted it yet (the caller's step walk runs before `emitDueNoteOffs`).

        The caller needs this to distinguish the two retrigger cases in §5.5: an
        entry still sounding (retire one sample before the new note-on) from an
        entry whose off was already owed (retire at its TRUE due position — see
        `dueOffsetWithinBlock`). Returns false for an out-of-range `index`. */
    bool isDueAtOrBefore (int index, std::int64_t sample) const noexcept;

    // RT-SAFE: audio thread. Pure query; no allocation.
    /** Entry `index`'s due sample expressed as an offset within the block starting
        at absolute `blockStartSample` and running `numSamples` samples, clamped
        into `[0, numSamples)`.

        A thin, RANGE-CHECKED view of the single private conversion
        (`offsetForSample`) that every emission path in this class routes through.
        Two copies of that arithmetic would be free to drift, and a drift here is a
        buffer-size-dependent note-off position — a direct determinism-contract
        violation (§1.2). Do not reimplement it in a caller.

        This accessor exists for OBSERVATION (the anti-drift guard in the unit
        suite asserts that the offset a caller can see is the offset the emitters
        actually write). Callers do not need it to place a note-off: the emission
        methods take absolute samples and do the conversion themselves.

        @returns the clamped offset, or -1 if `index` is out of range or
                 `numSamples` is not positive (no valid offset exists). */
    int dueOffsetWithinBlock (int index, std::int64_t blockStartSample, int numSamples) const noexcept;

    // RT-SAFE: audio thread.
    /** Registers a sounding note whose note-off is due at `dueOffSample`
        (absolute samples). Does NOT emit the note-on — the caller does, and MUST
        NOT emit it if this returns false.

        @returns false if the table is full (the note-on must be suppressed). */
    bool add (int channel, int note, std::int64_t dueOffSample) noexcept;

    // RT-SAFE: audio thread.
    /** Emits the note-off for entry `index` at `min (its own due sample,
        capSample)` — converted into the block starting at `blockStartSample` and
        running `numSamples` samples — and removes the entry. `index` must come
        from `find()`; out-of-range indices are ignored.

        THE CAP CAN ONLY SHORTEN, NEVER EXTEND (see "THE PLACEMENT RULE" above).
        That is why this takes an absolute sample rather than an offset: the form it
        replaces, `retireAt (index, midi, jmax (0, offset - 1))`, silently became
        `offset` whenever the buffer size put the caller's event on a block head
        (#46) and dragged an already-owed off to wherever the retrigger happened to
        land (#36). Neither is expressible through this signature. */
    void retireNoLaterThan (int index,
                            juce::MidiBuffer& midi,
                            std::int64_t capSample,
                            std::int64_t blockStartSample,
                            int numSamples) noexcept;

    // RT-SAFE: audio thread.
    /** Emits note-offs for every note due before `blockStartSample + numSamples`,
        each at its exact offset within the block, and removes those entries.

        A note whose due sample already passed (possible only after a discontinuity
        the caller failed to flush) is emitted at offset 0 rather than dropped.

        The offset comes from the shared `offsetForSample` conversion, so an off
        emitted here and the same off emitted by any other path land identically. */
    void emitDueNoteOffs (juce::MidiBuffer& midi, std::int64_t blockStartSample, int numSamples) noexcept;

    // RT-SAFE: audio thread.
    /** Releases EVERY sounding note because the music this table belongs to is
        discontinued from absolute `releaseFromSample` onward (§5.5 flush point),
        and empties the table. `isEmpty()` is true on return.

        TWO KINDS OF ENTRY, AND THE DISTINCTION IS THE ISSUE #48 FIX:

          - AN ENTRY THAT HAD ALREADY ENDED. Its own due sample lies inside this
            block AND strictly before `releaseFromSample`, i.e. in the half-open
            window `[blockStartSample, releaseFromSample)`. Nothing cut this note
            short — it finished on its own schedule, and it is emitted AT ITS OWN
            DUE SAMPLE. It is not part of the CC123 sweep, because it is not a note
            the discontinuity had to kill.

            This window is exactly the span `emitDueNoteOffs` would have covered had
            it run before the flush, which is what makes the emitted stream identical
            at every buffer size. Before the fix, `flush` took a single offset and
            slammed EVERY entry onto it: at a buffer size where the flush point and
            the due sample shared a block the off came out LATE (and dragged a
            spurious CC123 with it), while a smaller buffer put the due sample in an
            earlier block and emitted it exactly. Same music, different MIDI, decided
            by the device buffer size (#48).

          - AN ENTRY THE DISCONTINUITY CUTS SHORT. Still sounding at
            `releaseFromSample`, or orphaned from a timeline a locate has broken (due
            sample before this block). Emitted at the last sample on which it may
            still sound, `releaseFromSample - 1`, clamped into the block — the same
            1-sample-gap discipline the same-pitch retrigger uses, so the outgoing
            note is released BEFORE anything the incoming material plays on that
            pitch. These are the entries the CC123 sweep covers.

        A BLOCK-HEAD FLUSH (`releaseFromSample == blockStartSample`, every transport
        discontinuity) therefore has an EMPTY "already ended" window and behaves
        exactly as the single-offset version did: everything at offset 0, swept.

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
    void flush (juce::MidiBuffer& midi,
                std::int64_t blockStartSample,
                int numSamples,
                std::int64_t releaseFromSample) noexcept;

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

    // RT-SAFE: THE ONE absolute-sample → block-offset conversion, and the choke
    // point every emission path in this class goes through. Exact integer
    // subtraction, clamped into [0, numSamples - 1] (0 when `numSamples <= 0`).
    //
    // The lower clamp catches a sample already in the past — emit immediately
    // rather than drop, because a late note-off is a blemish and a missing one is a
    // hung note. The upper clamp guards a sample the caller has not established
    // lies inside this block. Neither clamp is reachable on the common paths; both
    // exist so an out-of-range juce::MidiBuffer offset is unrepresentable.
    static int offsetForSample (std::int64_t absoluteSample, std::int64_t blockStartSample, int numSamples) noexcept;

    // RT-SAFE: emits a 3-byte note-off from raw bytes (never constructs a
    // juce::MidiMessage — that could heap-allocate on the audio thread).
    static void emitNoteOff (juce::MidiBuffer& midi, const Entry& entry, int offset) noexcept;

    // RT-SAFE: removes entry `index` by shifting every later entry down one slot.
    // ORDER-PRESERVING, and that is load-bearing rather than incidental: storage order
    // is emission order for every same-sample group (see "THE ORDERING RULE"). The
    // swap-with-last form this replaces was cheaper and wrong — it made the survivors'
    // order a function of the block carving (#51). A forward walk that removes must
    // NOT advance its index across a removal, since the successor shifts into the
    // vacated slot.
    void removeAt (int index) noexcept;

    std::array<Entry, static_cast<std::size_t> (capacity)> entries {};
    int count = 0;                    ///< Live entries in `entries[0 .. count-1]`.
    std::uint32_t droppedNoteOns = 0; ///< Overflow diagnostics (see class comment).

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundingNoteTable)
};
} // namespace arpbox::engine
