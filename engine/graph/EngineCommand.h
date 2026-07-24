#pragma once

#include "../EngineGuiGuard.h"
#include "SpscFifo.h"

#include <cstdint>
#include <type_traits>

namespace arpbox::engine
{
/** Discriminator for a UI→engine command (ARCHITECTURE §3.4, channel 1).

    Kept as a small `uint8_t` enum so `EngineCommand` stays a compact POD that
    copies cheaply through the lock-free FIFO. One value per distinct action the
    message thread can ask the audio engine to perform.

    Phase 2 ships only the master-section controls below. Transport commands
    (play/stop/locate, set-tempo), pattern-switch queueing, seed re-roll, commit,
    etc. are ADDED HERE in Phase 5+ as new enumerators — never as a second queue
    type. Extend this enum; do not fork the channel. */
enum class EngineCommandType : std::uint8_t
{
    none = 0,             ///< No-op / default-initialised sentinel.
    setMasterGainDb,      ///< value.f = target master gain in decibels.
    setLimiterEnabled,    ///< value.i = 0/1 (safety limiter default ON).
    setTestToneEnabled,   ///< value.i = 0/1 (debug passthrough test tone).
    setTestToneFrequency, ///< value.f = tone frequency in Hz.
    // Phase 5+: transport (play/stop/locate/setTempo), pattern-switch queue,
    // seed/DICE, commit/uncommit — appended as new enumerators above this line.
};

/** A single POD command posted by the UI (message thread) and drained by the
    engine at the top of each `processBlock` (ARCHITECTURE §4, step 1).

    Trivially copyable by construction: an enum tag, an optional 16-bit target id
    (e.g. FX-slot index or parameter id for future targeted commands), and a
    type-punned scalar payload. The `union` keeps the struct to 8 bytes while
    letting each command type read the field it means. Interpret `value` strictly
    according to `type`; the producer and consumer agree on which member is live
    per `EngineCommandType`. */
struct EngineCommand
{
    EngineCommandType type = EngineCommandType::none;
    std::uint16_t targetId = 0; ///< Optional target (slot/param id); 0 when unused.

    /** Type-punned payload; the live member is fixed by `type`. */
    union Value
    {
        float f;
        std::int32_t i;
        std::uint32_t u;
    } value {};
};

static_assert (std::is_trivially_copyable_v<EngineCommand>,
               "EngineCommand must be trivially copyable to travel through SpscFifo.");

/** UI→engine command channel (ARCHITECTURE §3.4, channel 1). Single producer =
    message thread; single consumer = audio thread (drained per block). 1024
    slots absorb a burst of UI automation without dropping under normal use;
    drops are counted (`getDroppedCount`) rather than blocking the UI. */
using EngineCommandQueue = SpscFifo<EngineCommand, 1024>;
} // namespace arpbox::engine
