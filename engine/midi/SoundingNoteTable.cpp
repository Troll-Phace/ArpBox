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
void SoundingNoteTable::emitNoteOff (juce::MidiBuffer& midi,
                                     const Entry& entry,
                                     int offset) noexcept
{
    // Raw bytes, never juce::MidiMessage — constructing one can heap-allocate.
    const juce::uint8 bytes[3] = {
        static_cast<juce::uint8> (noteOffStatus | ((entry.channel - 1) & 0x0F)),
        static_cast<juce::uint8> (entry.note & 0x7F),
        noteOffVelocity
    };
    midi.addEvent (bytes, 3, offset);
}

// RT-SAFE:
void SoundingNoteTable::removeAt (int index) noexcept
{
    if (index < 0 || index >= count)
        return;

    entries[static_cast<std::size_t> (index)] =
        entries[static_cast<std::size_t> (count - 1)];
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
void SoundingNoteTable::retireAt (int index, juce::MidiBuffer& midi, int offset) noexcept
{
    if (index < 0 || index >= count)
        return;

    emitNoteOff (midi, entries[static_cast<std::size_t> (index)], offset);
    removeAt (index);
}

// RT-SAFE:
void SoundingNoteTable::emitDueNoteOffs (juce::MidiBuffer& midi,
                                         std::int64_t blockStartSample,
                                         int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    const std::int64_t blockEndSample =
        blockStartSample + static_cast<std::int64_t> (numSamples);

    // Backwards walk: removeAt swaps the LAST entry into the vacated slot, so
    // iterating downwards visits every entry exactly once regardless of removals.
    for (int i = count - 1; i >= 0; --i)
    {
        const Entry& e = entries[static_cast<std::size_t> (i)];

        // Half-open window on the EXACT integer sample timeline. An off due exactly
        // at blockEndSample belongs to the next block at offset 0; because both
        // sides are int64 there is no rounding and therefore no way for a buffer-size
        // change to move the decision (the property §5.5 / Phase 5.3 require).
        if (e.dueOffSample >= blockEndSample)
            continue;

        // An off already in the past can only mean a discontinuity the caller did not
        // flush. Emit it immediately rather than dropping it — a late note-off is a
        // blemish, a missing one is a hung note.
        const std::int64_t delta = e.dueOffSample - blockStartSample;
        const auto offset = static_cast<int> (
            juce::jlimit<std::int64_t> (0, static_cast<std::int64_t> (numSamples) - 1, delta));

        emitNoteOff (midi, e, offset);
        removeAt (i);
    }
}

// RT-SAFE:
void SoundingNoteTable::flush (juce::MidiBuffer& midi, int offset) noexcept
{
    const int safeOffset = juce::jmax (0, offset);

    // 1. Explicit per-note offs — the real mechanism (§5.5). Accumulate the set of
    //    channels we sounded on while we walk.
    std::uint16_t sweepChannels = 0;

    for (int i = 0; i < count; ++i)
    {
        const Entry& e = entries[static_cast<std::size_t> (i)];
        emitNoteOff (midi, e, safeOffset);

        if (e.channel >= 1 && e.channel <= 16)
            sweepChannels = static_cast<std::uint16_t> (
                sweepChannels | static_cast<std::uint16_t> (1u << (e.channel - 1)));
    }

    count = 0;

    // 2. CC123 belt and braces, ONLY on channels we actually sounded on (see the
    //    class comment: an unconditional sweep would kill live THRU notes flowing
    //    through this node). An empty table therefore emits nothing at all, which is
    //    what makes a spurious/defensive flush harmless.
    for (int ch = 0; ch < 16; ++ch)
    {
        if ((sweepChannels & static_cast<std::uint16_t> (1u << ch)) == 0)
            continue;

        const juce::uint8 bytes[3] = {
            static_cast<juce::uint8> (controlChangeStatus | ch),
            allNotesOffController,
            0
        };
        midi.addEvent (bytes, 3, safeOffset);
    }
}

// RT-SAFE:
void SoundingNoteTable::reset () noexcept
{
    count = 0;
}
} // namespace arpbox::engine
