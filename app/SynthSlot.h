#pragma once

#include "ISynthEngine.h"

#include "hosting/InstantiationResult.h"
#include "hosting/PluginInstantiator.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace arpbox::hosting
{
// Forward decl: the wrapper this coordinator constructs and hands to the engine
// as a base juce::AudioProcessor (kept out of this header — see the .cpp).
class HostedPluginNode;
} // namespace arpbox::hosting

namespace arpbox::app
{
/** Message-thread coordinator for the single instrument (synth) slot (ARCHITECTURE
    §6.2, §6.3; INSTRUCTIONS Phase 4.3).

    THE ONE PLACE that may name BOTH `hosting::HostedPluginNode` AND the engine: the
    engine library cannot include `hosting/`, so the wrapper is constructed here and
    handed to the engine as an opaque `std::unique_ptr<juce::AudioProcessor>`.

    RESPONSIBILITIES:
      - Async, failure-isolated load of a chosen `PluginDescription` via
        `PluginInstantiator` (the AUv3-safe `createPluginInstanceAsync` path).
      - Click/hang/stuck-note-free swap-under-playback choreography, driven off the
        UI tick (`poll()`), NOT a `juce::Timer` (prohibited — code-style.md).
      - In-flight invalidation (issue #16): a `generation` counter drops results
        from a load that was superseded by a newer load / a remove.
      - Output-gain trim forwarding, persisted across swaps.

    THREADING: MESSAGE-THREAD ONLY. Every method here runs on the message thread and
    touches the audio thread only indirectly, through the engine's own
    message-thread graph edits (`setSynth`/`removeSynth`, `UpdateKind::async`) and
    the wrapper's atomic-published setters. Plugin construction is async/off the
    audio thread; removed nodes are freed by JUCE on the message thread — this class
    never deletes a plugin instance on the audio thread.

    FAILURE ISOLATION: an instantiation failure surfaces as an error string and
    leaves any currently-running synth untouched — a bad plugin never drops the good
    one and never crashes the app (§6.2, §1.4). */
class SynthSlot final
{
public:
    /** Builds the coordinator. Borrows the engine (the graph it hands synths to,
        via the GUI-free `ISynthEngine` seam) and a format manager (to construct the
        internal `PluginInstantiator`); BOTH must outlive this object. MESSAGE-THREAD
        ONLY. */
    SynthSlot (ISynthEngine& engineToUse, juce::AudioPluginFormatManager& formats);

    /** Destroys the coordinator. Any pending async load is neutralised by the
        `WeakReference` guard, so a late instantiation callback becomes a no-op (it
        simply frees its prepared instance on the message thread). Any not-yet-
        inserted incoming node destructs here on the message thread. The
        currently-inserted synth (if any) is left owned by the engine graph, which
        outlives this coordinator. MESSAGE-THREAD ONLY. */
    ~SynthSlot ();

    // ── Slot operations (MESSAGE-THREAD ONLY) ─────────────────────────────────

    /** Asynchronously loads/swaps to the described instrument. Bumps `generation`
        and captures it, so a rapid second `load()` (or a `remove()`) invalidates
        this request when its callback lands. On success, wraps the prepared
        instance and — if a synth is already playing — runs the fade-out → silence
        handshake → swap → fade-in choreography via `poll()`. On failure, records the
        error and leaves any current synth running. */
    void load (const juce::PluginDescription& description);

    /** Removes the current synth (if any). Bumps `generation` (invalidating any
        pending load), flushes held notes, fades the current synth to silence, and
        removes it from the graph once silent — advanced by `poll()`. */
    void remove ();

    /** Advances the swap/remove state machine. MUST be called once per UI frame
        (from the DebugPanel's `VBlankAttachment` tick — never a `juce::Timer`). It
        polls the outgoing node's `isFadeOutComplete()` handshake and performs the
        graph edit only once the node is genuinely silent. */
    void poll ();

    // ── Gain trim (MESSAGE-THREAD ONLY; persisted across swaps) ───────────────

    /** Sets the synth output-gain trim in decibels. Applied to the current synth
        immediately and re-applied to any synth loaded later. */
    void setGainDb (float gainDb);

    /** Sets the synth output-gain trim as a linear multiplier (1.0 = unity). */
    void setGainLinear (float linearGain);

    // ── Accessors for the UI (MESSAGE-THREAD ONLY) ────────────────────────────

    /** Display name of the current synth, or an empty string when none is loaded. */
    juce::String getCurrentSynthName () const;

    /** True when a synth is currently inserted in the graph. */
    bool isLoaded () const noexcept { return currentSynth != nullptr; }

    /** True while an async load is in flight OR a swap/remove is mid-handshake. */
    bool isPending () const noexcept;

    /** Reported latency of the current synth's wrapped instance, in samples
        (0 when nothing is loaded). */
    int getLatencySamples () const;

    /** The last instantiation error text (empty if the last load succeeded or none
        has been attempted). Cleared on the next successful load. */
    juce::String getLastError () const { return lastError; }

    /** Diagnostic seam (for the test agent, Delegation D): number of instantiation
        results dropped because they were superseded by a newer load/remove. */
    int getDroppedAsStaleCount () const noexcept { return droppedAsStale; }

private:
    // Message-thread swap state. `awaitingFadeOut` ⇒ the current synth is ramping to
    // silence; when it reports `isFadeOutComplete()`, `poll()` either inserts the
    // pending incoming node (swap) or removes the node (remove), whichever applies.
    enum class State
    {
        idle,
        awaitingFadeOut
    };

    // MESSAGE-THREAD ONLY: message-thread completion of an async instantiation.
    // Drops the result when `callbackGeneration` != the live `generation`.
    void onInstantiated (int callbackGeneration, hosting::InstantiationResult result);

    // MESSAGE-THREAD ONLY: inserts `node` as the first synth directly (nothing to
    // fade out), armed silent then faded in.
    void insertFirstSynth (std::unique_ptr<hosting::HostedPluginNode> node, juce::String name);

    // MESSAGE-THREAD ONLY: applies the persisted output-gain trim to the current
    // synth (no-op when none is loaded).
    void applyGainToCurrent ();

    // MESSAGE-THREAD ONLY: enters the awaitingFadeOut state and (re)arms the bounded
    // fade-out poll budget (issue #24). Call when the current synth starts fading out
    // for a swap or a remove.
    void beginFadeOutWait () noexcept;

    ISynthEngine& engine; ///< Non-owning; the graph this coordinator hands synths to.
    hosting::PluginInstantiator instantiator; ///< Owns the async create+prepare path.

    // Non-owning: valid while the engine graph owns the node. Cleared on removal.
    hosting::HostedPluginNode* currentSynth = nullptr;
    juce::String currentName;

    // Incoming node: constructed + armed silent, waiting for the outgoing node to go
    // silent before it is inserted. Never inserted ⇒ safe to drop on the message
    // thread. Owned here until moved into the engine in `poll()`.
    std::unique_ptr<hosting::HostedPluginNode> pendingNode;
    juce::String pendingName;

    State state = State::idle;

    // Bounded fade-out wait (issue #24). The wrapper's `isFadeOutComplete()` flag
    // only advances inside its audio-thread `processBlock`, so if the device is
    // stopped/dead the fade never completes and `poll()` would wait — and
    // `isPending()` stay true — forever. `poll()` counts this budget down while
    // awaiting the handshake and forces the graph edit when it reaches zero, so a
    // swap/remove requested while audio is stopped terminates. When audio IS running
    // the ~10 ms fade completes within 1–2 frames, far inside the budget, so the
    // click-free behaviour is untouched. Message-thread only; re-armed per wait.
    static constexpr int kMaxFadeOutPolls = 30; ///< ~0.5 s at 60 fps.
    int fadeOutPollsRemaining = 0;

    // In-flight invalidation (issue #16). Message-thread only — NOT atomic. Bumped by
    // load()/remove(); each instantiation callback captures the value at request time
    // and drops itself if it no longer matches.
    int generation = 0;
    int inFlightInstantiations = 0; ///< Outstanding async loads (for isPending()).
    int droppedAsStale = 0;         ///< Diagnostic: superseded results dropped.

    // Persisted output-gain trim (linear), re-applied to every synth we load.
    float outputGainLinear = 1.0f;

    juce::String lastError;

    // Guards the async instantiation callback against use-after-free if this slot is
    // destroyed while a load is in flight (the callback is a bare MessageManager
    // async task with no lifetime tie to us).
    JUCE_DECLARE_WEAK_REFERENCEABLE (SynthSlot)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SynthSlot)
};
} // namespace arpbox::app
