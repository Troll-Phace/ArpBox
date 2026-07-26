#pragma once

#include "../EngineGuiGuard.h"

#include <cstdint>

namespace arpbox::engine::provenance
{
// ─────────────────────────────────────────────────────────────────────────────
// Provenance.h — THE §5.4 X-RAY BITMASK. "Every emitted note carries a
// provenance bitmask (core, each operator slot, constraint-snapped,
// ratchet-child). Rides the step event into the `EngineSnapshot`; UI renders
// badges."
//
// ── WHAT IT IS FOR, AND WHY IT EXISTS THREE PHASES EARLY ────────────────────
// Phase 17 builds the X-RAY view; Phase 12 fills in the operator bits. Neither
// is here yet. The mask is nevertheless plumbed NOW because §5.4 makes it a
// property of an EMITTED NOTE, and Phase 7.2 is the phase that turns "one note
// per step" into "one to eight notes per step" — i.e. the phase in which a note
// first has a source that is not simply "the step". Retrofitting a per-note
// field after ratchets ship would mean reshaping `StepNote` twice, and the
// determinism suite would have no way to tell the ratchet children apart from
// core notes in the interim.
//
// ── THE LAYOUT IS APPEND-ONLY AND THE GAP IS DELIBERATE ─────────────────────
// The mask is not serialized (§8.1 has no provenance key — it is derived, not
// stored), so a re-layout would not corrupt a project. It is still treated as
// append-only, because it rides the `EngineSnapshot` across a thread boundary
// and a UI built against one layout must not silently mis-badge notes produced
// by an engine built against another. The 5-bit reserved gap exists so the
// pipeline-stage bits stay contiguous and the operator block stays byte-aligned
// at bit 8, which is what lets the UI read the operator set as one byte.
// ─────────────────────────────────────────────────────────────────────────────

/** Nothing recorded. A well-formed emitted note always has at least `core`; this
    value means "the mask was never filled in". */
inline constexpr std::uint32_t none = 0u;

/** §5.1 L1 — the note came from the deterministic pattern core (the committed
    material). Set on EVERY note this engine emits, including ratchet children:
    the bits are additive, not exclusive. */
inline constexpr std::uint32_t core = 1u << 0;

/** §5.1 L2 — the note is a RATCHET CHILD, i.e. a subdivision of its step rather
    than the step's own onset. Set on children `c > 0` only; child 0 IS the step's
    onset and carries `core` alone.

    NOT set from `noteCount > 1`: the parent of an 8-ratchet step is still a
    parent. The distinction is what the X-RAY badge needs — "this note is here
    because the RATCHET lane says 8", as opposed to "this note is here because the
    GATE lane fired". */
inline constexpr std::uint32_t ratchetChild = 1u << 1;

/** The constraint gate (§5.1) moved this note — a scale-mask snap or a range
    fold/clamp. Phase 12.3 sets it; nothing sets it yet. */
inline constexpr std::uint32_t constraintSnapped = 1u << 2;

// Bits 3..7 are RESERVED for further pipeline stages (§5.1 L0's pool source, the
// note-budget suppressor, …). Do not use them for operator slots.

/** Bit position of operator slot 0. The eight §5.1 L3 operator slots occupy bits
    8..15, one per slot, so the whole stack's contribution to a note is readable
    as one byte (`(mask >> operatorSlotShift) & 0xFF`). Phase 12.2 sets them. */
inline constexpr int operatorSlotShift = 8;

/** Operator slots per §5.1 L3 ("ordered, reorderable, ≤ 8"). */
inline constexpr int numOperatorSlots = 8;

/** The bit for operator slot `slot` (0..`numOperatorSlots`-1). Out-of-range
    slots yield `none` rather than aliasing onto another stage's bit. */
constexpr std::uint32_t operatorSlot (int slot) noexcept
{
    if (slot < 0 || slot >= numOperatorSlots)
        return none;

    return 1u << (operatorSlotShift + slot);
}

/** Every operator-slot bit — the mask the X-RAY view ANDs with to ask "did the
    operator stack touch this note at all". */
inline constexpr std::uint32_t allOperatorSlots = 0xFF00u;

static_assert (operatorSlot (0) == (1u << 8), "Operator slot 0 must sit at bit 8 (byte-aligned).");
static_assert (operatorSlot (7) == (1u << 15), "Eight operator slots occupy bits 8..15.");
static_assert (operatorSlot (8) == none, "Out-of-range slots must not alias another stage's bit.");
static_assert ((core | ratchetChild | constraintSnapped) == 0x7u, "The pipeline-stage bits are 0..2.");
static_assert ((allOperatorSlots & 0xFFu) == 0u, "The reserved gap keeps the stage bits clear of the operator byte.");
} // namespace arpbox::engine::provenance
