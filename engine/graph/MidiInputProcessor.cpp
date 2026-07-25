#include "MidiInputProcessor.h"

namespace arpbox::engine
{
namespace
{
    // Pre-sized capacity (bytes) for the hardware-MIDI scratch buffer. Generous for
    // a single block's worth of channel-voice + controller traffic; keeps
    // removeNextBlockOfMessages from ever growing the buffer on the audio thread.
    constexpr int hardwareScratchBytes = 4096;

    // Reserved capacity (bytes) for the graph-owned OUTGOING MidiBuffer, ensured
    // unconditionally every block (ensureSize is a no-op once satisfied). Covers the
    // deterministic 16-event CC123 flush (16 * 3-byte messages) plus a healthy block
    // of note/controller traffic, so no addEvent on the hot path grows it in steady
    // state. An extreme hardware burst beyond this may still grow the buffer once —
    // bounded by the device, not by us — but the flush never does.
    constexpr int outgoingWarmupBytes = 1024;

    // ── Raw-byte MIDI classification (no juce::MidiMessage construction) ──────
    // The hot path inspects the collector's raw bytes directly so nothing on the
    // audio thread can heap-allocate a MidiMessage for a long/sysex packet.

    // Channel (1..16) of a channel-voice message, or 0 for a system message.
    int rawChannel (const juce::uint8* d, int n) noexcept
    {
        if (n < 1)
            return 0;
        const int status = d[0] & 0xF0;
        return (status >= 0x80 && status <= 0xE0) ? (d[0] & 0x0F) + 1 : 0;
    }

    // A note-on with non-zero velocity.
    bool rawIsNoteOn (const juce::uint8* d, int n) noexcept
    {
        return n >= 3 && (d[0] & 0xF0) == 0x90 && d[2] > 0;
    }

    // A note-off, or a note-on with velocity 0 (the MIDI running-status "off").
    bool rawIsNoteOff (const juce::uint8* d, int n) noexcept
    {
        if (n < 3)
            return false;
        const int status = d[0] & 0xF0;
        return status == 0x80 || (status == 0x90 && d[2] == 0);
    }
} // namespace

MidiInputProcessor::MidiInputProcessor ()
    : juce::AudioProcessor (BusesProperties ()) // MIDI-only: no audio buses
{
}

// MESSAGE-THREAD ONLY:
void MidiInputProcessor::setSharedState (NoteEventQueue* notes,
                                         juce::MidiMessageCollector* collectorIn,
                                         MidiInputControl* controlIn) noexcept
{
    noteQueue = notes;
    collector = collectorIn;
    control = controlIn;
}

// MESSAGE-THREAD ONLY:
void MidiInputProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;

    // This node owns the collector's sample-rate configuration: it is the audio
    // node whose prepareToPlay the graph drives with the real device SR. Resetting
    // here (message thread, audio stopped) is the single, race-free reset point —
    // the app deliberately does NOT reset the shared collector concurrently.
    if (collector != nullptr)
        collector->reset (currentSampleRate);

    hardwareScratch.ensureSize (hardwareScratchBytes);
    hardwareScratch.clear ();
}

// RT-SAFE:
bool MidiInputProcessor::channelPasses (std::uint16_t mask, int channel) noexcept
{
    if (channel < 1 || channel > 16)
        return true; // system message / out of range → always pass
    return (mask & static_cast<std::uint16_t> (1u << (channel - 1))) != 0;
}

// RT-SAFE:
void MidiInputProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // MIDI-only node: no audio to render. Clear the (zero-channel) buffer defensively.
    buffer.clear ();

    // Capacity warm-up (W2): reserve enough outgoing-buffer capacity for the
    // deterministic 16-event CC123 flush plus ordinary note traffic, so no later
    // addEvent grows the graph-owned buffer on the audio thread. This is
    // UNCONDITIONAL on purpose: ensureSize early-returns (no allocation) once
    // capacity suffices, so steady state costs nothing — but the graph can hand us a
    // FRESH, small pool buffer after a render-sequence rebuild (e.g. a synth swap),
    // and re-checking every block re-warms that new buffer, closing the
    // post-rebuild gap a first-block-only flag would miss.
    midi.ensureSize (outgoingWarmupBytes);

    const int numSamples = buffer.getNumSamples ();
    const std::uint16_t mask =
        control != nullptr ? control->channelMask.load (std::memory_order_relaxed)
                           : static_cast<std::uint16_t> (0xFFFF);

    // 1. One-shot all-notes-off flush (message-thread requested). Emit CC123 on
    //    every channel and zero the voice count — a clean flush primitive for
    //    synth swaps/removals (ARCHITECTURE §5.5). The 16 addEvent calls do not
    //    allocate: the outgoing buffer was capacity-warmed above (W2).
    if (control != nullptr
        && control->allNotesOffRequested.exchange (false, std::memory_order_acq_rel))
    {
        for (int ch = 0; ch < 16; ++ch)
        {
            const juce::uint8 bytes[3] = { static_cast<juce::uint8> (0xB0 | ch), 123, 0 };
            midi.addEvent (bytes, 3, 0);
        }
        liveVoices = 0;
    }

    // 2. QWERTY/pad events. Drain is FIFO, so producer order is preserved; add at
    //    offset 0. Built from raw bytes → no MidiMessage allocation.
    if (noteQueue != nullptr)
    {
        noteQueue->drain (
            [this, &midi, mask] (const NoteEvent& e) noexcept
            {
                const bool on = e.kind == NoteEventKind::noteOn;

                // §5.5 MIDI invariant: the channel mask gates ONLY note-ONs. A
                // note-OFF must ALWAYS pass — if the mask changed while a note (let
                // through under the old mask) was held, swallowing its off would
                // hang the note on the synth and permanently skew the voice count.
                if (on && ! channelPasses (mask, e.channel))
                    return;

                const juce::uint8 status = static_cast<juce::uint8> (
                    (on ? 0x90 : 0x80) | ((e.channel - 1) & 0x0F));
                const juce::uint8 bytes[3] = { status,
                                               static_cast<juce::uint8> (e.note & 0x7F),
                                               static_cast<juce::uint8> (e.velocity & 0x7F) };
                midi.addEvent (bytes, 3, 0);

                if (on)
                    ++liveVoices;
                else if (liveVoices > 0)
                    --liveVoices;
            });
    }

    // 3. Hardware MIDI from the shared collector.
    if (collector != nullptr && numSamples > 0)
    {
        hardwareScratch.clear ();

        // RT-SAFE (documented exception): removeNextBlockOfMessages briefly takes
        // the collector's internal juce::CriticalSection to splice the queued
        // messages the MIDI thread posted. This bounded (~µs) vendored lock is the
        // ONLY accepted lock on the audio thread in ARPBOX (user-approved this
        // session) — it is the canonical JUCE hardware-MIDI hand-off and cannot be
        // replaced without reimplementing the collector.
        collector->removeNextBlockOfMessages (hardwareScratch, numSamples);

        for (const auto meta : hardwareScratch)
        {
            const juce::uint8* const d = meta.data;
            const int n = meta.numBytes;

            const bool noteOn = rawIsNoteOn (d, n);

            // §5.5 MIDI invariant: the channel mask gates ONLY note-ONs. Note-offs,
            // CC123/all-notes-off, sustain-off — anything that RELEASES a note —
            // must always pass, or a mask change mid-note hangs the synth. Only a
            // fresh note-on is filtered.
            if (noteOn)
            {
                const int chan = rawChannel (d, n);
                if (chan > 0 && ! channelPasses (mask, chan))
                    continue; // masked note-on: drop and do not count
            }

            midi.addEvent (d, n, meta.samplePosition);

            if (noteOn)
                ++liveVoices;
            else if (rawIsNoteOff (d, n) && liveVoices > 0)
                --liveVoices;
        }
    }

    // 4. Publish the live MIDI-in voice count for the master to snapshot.
    if (control != nullptr)
        control->voiceCount.store (
            static_cast<std::uint16_t> (juce::jlimit (0, 0xFFFF, liveVoices)),
            std::memory_order_relaxed);
}

// RT-SAFE:
void MidiInputProcessor::processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer&)
{
    // The root graph runs in single precision; this must never be invoked.
    jassertfalse; // graph is single precision
    buffer.clear ();
}

bool MidiInputProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // MIDI-only: accept only the empty (no main audio in/out) layout.
    return layouts.getMainInputChannelSet ().isDisabled ()
        && layouts.getMainOutputChannelSet ().isDisabled ();
}
} // namespace arpbox::engine
