#pragma once

#include "../EngineGuiGuard.h"

#include <cstdint>

namespace arpbox::engine
{
/** Level values for `EngineSnapshot.deviceStatus` (a `std::uint8_t` field).

    This is the SHARED vocabulary the device/app layer (Phase 2.1) uses to report
    audio-device health into the engine→UI snapshot, and the UI reads to drive a
    status banner. It is a "level" — the most recent state overwrites the previous
    one — deliberately NOT an event: the message-thread device layer detects a
    device change and cannot push onto the audio-thread-only `EngineEventQueue`
    (that would break the SPSC contract, see EngineEvent.h). Instead it sets this
    level via `EngineGraph::setDeviceStatus()`, the audio thread copies it into
    every `EngineSnapshot` it publishes, and the UI observes it at 60 fps.

    Kept as a plain enum (not `enum class`) with explicit `std::uint8_t` values so
    it assigns directly to/from `EngineSnapshot.deviceStatus` without casts on
    either side of the boundary. */
enum DeviceStatus : std::uint8_t
{
    deviceStatusOk = 0,                ///< Requested device is active and healthy.
    deviceStatusFellBackToDefault = 1, ///< Requested device was lost; auto-fell back to the system default.
    deviceStatusDead = 2               ///< No usable output device (silence); UI shows a hard error.
};
} // namespace arpbox::engine
