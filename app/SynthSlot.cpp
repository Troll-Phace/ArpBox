#include "SynthSlot.h"

#include "AudioEngine.h"
#include "hosting/HostedPluginNode.h"

#include <utility>

namespace arpbox::app
{
using namespace juce;

// MESSAGE-THREAD ONLY.
SynthSlot::SynthSlot (AudioEngine& engineToUse, AudioPluginFormatManager& formats)
    : engine (engineToUse), instantiator (formats)
{
}

// MESSAGE-THREAD ONLY. `pendingNode` (if any) destructs here on the message thread;
// it was never inserted, so this is safe. The inserted synth stays with the engine
// graph (which outlives this coordinator). The WeakReference master (declared by
// JUCE_DECLARE_WEAK_REFERENCEABLE) zeroes out here, so any pending instantiation
// callback becomes a no-op.
SynthSlot::~SynthSlot () = default;

// MESSAGE-THREAD ONLY.
void SynthSlot::load (const PluginDescription& description)
{
    // Bump + capture the generation so a superseding load()/remove() invalidates
    // this request when its callback lands (issue #16 in-flight invalidation).
    const int myGeneration = ++generation;
    ++inFlightInstantiations;

    // Prepare at the graph's CURRENT config. Before a device is open these are 0/0
    // (W4): real third-party plugins routinely divide by the sample rate in
    // prepareToPlay, so instantiating at 0 crashes them. Clamp to sane defaults for
    // the safe-instantiation seed; the wrapper re-prepares the inner at the real
    // SR/block once audio starts (its prepare-on-change guard), so the clamp only
    // seeds a valid initial prepare and never sticks.
    const double graphSampleRate = engine.getCurrentSampleRate ();
    const int graphBlockSize = engine.getCurrentBlockSize ();
    const double sampleRate = graphSampleRate > 0.0 ? graphSampleRate : 44100.0;
    const int blockSize = graphBlockSize > 0 ? graphBlockSize : 512;

    // WeakReference guard: this callback is a bare async task with no lifetime tie to
    // us. If the slot is destroyed first, `weak.get()` returns null and the prepared
    // instance simply frees on the message thread.
    WeakReference<SynthSlot> weak (this);
    instantiator.instantiate (description, sampleRate, blockSize,
                              [weak, myGeneration] (hosting::InstantiationResult result) mutable
                              {
                                  if (auto* self = weak.get ())
                                      self->onInstantiated (myGeneration, std::move (result));
                              });
}

// MESSAGE-THREAD ONLY.
void SynthSlot::onInstantiated (int callbackGeneration, hosting::InstantiationResult result)
{
    --inFlightInstantiations;

    // Superseded by a newer load()/remove() while this one was in flight: drop it.
    // The prepared instance inside `result` destructs on the message thread as this
    // scope unwinds — never on the audio thread (issue #16).
    if (callbackGeneration != generation)
    {
        ++droppedAsStale;
        return;
    }

    // Failure isolation (§6.2, §1.4): surface the error, leave any current synth
    // running. A bad plugin never drops the good one.
    if (! result.ok ())
    {
        lastError = result.message;
        return;
    }

    lastError.clear ();

    // Wrap the prepared instance and ARM IT SILENT (swap-IN recipe from Wave-1 B):
    // fadeOut() before insertion makes the graph's prepareToPlay seed the gain at 0,
    // so the later fadeIn() ramps up click-free.
    auto node = std::make_unique<hosting::HostedPluginNode> (std::move (result.instance));
    node->fadeOut ();
    auto name = node->getName ();

    // First load with nothing playing and no swap already staged: insert directly.
    if (currentSynth == nullptr && pendingNode == nullptr && state == State::idle)
    {
        insertFirstSynth (std::move (node), std::move (name));
        return;
    }

    // Otherwise this becomes the incoming node for a swap. Replace any earlier
    // pending incoming (it was never inserted — dropping it here is safe).
    pendingNode = std::move (node);
    pendingName = std::move (name);

    // Begin fading the current synth out, if we are not already mid-handshake. If a
    // swap/remove is already awaiting fade-out, the outgoing node is the same one and
    // is already ramping down — poll() will pick up the (replaced) pendingNode.
    if (state == State::idle)
    {
        // Stuck-note guard: flush held MIDI-in notes before the outgoing synth goes.
        // (No sounding-note table until Phase 8; this is the interim guard.)
        engine.allNotesOff ();
        if (currentSynth != nullptr)
            currentSynth->fadeOut ();
        state = State::awaitingFadeOut;
    }
}

// MESSAGE-THREAD ONLY.
void SynthSlot::insertFirstSynth (std::unique_ptr<hosting::HostedPluginNode> node, String name)
{
    auto* raw = node.get ();

    // Hand ownership to the engine as a base AudioProcessor; the graph inserts and
    // prepares it (gain seeded 0 because we armed it silent above).
    engine.setSynth (std::move (node));

    currentSynth = raw;
    currentName = std::move (name);

    applyGainToCurrent ();
    raw->fadeIn (); // click-free ramp to unity.
}

// MESSAGE-THREAD ONLY.
void SynthSlot::remove ()
{
    // Invalidate any pending load: its callback will drop when it lands.
    ++generation;

    // Cancel a not-yet-inserted incoming node (safe to drop; never inserted).
    pendingNode.reset ();
    pendingName.clear ();

    if (currentSynth != nullptr)
    {
        engine.allNotesOff (); // stuck-note guard before the synth leaves.
        currentSynth->fadeOut ();
        state = State::awaitingFadeOut; // poll() removes it once silent.
    }
    else
    {
        state = State::idle;
    }
}

// MESSAGE-THREAD ONLY (UI tick — VBlankAttachment, never a juce::Timer).
void SynthSlot::poll ()
{
    // Message-thread drain of the wrapper's lock-free latencyDirty flag (W3): a
    // hostile inner can flag a runtime latency change from its audio-thread
    // processBlock, and the owner services it here. Must run UNCONDITIONALLY, every
    // frame, BEFORE the idle early-return below — otherwise a latency change while
    // no swap is in flight would never be applied.
    if (currentSynth != nullptr)
        currentSynth->pollPendingLatencyChange ();

    if (state != State::awaitingFadeOut)
        return;

    // Invariant: awaitingFadeOut ⇒ a synth is inserted and fading out. Defensive
    // (S4): if the invariant is ever violated, don't strand a staged incoming node —
    // insert it directly (nothing to fade out) rather than dropping to idle and
    // leaking the pending swap.
    if (currentSynth == nullptr)
    {
        if (pendingNode != nullptr)
            insertFirstSynth (std::move (pendingNode), std::move (pendingName));

        pendingName.clear ();
        state = State::idle;
        return;
    }

    // Wait until the outgoing node is genuinely silent (audio-thread → us handshake),
    // so no audible tail is chopped.
    if (! currentSynth->isFadeOutComplete ())
        return;

    if (pendingNode != nullptr)
    {
        // SWAP: setSynth removes the old node (JUCE frees it on the message thread —
        // no audio-thread delete) and inserts + prepares the new one (gain seeded 0).
        auto* raw = pendingNode.get ();
        engine.setSynth (std::move (pendingNode));

        currentSynth = raw;
        currentName = std::move (pendingName);
        pendingName.clear ();

        applyGainToCurrent ();
        raw->fadeIn (); // click-free ramp to unity.
    }
    else
    {
        // REMOVE: the outgoing node is silent — take it out of the graph.
        engine.removeSynth ();
        currentSynth = nullptr;
        currentName.clear ();
    }

    state = State::idle;
}

// MESSAGE-THREAD ONLY.
void SynthSlot::setGainDb (float gainDb)
{
    setGainLinear (Decibels::decibelsToGain (gainDb));
}

// MESSAGE-THREAD ONLY.
void SynthSlot::setGainLinear (float linearGain)
{
    outputGainLinear = linearGain;
    applyGainToCurrent ();
}

// MESSAGE-THREAD ONLY.
void SynthSlot::applyGainToCurrent ()
{
    if (currentSynth != nullptr)
        currentSynth->setOutputGain (outputGainLinear);
}

// MESSAGE-THREAD ONLY.
String SynthSlot::getCurrentSynthName () const
{
    return currentSynth != nullptr ? currentName : String {};
}

// MESSAGE-THREAD ONLY.
bool SynthSlot::isPending () const noexcept
{
    return inFlightInstantiations > 0 || state == State::awaitingFadeOut || pendingNode != nullptr;
}

// MESSAGE-THREAD ONLY.
int SynthSlot::getLatencySamples () const
{
    if (currentSynth != nullptr)
        if (auto* inner = currentSynth->getWrappedInstance ())
            return inner->getLatencySamples ();

    return 0;
}
} // namespace arpbox::app
