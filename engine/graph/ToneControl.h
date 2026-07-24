#pragma once

#include "../EngineGuiGuard.h"

#include <atomic>

namespace arpbox::engine
{
/** Lock-free control state for the debug `TestToneProcessor`, shared between two
    graph nodes that live on the SAME audio thread but process in a fixed order.

    WHY THIS EXISTS (Phase 2 command-drain arrangement):
      The single `EngineCommandQueue` is strict SPSC — it may have exactly ONE
      consumer. `MasterProcessor` is that consumer: it drains the queue at the top
      of its `processBlock`. But two of the Phase-2 commands
      (`setTestToneEnabled` / `setTestToneFrequency`) target the tone SOURCE node,
      which the graph processes BEFORE the master. The master therefore cannot
      hand those commands to the tone node by a return value — it publishes them
      into this shared, atomic control block, and `TestToneProcessor` reads it at
      the top of its own `processBlock`.

    ORDERING NOTE: because the tone node runs earlier in the block than the master
    that drains the command, a tone-control change lands on the tone node ONE
    block later (~a few ms). That one-block latency is irrelevant for a debug test
    tone and is the intended trade-off for keeping a single, canonical command
    consumer. Phase 5 replaces the master-as-drainer arrangement with a dedicated
    head node and removes this shim.

    RT-SAFETY: both fields are `std::atomic` written/read with relaxed ordering —
    no lock, no allocation. The two scalars are independent (no cross-field
    invariant), so relaxed is sufficient; the tone node simply reads the latest
    value each field has. */
struct ToneControl
{
    std::atomic<bool> enabled { false };      ///< Debug tone on/off (default OFF).
    std::atomic<float> frequencyHz { 440.0f };///< Tone frequency in Hz (default A4).
};
} // namespace arpbox::engine
