#pragma once

#include "../EngineGuiGuard.h"
#include "../graph/RetirementQueue.h"
#include "PatternSnapshot.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace arpbox::engine
{
// ─────────────────────────────────────────────────────────────────────────────
// PatternChannel.h — ARCHITECTURE §3.4's THIRD canonical cross-thread mechanism,
// "Pattern data": immutable `PatternSnapshot` objects built on the message
// thread, published by atomic pointer swap, adopted by the audio thread at a
// quantize boundary, and returned via FIFO for message-thread deletion.
//
// The other two are graph/EngineCommand.h (UI → engine) and
// graph/EngineSnapshotBuffer.h (engine → UI). §3.4 says "do not invent a fourth";
// this class is the third finally getting its production implementation. It is
// the FIRST consumer of graph/RetirementQueue.h, which Phase 2.3 built and
// unit-tested for exactly this seam.
//
// ── WHY THIS LIVES IN sequencer/ AND NOT graph/ ─────────────────────────────
// Purely the §3.2 ownership split: `engine/graph` belongs to the audio-graph
// owner, `engine/sequencer` to the sequencer owner, and the payload this channel
// carries is pattern data. The MECHANISM is §3.4's, and the reusable primitive
// (RetirementQueue) does live in graph/ — only this thin pattern-typed binding is
// here. Cross-referenced both ways so it stays findable from §3.4.
// ─────────────────────────────────────────────────────────────────────────────

/** The publish / adopt / retire channel for `PatternSnapshot` (§3.4 mechanism 3).

    ── LIFETIME, IN ONE PARAGRAPH ──────────────────────────────────────────────
    A snapshot is born owned by the message thread (`buildPatternSnapshot`), is
    handed to `publish` (ownership → the channel), is taken by the audio thread's
    `adopt` (ownership → the audio thread's held pointer), and dies on the message
    thread — either through `reclaim` after the audio thread retired it, or
    directly inside `publish` if the audio thread never took it. THE AUDIO THREAD
    NEVER CALLS `delete`, which is the whole point (code-style.md: "Retired
    snapshots are returned to the message thread for deletion").

    Threading: strictly one message thread and one audio thread. `publish` /
    `reclaim` / `peekPending` are message-thread only; `adopt` / `retire` are
    audio-thread only and RT-safe.

    ── HOW THE SPSC CONTRACT IS ENFORCED, NOT JUST STATED (issue #57) ──────────
    Every method that MUTATES the channel is non-const; every pure observation
    (`peekPending`, `getNumPendingRetirements`, `getDroppedRetirementCount`) is
    const. That split is load-bearing: `EngineGraph::patternSnapshots()` hands out
    a `const PatternChannel&`, so a message-thread caller reaching the graph's
    channel can observe it but CANNOT compile a call to `adopt`, `retire` or
    `publish` — the misuse that would create a second producer on the retirement
    queue or a second consumer on the publish slot. Keep new mutators non-const
    and new observers const, or that barrier quietly stops holding. */
class PatternChannel
{
public:
    /** Constructs an empty channel: nothing pending, nothing retired. */
    PatternChannel () = default;

    // MESSAGE-THREAD ONLY: call with the audio thread STOPPED.
    /** Reclaims everything the audio thread retired and deletes any snapshot that
        was published but never adopted.

        The snapshot the audio thread currently HOLDS is not owned here (the holder
        owns it) and is not touched — the audio thread must be stopped and its held
        pointer disposed of by its owner. */
    ~PatternChannel ()
    {
        reclaim ();

        // Never adopted: still owned by this channel.
        delete pendingPublish.exchange (nullptr, std::memory_order_acquire);
    }

    PatternChannel (const PatternChannel&) = delete;
    PatternChannel& operator= (const PatternChannel&) = delete;

    // MESSAGE-THREAD ONLY: may delete a displaced snapshot (allocation-adjacent —
    // never call from processBlock).
    /** Offers `next` to the audio thread, taking ownership.

        ── WHY `exchange` AND NOT `store` — THE LOAD-BEARING DETAIL ────────────
        A plain `store` LEAKS. Publish S2; the audio thread has not reached a
        quantize boundary yet; publish S3. S2's pointer is now unreachable, and the
        only code that ever retires a snapshot is the audio thread — which never
        saw S2. Under a piano-roll drag that is one leaked ~100 KB snapshot per
        mouse move.

        `exchange` returns the displaced pointer, and exactly ONE side can win the
        exchange: either the audio thread's `adopt` took S2 (this returns nullptr,
        the audio thread will retire it normally) or this call took it back (the
        audio thread can never see it again, so deleting it HERE, on the message
        thread, is safe and is the only deletion it will get). Either way S2 is
        freed exactly once and never on the audio thread.

        The consequence worth designing around: at most ONE snapshot is ever live
        in the pending slot, whatever the edit rate. Memory during a drag is
        bounded by the edit rate not at all — only by the adoption rate. */
    void publish (std::unique_ptr<const PatternSnapshot> next) noexcept
    {
        const PatternSnapshot* const displaced = pendingPublish.exchange (next.release (), std::memory_order_release);

        // Never adopted by the audio thread, so this thread still owns it.
        delete displaced;
    }

    // MESSAGE-THREAD ONLY: deletes. Call once per UI tick (or after publishing).
    /** Deletes every snapshot the audio thread has retired. Safe when empty. */
    void reclaim () noexcept { retired.reclaim (); }

    // MESSAGE-THREAD ONLY: observation.
    /** Snapshots awaiting `reclaim` (advisory). */
    int getNumPendingRetirements () const noexcept { return retired.getNumPending (); }

    // MESSAGE-THREAD ONLY: observation.
    /** Retirements dropped because the retirement queue was full. EACH ONE IS A
        LEAKED SNAPSHOT — the queue deliberately does not free on the audio thread
        (graph/RetirementQueue.h), so a dropped retirement is memory lost rather
        than a crash. Must stay 0; a non-zero value means the message thread is not
        calling `reclaim` often enough. */
    std::uint64_t getDroppedRetirementCount () const noexcept { return retired.getDroppedCount (); }

    // MESSAGE-THREAD ONLY: observation. TESTS ONLY — see the warning.
    /** The snapshot currently offered but not yet adopted, or nullptr.

        NON-OWNING and inherently racy against a live audio thread: `adopt` can
        take the pointer the instant after this returns. It exists so headless
        tests (which drive both sides by hand, with no real audio thread) can
        assert on the pending slot. Production code must not dereference it. */
    const PatternSnapshot* peekPending () const noexcept { return pendingPublish.load (std::memory_order_acquire); }

    // RT-SAFE: audio thread. One atomic exchange plus at most one FIFO push.
    // Lock-free, allocation-free; never deletes.
    /** Adopts a newly published snapshot, if there is one.

        On success `heldInOut` is retired (handed to the message thread for
        deletion) and replaced with the new snapshot, which the caller then owns
        the *use* of until its own next adoption. Call this at a quantize boundary
        (§4 step 3), not mid-step: adopting between two steps of the same bar is
        what a pattern edit is allowed to sound like; adopting mid-note is not.

        @param heldInOut  In: the snapshot currently in use (may be nullptr on the
                          very first adoption). Out: the newly adopted snapshot, or
                          untouched if there was nothing to adopt.
        @returns true if a swap happened. */
    bool adopt (const PatternSnapshot*& heldInOut) noexcept
    {
        const PatternSnapshot* const next = pendingPublish.exchange (nullptr, std::memory_order_acquire);

        if (next == nullptr)
            return false;

        // Ordering matters only in one direction: `heldInOut` must not be
        // overwritten before it is handed back, or it leaks silently.
        retired.retire (heldInOut);
        heldInOut = next;

        return true;
    }

    // RT-SAFE: audio thread. One FIFO push; never deletes.
    /** Hands `snapshot` back for message-thread deletion without adopting anything
        — for the teardown path where the audio thread drops its held snapshot
        (graph rebuild, `releaseResources`) with no replacement.

        @returns false if the retirement queue was full, which LEAKS `snapshot`
                 (see `getDroppedRetirementCount`). */
    bool retire (const PatternSnapshot* snapshot) noexcept { return retired.retire (snapshot); }

private:
    /** The single-slot publish hand-off. `exchange`-only on both sides — see the
        note on `publish` for why a `store` here would leak. */
    std::atomic<const PatternSnapshot*> pendingPublish { nullptr };

    /** Audio → message hand-back. 256 is far past what one quantize boundary per
        block can produce before a UI-tick `reclaim` drains it. */
    RetirementQueue<const PatternSnapshot, 256> retired;

    static_assert (std::atomic<const PatternSnapshot*>::is_always_lock_free,
                   "PatternChannel's publish slot must be lock-free: the audio thread "
                   "exchanges it inside processBlock (code-style.md: no locks).");
};
} // namespace arpbox::engine
