#pragma once

#include "../EngineGuiGuard.h"
#include "SpscFifo.h"

#include <cstdint>
#include <type_traits>

namespace arpbox::engine
{
/** Whether a `NoteEvent` turns a note on or off. Stored as a `std::uint8_t` so the
    enclosing `NoteEvent` stays a compact trivially-copyable POD. A velocity-0
    note-on from the UI is represented explicitly as `noteOff` by the producer, so
    the audio-thread consumer never has to re-derive on/off from velocity. */
enum class NoteEventKind : std::uint8_t
{
    noteOff = 0, ///< Release the note.
    noteOn = 1   ///< Strike the note (velocity is the strike velocity, 1..127).
};

/** A single POD note event posted by the MESSAGE thread (on-screen keyboard /
    QWERTY / pads) and consumed by the `MidiInputProcessor` graph node on the AUDIO
    thread (ARCHITECTURE §3.3, the "QWERTY/pads (UI thread via FIFO)" input).

    This is the dedicated QWERTY/pad channel — it is deliberately SEPARATE from the
    `EngineCommandQueue`: that queue is strict SPSC with `MasterProcessor` as its
    sole consumer, so a second consumer (the MIDI-In node) may NOT drain it.
    Note-generating input therefore rides its own `NoteEventQueue` instance whose
    single consumer is the MIDI-In node.

    Trivially copyable by construction (an enum tag + three 7-bit MIDI values), so
    it copies byte-for-byte through `SpscFifo` with no ctor/dtor across the
    boundary. `channel` is 1-based MIDI (1..16) to match `juce::MidiMessage`. */
struct NoteEvent
{
    NoteEventKind kind = NoteEventKind::noteOff; ///< On or off.
    std::uint8_t note = 0;                       ///< MIDI note number 0..127.
    std::uint8_t velocity = 0;                   ///< Strike velocity 1..127 (0 for note-off).
    std::uint8_t channel = 1;                    ///< MIDI channel 1..16.
};

static_assert (std::is_trivially_copyable_v<NoteEvent>,
               "NoteEvent must be trivially copyable to travel through SpscFifo.");

/** QWERTY/pad note channel (ARCHITECTURE §3.3). STRICT SPSC: the single producer
    is the MESSAGE thread (DebugPanel keyboard today; the real pad/QWERTY UI in
    Phase 17); the single consumer is the `MidiInputProcessor` graph node on the
    AUDIO thread, which drains it at the top of its block. 256 slots absorb a burst
    of chords/roll input between blocks without dropping under normal use; drops are
    counted (`getDroppedCount`) rather than blocking the UI. */
using NoteEventQueue = SpscFifo<NoteEvent, 256>;
} // namespace arpbox::engine
