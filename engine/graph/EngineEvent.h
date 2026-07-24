#pragma once

#include "../EngineGuiGuard.h"
#include "SpscFifo.h"

#include <cstdint>
#include <type_traits>

namespace arpbox::engine
{
/** Discriminator for a discrete engine→UI event (ARCHITECTURE §3.4, channel 2,
    event half). Where `EngineSnapshot` carries continuous per-block STATE, these
    are one-shot NOTIFICATIONS the UI reacts to once (fire an animation, show a
    banner, recompute a latency badge). */
enum class EngineEventType : std::uint8_t
{
    deviceChanged = 0,       ///< Active audio device changed (a = new SR, b = block size).
    deviceDied,              ///< Current device dropped out (unplugged / lost).
    deviceFellBackToDefault, ///< Auto-fallback to the default device occurred.
    sampleRateChanged,       ///< Sample rate changed (a = new SR in Hz).
    bufferSizeChanged,       ///< Block size changed (a = new size in samples).
    latencyChanged,          ///< Graph latency recomputed (a = total latency samples).
    // Phase 5+: stepFired, patternSwitched, seedRolled, etc. appended here.
};

/** A single POD event. `a`/`b` are event-specific payload words (see each
    enumerator); unused words stay 0. Trivially copyable so it rides the FIFO. */
struct EngineEvent
{
    EngineEventType type = EngineEventType::deviceChanged;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
};

static_assert (std::is_trivially_copyable_v<EngineEvent>,
               "EngineEvent must be trivially copyable to travel through SpscFifo.");

/** Discrete engine→UI event channel (ARCHITECTURE §3.4, channel 2).

    STRICT SPSC — the SINGLE producer is the AUDIO THREAD ONLY. A device change
    that is detected on the MESSAGE thread (e.g. an `AudioIODeviceCallback`
    device-stopped notification handled off the audio thread) must NOT be pushed
    here: a second producer would break the SPSC contract and corrupt the FIFO.
    Instead, message-thread-detected changes update the UI directly (both sides
    already live on the message thread) AND are mirrored into
    `EngineSnapshot.deviceStatus` (a level field) so the state is observable
    without a cross-thread push. 256 slots comfortably hold a burst of events
    between UI frames. */
using EngineEventQueue = SpscFifo<EngineEvent, 256>;
} // namespace arpbox::engine
