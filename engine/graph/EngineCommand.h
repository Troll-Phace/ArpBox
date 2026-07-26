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

    Pattern-switch queueing, seed re-roll, commit, etc. are ADDED HERE in later
    phases as new enumerators — never as a second queue type. Extend this enum; do
    not fork the channel.

    APPEND-ONLY: enumerators are never renumbered or reordered. The numeric values
    are not persisted today, but the producer/consumer pairing across the FIFO (and
    Phase 11's project schema) treats them as stable identities. */
enum class EngineCommandType : std::uint8_t
{
    none = 0,             ///< No-op / default-initialised sentinel.
    setMasterGainDb,      ///< value.f = target master gain in decibels.
    setLimiterEnabled,    ///< value.i = 0/1 (safety limiter default ON).
    setTestToneEnabled,   ///< value.i = 0/1 (debug passthrough test tone).
    setTestToneFrequency, ///< value.f = tone frequency in Hz.
    // ── Phase 5.1 transport (consumed by `Transport`, an ICommandSink) ────────
    transportPlay,   ///< no payload — start the transport.
    transportStop,   ///< no payload — stop AND rewind to PPQ 0 (§5.5 flush point).
    transportLocate, ///< value.d = target PPQ (must be finite and >= 0).
    setTempoBpm,     ///< value.d = target tempo in BPM (clamped to 20..300).
    // ── Phase 6.1 sequencer (consumed by `SequencerProcessor`, an ICommandSink) ─
    queuePatternSwitch, ///< targetId = pattern index 0..15; value.u = QuantizeMode.
    // ── Phase 7.1 step logic (consumed by `SequencerProcessor`, an ICommandSink) ─
    /** FILL flag (pad 16 held), §12.2 — consumed by SequencerProcessor.
        `value.i` = 0 (released) / 1 (held); any non-zero reads as held.

        Deliberately a COMMAND rather than snapshot state: FILL gates the
        `FILL`/`!FILL` trig conditions and is a momentary live-performance flag
        that changes on both press AND release, so carrying it on
        `PatternSnapshot` would rebuild and republish the whole document twice
        per pad tap. Master and Transport ignore it via their `default:` arms
        (ICommandSink.h fan-out contract). */
    setFillHeld,
    // Later phases: seed/DICE, commit/uncommit — appended as new enumerators above
    // this line.
};

/** A single POD command posted by the UI (message thread) and drained by the
    engine at the top of each `processBlock` (ARCHITECTURE §4, step 1).

    Trivially copyable by construction: an enum tag, an optional 16-bit target id
    (e.g. FX-slot index or parameter id for future targeted commands), and a
    type-punned scalar payload. Interpret `value` strictly according to `type`; the
    producer and consumer agree on which member is live per `EngineCommandType`.

    PAYLOAD WIDTH (Phase 5.1): the union carries a 64-bit member, so the struct is
    16 bytes (8 payload + tag/target + padding) rather than 8. That is deliberate
    and required, not convenience:
      - `d` (double) — tempo and locate targets. BPM and PPQ are the inputs to the
        transport's exact PPQ arithmetic (see Transport.h); rounding them through a
        `float` on the way across the FIFO would inject error into the determinism
        contract at the very first step.
      - `u64` — Phase 12's xoshiro256++ seeds are 64-bit; a seed truncated to 32
        bits is a different (and unreproducible) stream.
    The queue is a fixed inline array of 1024 items, so the extra 8 bytes cost 8 KB
    of static storage and nothing on the hot path. */
struct EngineCommand
{
    EngineCommandType type = EngineCommandType::none;
    /** Optional target id; 0 when unused. FIRST REAL USE is Phase 6.1's
        `queuePatternSwitch`, where it carries the destination pattern index
        (0..`maxPatterns`-1). Later: FX-slot index, parameter id. */
    std::uint16_t targetId = 0;

    /** Type-punned payload; the live member is fixed by `type`. */
    union Value
    {
        float f;
        std::int32_t i;
        std::uint32_t u;
        double d;          ///< Tempo (BPM) / locate target (PPQ) — needs full precision.
        std::uint64_t u64; ///< 64-bit RNG seeds (Phase 12).
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
