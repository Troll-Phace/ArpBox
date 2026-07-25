#pragma once

#include "../EngineGuiGuard.h"

#include <cstdint>
#include <type_traits>

namespace arpbox::engine
{
/** Immutable-by-convention POD the engine writes once per audio block and the UI
    reads at ~60 fps (ARCHITECTURE §3.4, channel 2). This is the engine→UI STATE
    snapshot; discrete one-shot notifications go through `EngineEvent` instead.

    It is published through `EngineSnapshotBuffer` (a fixed-slot triple buffer),
    so the UI always sees a whole, self-consistent block of state and never a
    torn half-write. Every field is a plain scalar → trivially copyable, cheap to
    memcpy between slots.

    Field lifecycle across phases: Phase 2 populates the meters and device status;
    the transport/step/seed/voice fields are PRESENT but zero until Phase 5 wires
    the sequencer. Keeping them here now freezes the struct layout so the UI and
    tests can bind against final field names from the start. */
struct EngineSnapshot
{
    // ── Transport (Phase 5 fills; zeroed until then) ─────────────────────────
    double ppqPosition = 0.0; ///< Playhead position in quarter notes.
    double bpm = 0.0;         ///< Current tempo (beats per minute).
    bool isPlaying = false;   ///< Transport running state.

    // ── Master meters (Phase 2) — LINEAR amplitude, NOT dB ───────────────────
    // The UI converts to dB for display; keeping meters linear avoids a log on
    // the audio thread and keeps -inf handling on the UI side.
    float peakL = 0.0f; ///< Left peak, linear (0 = silence, 1 = 0 dBFS).
    float peakR = 0.0f; ///< Right peak, linear.
    float rmsL = 0.0f;  ///< Left RMS over the block, linear.
    float rmsR = 0.0f;  ///< Right RMS over the block, linear.

    // ── Generative / sequencer (Phase 5+; zeroed until then) ─────────────────
    std::uint32_t seed = 0;       ///< Current master seed (for the UI readout).
    std::uint16_t voiceCount = 0; ///< Live sounding-note count.

    // ── Engine/device status ────────────────────────────────────────────────
    std::uint8_t deviceStatus = 0; ///< Device-state LEVEL (see app/master enum).

    // ── Freshness ────────────────────────────────────────────────────────────
    /** Monotonically increasing per committed block. The UI compares against the
        last value it saw to detect a fresh write vs. STARVATION (engine stalled
        or stopped): if `blockCounter` is unchanged frame-over-frame, the state is
        stale and the UI can render a "no data" / paused affordance. */
    std::uint64_t blockCounter = 0;
};

static_assert (std::is_trivially_copyable_v<EngineSnapshot>,
               "EngineSnapshot must be trivially copyable — the triple buffer "
               "publishes it by value and it must never carry ownership.");
} // namespace arpbox::engine
