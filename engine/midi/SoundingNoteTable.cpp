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
    // Cost: one contiguous move of at most `capacity - 1` trivially-copyable 16-byte
    // entries (< 4 KB, realistically ~1 KB — see the capacity discussion). No
    // allocation, no branching per element, and removals are rare relative to blocks.
    // The alternative — sorting at every emission point — leaves the root cause in
    // place for the next emission path to rediscover.
    for (int i = index; i + 1 < count; ++i)
        entries[static_cast<std::size_t> (i)] = entries[static_cast<std::size_t> (i + 1)];

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

    // A range-checked view of the shared conversion — see the header on why this is
    // an observation accessor rather than the way a caller places a note-off.
    return offsetForSample (entries[static_cast<std::size_t> (index)].dueOffSample, blockStartSample, numSamples);
}

// RT-SAFE:
bool SoundingNoteTable::add (int channel, int note, std::int64_t dueOffSample) noexcept
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

    // THE PLACEMENT RULE (header): the cap can only pull the off EARLIER. An entry
    // whose off was already owed keeps its own scheduled sample no matter what the
    // caller asks for, which is what makes the placement independent of which block
    // happens to contain the caller's event.
    const std::int64_t at = juce::jmin (entry.dueOffSample, capSample);

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
        emitNoteOff (midi, e, offsetForSample (e.dueOffSample, blockStartSample, numSamples));
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
            emitNoteOff (midi, e, offsetForSample (e.dueOffSample, blockStartSample, numSamples));
            continue;
        }

        emitNoteOff (midi, e, cutOffset);

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
