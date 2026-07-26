#include "SoundingNoteTable.h"

namespace arpbox::engine
{
namespace
{
    // MIDI status nibbles / controller numbers, spelled out so the raw-byte
    // construction below reads as MIDI rather than as magic numbers.
    constexpr juce::uint8 noteOffStatus = 0x80;
    constexpr juce::uint8 controlChangeStatus = 0xB0;
    constexpr juce::uint8 allNotesOffController = 123;

    // Release velocity on a generated note-off. 0 is the conventional value and the
    // one every synth treats as "plain release".
    constexpr juce::uint8 noteOffVelocity = 0;
} // namespace

// RT-SAFE:
std::int64_t SoundingNoteTable::placementSampleFor (const Entry& entry,
                                                    std::int64_t capSample,
                                                    std::int64_t blockStartSample,
                                                    int numSamples) noexcept
{
    // THE PLACEMENT RULE, in one place (see the header for the full argument).
    const std::int64_t blockEndSample = blockStartSample + static_cast<std::int64_t> (juce::jmax (0, numSamples));

    // THE FLOOR. An on sample outside this block is on a timeline this block cannot
    // compare against (a locate breaks it), so the block head stands in — which is a
    // no-op for every entry that started earlier on the SAME timeline.
    const bool onSampleIsComparable = entry.onSample >= blockStartSample && entry.onSample < blockEndSample;
    const std::int64_t floorSample = onSampleIsComparable ? entry.onSample : blockStartSample;

    // The cap can only SHORTEN (jmin against the entry's own schedule) and can never
    // reach below the floor (jmax).
    return juce::jmin (entry.dueOffSample, juce::jmax (floorSample, capSample));
}

// RT-SAFE:
int SoundingNoteTable::offsetForSample (std::int64_t absoluteSample,
                                        std::int64_t blockStartSample,
                                        int numSamples) noexcept
{
    // THE conversion (see the header): exact integer subtraction on the absolute
    // sample timeline, clamped into the block. Every emission path in this class
    // routes through here, so the arithmetic exists exactly once and two paths
    // cannot place the same sample on two different offsets.
    const std::int64_t upper = static_cast<std::int64_t> (juce::jmax (1, numSamples)) - 1;
    return static_cast<int> (juce::jlimit<std::int64_t> (0, upper, absoluteSample - blockStartSample));
}

// RT-SAFE:
void SoundingNoteTable::emitNoteOff (juce::MidiBuffer& midi, const Entry& entry, int offset) noexcept
{
    // Raw bytes, never juce::MidiMessage — constructing one can heap-allocate.
    const juce::uint8 bytes[3] = { static_cast<juce::uint8> (noteOffStatus | ((entry.channel - 1) & 0x0F)),
                                   static_cast<juce::uint8> (entry.note & 0x7F),
                                   noteOffVelocity };
    midi.addEvent (bytes, 3, offset);
}

// RT-SAFE:
void SoundingNoteTable::removeAt (int index) noexcept
{
    if (index < 0 || index >= count)
        return;

    // ORDER-PRESERVING (see "THE ORDERING RULE" in the header, issue #51). The
    // predecessor — `entries[index] = entries[count - 1]` — vacated the slot by
    // swapping the LAST entry into it, which made the storage order of the SURVIVORS
    // a function of WHICH ENTRIES WERE REMOVED EARLIER, i.e. of how the render was
    // carved into blocks. Shifting the tail down instead makes storage order equal
    // REGISTRATION ORDER unconditionally, which is a property of the music alone.
    //
    // Cost: one contiguous move of at most `capacity - 1` trivially-copyable 24-byte
    // entries (< 6 KB; see the capacity discussion, which since Phase 7.2 puts the
    // realistic live count in the low tens rather than at 1 per pitch). No allocation,
    // no branching per element, and removals are rare relative to blocks. The
    // alternative — sorting at every emission point — leaves the root cause in place
    // for the next emission path to rediscover.
    //
    // THE FIGURES ARE 24 / 6 KB, NOT 16 / 4 KB: `Entry` grew an `onSample` field in
    // Phase 7.2 (THE PLACEMENT RULE's floor), taking it from 16 to 24 bytes and the
    // whole table from 4104 to 6152 bytes. The old numbers survived here after the
    // header's own `Entry` doc had been corrected, which is the shape issue #80 is
    // about: a retracted figure left behind in the one place a future reader would
    // consult before touching this loop.
    //
    // The `+ 1` is done in the WIDE type (`std::size_t (i) + 1`), never in `int`
    // then widened: widening after the add is the shape that can overflow before the
    // cast ever runs. It cannot here — the loop condition bounds `i + 1` by `count`,
    // itself bounded by `capacity` — but the narrow-add-then-widen shape is the one
    // that bites when a bound later moves, so the repo fixes it on sight (same
    // treatment SpscFifo's drain index got in Phase 2).
    for (int i = index; i + 1 < count; ++i)
        entries[static_cast<std::size_t> (i)] = entries[static_cast<std::size_t> (i) + 1];

    --count;
}

// RT-SAFE:
int SoundingNoteTable::find (int channel, int note) const noexcept
{
    const auto ch = static_cast<std::uint8_t> (channel);
    const auto n = static_cast<std::uint8_t> (note);

    for (int i = 0; i < count; ++i)
    {
        const Entry& e = entries[static_cast<std::size_t> (i)];
        if (e.channel == ch && e.note == n)
            return i;
    }

    return -1;
}

// RT-SAFE:
int SoundingNoteTable::findStartedAtOrBefore (int channel, int note, std::int64_t sample) const noexcept
{
    const auto ch = static_cast<std::uint8_t> (channel);
    const auto n = static_cast<std::uint8_t> (note);

    int best = -1;
    std::int64_t bestOnSample = 0;

    // FULL scan, not an early exit on the first match: storage order is REGISTRATION
    // order (THE ORDERING RULE) and registration order is the caller's emission
    // order, which since Phase 7.2 is index order rather than sample order. So the
    // first qualifying entry is not necessarily the latest-starting one, and the
    // latest-starting one is the note this pitch is actually taking over from. Same
    // O(size()) cost as `find`; no allocation.
    for (int i = 0; i < count; ++i)
    {
        const Entry& e = entries[static_cast<std::size_t> (i)];

        if (e.channel != ch || e.note != n || e.onSample > sample)
            continue;

        if (best < 0 || e.onSample >= bestOnSample)
        {
            best = i;
            bestOnSample = e.onSample;
        }
    }

    return best;
}

// RT-SAFE:
bool SoundingNoteTable::isDueAtOrBefore (int index, std::int64_t sample) const noexcept
{
    if (index < 0 || index >= count)
        return false;

    return entries[static_cast<std::size_t> (index)].dueOffSample <= sample;
}

// RT-SAFE:
int SoundingNoteTable::dueOffsetWithinBlock (int index, std::int64_t blockStartSample, int numSamples) const noexcept
{
    if (index < 0 || index >= count || numSamples <= 0)
        return -1;

    // A range-checked view of the shared placement + conversion — see the header on
    // why this is an observation accessor rather than the way a caller places a
    // note-off. Routed through `placementSampleFor` with the entry's OWN due sample as
    // the cap, exactly as `emitDueNoteOffs` does, so the anti-drift guard in the unit
    // suite compares like with like.
    const Entry& entry = entries[static_cast<std::size_t> (index)];

    return offsetForSample (placementSampleFor (entry, entry.dueOffSample, blockStartSample, numSamples),
                            blockStartSample,
                            numSamples);
}

// RT-SAFE:
bool SoundingNoteTable::add (int channel, int note, std::int64_t onSample, std::int64_t dueOffSample) noexcept
{
    if (count >= capacity)
    {
        // OVERFLOW POLICY (see the class comment): refuse the note-ON. Never drop a
        // note-OFF — that is the only failure that outlives the block.
        ++droppedNoteOns;
        return false;
    }

    Entry& e = entries[static_cast<std::size_t> (count)];
    e.dueOffSample = dueOffSample;
    e.onSample = onSample;
    e.channel = static_cast<std::uint8_t> (juce::jlimit (1, 16, channel));
    e.note = static_cast<std::uint8_t> (juce::jlimit (0, 127, note));
    ++count;

    return true;
}

// RT-SAFE:
void SoundingNoteTable::retireNoLaterThan (int index,
                                           juce::MidiBuffer& midi,
                                           std::int64_t capSample,
                                           std::int64_t blockStartSample,
                                           int numSamples) noexcept
{
    if (index < 0 || index >= count)
        return;

    const Entry& entry = entries[static_cast<std::size_t> (index)];

    // THE PLACEMENT RULE, through its ONE implementation: the cap can only pull the
    // off EARLIER, and never earlier than the note's own on. An entry whose off was
    // already owed keeps its own scheduled sample no matter what the caller asks for,
    // which is what makes the placement independent of which block happens to contain
    // the caller's event; the FLOOR is the Phase 7.2 half, and it BINDS — a step
    // displaced by swing or MICRO can be emitted after a same-pitch note that starts
    // LATER than it (index order stopped being sample order), and without it that
    // entry's off would land before its own note-on and hang the note at the synth.
    const std::int64_t at = placementSampleFor (entry, capSample, blockStartSample, numSamples);

    emitNoteOff (midi, entry, offsetForSample (at, blockStartSample, numSamples));
    removeAt (index);
}

// RT-SAFE:
void SoundingNoteTable::emitDueNoteOffs (juce::MidiBuffer& midi, std::int64_t blockStartSample, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    const std::int64_t blockEndSample = blockStartSample + static_cast<std::int64_t> (numSamples);

    // FORWARD walk, emitting in REGISTRATION ORDER ("THE ORDERING RULE", issue #51).
    // Two notes coming due on the same sample in one block are emitted oldest-first,
    // at every buffer size, because `removeAt` no longer reshuffles the survivors.
    //
    // The predecessor walked BACKWARDS, which was correct only under the old
    // swap-with-last removal (downwards is the direction that visits every entry
    // exactly once when a removal drags the tail into the current slot). Under
    // order-preserving removal the survivors shift DOWN into `i`, so the index is
    // simply not advanced on a removal — and the direction becomes free to be the one
    // the ordering rule needs.
    for (int i = 0; i < count; /* advanced below */)
    {
        const Entry& e = entries[static_cast<std::size_t> (i)];

        // Half-open window on the EXACT integer sample timeline. An off due exactly
        // at blockEndSample belongs to the next block at offset 0; because both
        // sides are int64 there is no rounding and therefore no way for a buffer-size
        // change to move the decision (the property §5.5 / Phase 5.3 require).
        if (e.dueOffSample >= blockEndSample)
        {
            ++i; // survivor: it keeps this slot, so move past it
            continue;
        }

        // Placement via the SHARED conversion, not a local copy of it: the same call
        // every other emission path in this class makes, so an off emitted here and
        // an off emitted there for the same due sample land on the same offset,
        // always.
        emitNoteOff (midi,
                     e,
                     offsetForSample (placementSampleFor (e, e.dueOffSample, blockStartSample, numSamples),
                                      blockStartSample,
                                      numSamples));
        removeAt (i); // shifts the next entry INTO `i` — deliberately not advanced
    }
}

// RT-SAFE:
void SoundingNoteTable::flush (juce::MidiBuffer& midi,
                               std::int64_t blockStartSample,
                               int numSamples,
                               std::int64_t releaseFromSample) noexcept
{
    // The last sample on which a note cut short by this discontinuity may still
    // sound. `releaseFromSample - 1` is decided on the ABSOLUTE timeline and
    // converted once, so it is the same musical sample at every buffer size; the
    // conversion's lower clamp is what turns a block-head flush into offset 0.
    const int cutOffset = offsetForSample (releaseFromSample - 1, blockStartSample, numSamples);

    // 1. Explicit per-note offs — the real mechanism (§5.5). Accumulate the set of
    //    channels we actually CUT SHORT while we walk.
    //
    //    FORWARD WALK, and no `removeAt`: entries that share a timestamp are inserted
    //    in table order, and `count = 0` below retires the lot in one go. Table order
    //    IS registration order ("THE ORDERING RULE", issue #51), so a flush cutting
    //    several notes emits their offs oldest-first at every buffer size. Before
    //    that rule, the survivors' storage order depended on which entries earlier
    //    blocks happened to remove, and this walk handed `juce::MidiBuffer` — which
    //    is STABLE among equal timestamps — a buffer-size-dependent sequence.
    std::uint16_t sweepChannels = 0;

    for (int i = 0; i < count; ++i)
    {
        const Entry& e = entries[static_cast<std::size_t> (i)];

        // ── THE ISSUE #48 SPLIT (see the header) ─────────────────────────────
        // An entry whose own off falls inside this block and strictly before the
        // release point ALREADY ENDED — `emitDueNoteOffs` would have emitted it
        // there had it run first. Give it its true sample, and leave it out of the
        // sweep: it is not a note this discontinuity had to kill. Everything else —
        // still sounding, or orphaned by a locate that broke the timeline — is cut
        // short at `releaseFromSample - 1` and IS swept.
        const bool alreadyEnded = e.dueOffSample >= blockStartSample && e.dueOffSample < releaseFromSample;

        if (alreadyEnded)
        {
            emitNoteOff (midi,
                         e,
                         offsetForSample (placementSampleFor (e, e.dueOffSample, blockStartSample, numSamples),
                                          blockStartSample,
                                          numSamples));
            continue;
        }

        // CUT SHORT — but never before its own note-on, which is the Phase 7.2 half
        // of THE PLACEMENT RULE and binds here for the same reason it binds in
        // `retireNoLaterThan`: a positively displaced step (swing, MICRO) or a late
        // ratchet child can be registered with an on sample AFTER the point this
        // discontinuity releases from, and forcing it onto `releaseFromSample - 1`
        // would emit its off before its on.
        //
        // THROUGH `placementSampleFor`, NOT A LOCAL `jmax`, and that is not tidiness:
        // this branch also handles entries ORPHANED BY A LOCATE, whose on samples are
        // on a timeline `blockStartSample` no longer belongs to. Only that function
        // knows to disregard an incomparable on sample; a local `jmax (e.onSample,
        // releaseFromSample - 1)` here moved the terminating stop-flush off of
        // `sequencer_retrigger`'s tied sweep from 122880 to 123007 and reddened a
        // golden. See the header note on the function.
        //
        // `cutOffset` is still used verbatim for the CC123 sweep below, which
        // describes the DISCONTINUITY rather than any one note.
        emitNoteOff (midi,
                     e,
                     offsetForSample (placementSampleFor (e, releaseFromSample - 1, blockStartSample, numSamples),
                                      blockStartSample,
                                      numSamples));

        if (e.channel >= 1 && e.channel <= 16)
            sweepChannels =
                static_cast<std::uint16_t> (sweepChannels | static_cast<std::uint16_t> (1u << (e.channel - 1)));
    }

    count = 0;

    // 2. CC123 belt and braces, ONLY on channels we actually sounded on (see the
    //    class comment: an unconditional sweep would kill live THRU notes flowing
    //    through this node). An empty table therefore emits nothing at all, which is
    //    what makes a spurious/defensive flush harmless — and so does a table
    //    holding only notes that had already ended, which is what keeps a
    //    mid-block pattern-switch flush byte-identical to the smaller buffer size
    //    that emitted those offs from an earlier block.
    for (int ch = 0; ch < 16; ++ch)
    {
        if ((sweepChannels & static_cast<std::uint16_t> (1u << ch)) == 0)
            continue;

        const juce::uint8 bytes[3] = { static_cast<juce::uint8> (controlChangeStatus | ch), allNotesOffController, 0 };
        midi.addEvent (bytes, 3, cutOffset);
    }
}

// RT-SAFE:
void SoundingNoteTable::reset () noexcept
{
    count = 0;
}
} // namespace arpbox::engine
