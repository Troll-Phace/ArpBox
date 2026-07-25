#pragma once

#include "../EngineGuiGuard.h"
#include "Transport.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace arpbox::engine
{
/** The custom `juce::AudioPlayHead` that exposes ARPBOX's transport to every node
    in the graph — most importantly to HOSTED PLUGINS (ARCHITECTURE §3.3: "the
    custom `AudioPlayHead` exposes tempo/PPQ/play-state to hosted plugins (synced
    LFOs and delays depend on it)"). A tempo-synced delay or LFO in a third-party
    plugin is only correct because this object answers truthfully, and answers with
    the SAME values for the whole block.

    HOW IT IS INSTALLED (verified against JUCE 8.0.15):
      `EngineGraph::buildGraph()` calls `graph.setPlayHead (&playHead)` ONCE.
      `AudioProcessorGraph::processBlock` passes `getPlayHead()` into its render
      sequence, and every node's render op calls `processor.setPlayHead (...)` with
      it EVERY block — so the single call at build time propagates to every current
      and future node (including plugins inserted later by an async graph edit),
      with no per-node wiring.

      `AudioProcessorPlayer` has NO `setPlayHead()`; it installs its own internal
      sample-count playhead only when `processor.getPlayHead() == nullptr`. Because
      ours is already set on the graph, the player's is NEVER installed and ours is
      the only playhead in the system. (INSTRUCTIONS 5.1 says "custom AudioPlayHead
      on the player"; the only API that exists is on the graph — the effect is the
      same and strictly wider in reach.)

    RT-SAFETY: JUCE calls `getPosition()` from `processBlock` on the AUDIO thread,
    potentially once per hosted plugin per block. It reads the transport's already
    LATCHED block-start values and fills a stack `PositionInfo` — no allocation, no
    locks, no atomics needed (same thread, and the latch happened earlier in the
    same callback in the transport head node). */
class TransportPlayHead final : public juce::AudioPlayHead
{
public:
    /** Constructs a playhead reading `transportToRead`, which must outlive it (in
        `EngineGraph` both are members, transport declared first). */
    explicit TransportPlayHead (const Transport& transportToRead) noexcept
        : transport (transportToRead)
    {
    }

    /** ~TransportPlayHead. */
    ~TransportPlayHead () override = default;

    // RT-SAFE: audio thread. Called by JUCE from processBlock (once per hosted
    // plugin per block). Allocation-free, lock-free.
    /** Returns this block's transport position: tempo, PPQ, PPQ of the last bar
        start, time in samples/seconds, play state and a 4/4 time signature, all
        taken from the transport's LATCHED block-start values so every plugin in the
        block sees one consistent position. Never returns `nullopt` — the engine
        always knows its own time. */
    juce::Optional<PositionInfo> getPosition () const override;

private:
    const Transport& transport; ///< Non-owning; must outlive this playhead.

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportPlayHead)
};
} // namespace arpbox::engine
