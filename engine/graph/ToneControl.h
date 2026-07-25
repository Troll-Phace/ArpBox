#pragma once

#include "../EngineGuiGuard.h"

#include <atomic>

namespace arpbox::engine
{
/** Lock-free control state for the debug `TestToneProcessor`, shared between two
    graph nodes that live on the SAME audio thread but process in a fixed order.

    WHY THIS EXISTS:
      The single `EngineCommandQueue` is strict SPSC — it may have exactly ONE
      consumer. Since Phase 5.1 that consumer is `TransportProcessor` (the graph's
      head node), which fans each drained command out to the registered
      `ICommandSink`s. `MasterProcessor` is the sink that owns the two test-tone
      commands (`setTestToneEnabled` / `setTestToneFrequency`), but they target the
      tone SOURCE node — a different processor. The master cannot hand them over by
      a return value, so it publishes them into this shared, atomic control block
      and `TestToneProcessor` reads it at the top of its own `processBlock`.

    ORDERING NOTE — WHY THE SHIM SURVIVES PHASE 5: the tone node is NOT downstream
    of the transport head node (nothing feeds it; it only feeds the master), so its
    render order relative to the head node is UNCONSTRAINED by the topology. It can
    therefore run before the drain, and a tone-control change may land on the tone
    node ONE block later (~a few ms). That is irrelevant for a debug test tone, and
    the alternative — an ordering-only edge into a node that accepts no MIDI — does
    not exist. This shim lives until `TestToneProcessor` itself is deleted (Phase
    15+, when the real UI replaces the debug panel that drives it).

    RT-SAFETY: both fields are `std::atomic` written/read with relaxed ordering —
    no lock, no allocation. The two scalars are independent (no cross-field
    invariant), so relaxed is sufficient; the tone node simply reads the latest
    value each field has. */
struct ToneControl
{
    std::atomic<bool> enabled { false };       ///< Debug tone on/off (default OFF).
    std::atomic<float> frequencyHz { 440.0f }; ///< Tone frequency in Hz (default A4).
};
} // namespace arpbox::engine
