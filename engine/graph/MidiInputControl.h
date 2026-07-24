#pragma once

#include "../EngineGuiGuard.h"

#include <atomic>
#include <cstdint>

namespace arpbox::engine
{
/** Lock-free control/status block shared across audio-graph nodes for the MIDI-In
    path (mirrors the `ToneControl` cross-node pattern — see ToneControl.h).

    Three independent atomics, three directions of travel:

      - `channelMask` (MESSAGE thread → audio): a 16-bit channel filter, bit `i`
        set ⇒ MIDI channel `i + 1` passes. Default `0xFFFF` (all channels pass).
        Read by the `MidiInputProcessor` at the top of each block.

      - `voiceCount` (audio → audio): the `MidiInputProcessor` publishes its live
        MIDI-in note count here; the `MasterProcessor` (which runs LATER in the
        block) reads it and copies it into `EngineSnapshot.voiceCount`. The two
        nodes run on the same audio thread but in a fixed order, so — exactly like
        the tone-control shim — the value crosses between them via an atomic rather
        than a return value. This is an INTERIM voice source for Phase 4 (raw
        MIDI-in note tracking); Phase 8's sounding-note table becomes the
        authoritative voice count.

      - `allNotesOffRequested` (MESSAGE thread → audio): a one-shot flush request.
        The message thread sets it; the `MidiInputProcessor` clears it (exchange)
        and emits CC123 (all-notes-off) on every channel, then zeroes its voice
        count. Gives the plugin-slot coordinator a clean flush primitive around
        synth swaps/removals (ARCHITECTURE §5.5 flush points; full note-lifecycle
        ownership arrives with the sequencer in Phase 8).

    RT-SAFETY: every field is a lock-free `std::atomic` accessed with relaxed
    ordering (the flush flag uses acq_rel on its exchange). The fields carry no
    cross-field invariant, so relaxed is sufficient for the scalars. */
struct MidiInputControl
{
    std::atomic<std::uint16_t> channelMask { 0xFFFF }; ///< Bit i ⇒ channel i+1 passes. All-pass default.
    std::atomic<std::uint16_t> voiceCount { 0 };       ///< Live MIDI-in sounding-note count (interim).
    std::atomic<bool> allNotesOffRequested { false };  ///< One-shot: emit CC123 on all channels + reset count.
};
} // namespace arpbox::engine
