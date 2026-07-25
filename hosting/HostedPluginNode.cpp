#include "hosting/HostedPluginNode.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>

namespace arpbox::hosting
{
using namespace juce;

namespace
{
// A gain below this (and not smoothing) counts as "silent" for the swap handshake.
constexpr float kSilenceEpsilon = 1.0e-6f;
} // namespace

//==============================================================================
HostedPluginNode::BusesProperties HostedPluginNode::makeWrapperBuses ()
{
    // Synth wrapper (§6.3, Phase 4): MIDI in, stereo audio out, NO audio in.
    return BusesProperties ().withOutput ("Output", AudioChannelSet::stereo (), true);
}

HostedPluginNode::HostedPluginNode (std::unique_ptr<juce::AudioPluginInstance> preparedInstance)
    : juce::AudioProcessor (makeWrapperBuses ())
    , inner (std::move (preparedInstance))
{
    // Constructed on the message thread from an already-prepared inner (§6.2). The
    // contract guarantees a non-null instance for a wrapped node; guard anyway so a
    // misuse degrades to a silent node rather than a crash.
    jassert (inner != nullptr);

    if (inner == nullptr)
    {
        renderActive.store (false, std::memory_order_relaxed);
        return;
    }

    pluginName = inner->getName ();

    // S5: the inner was ALREADY prepared by the instantiator (§6.2). Seed the
    // re-prepare guards from what the instance reports so an identical first
    // prepareToPlay from the graph is a no-op (avoids the benign double-prepare).
    // Instances that were prepared WITHOUT setRateAndBufferSizeDetails report 0
    // here (some fakes do this) — the guard then simply won't match and the first
    // graph prepare runs normally, which is harmless.
    lastPreparedSampleRate = inner->getSampleRate ();
    lastPreparedBlockSize = inner->getBlockSize ();

    // Latency-change forwarding (§6.3 B5): listen so an inner that revises its
    // latency after prepare propagates to the graph.
    inner->addListener (this);

    // Seed the bus layout + latency now so the node is graph-ready before its first
    // prepareToPlay. Re-prepare of the inner happens in prepareToPlay at the graph's
    // authoritative SR/block.
    negotiateBusLayout ();
    setLatencySamples (inner->getLatencySamples ());
}

HostedPluginNode::~HostedPluginNode ()
{
    // MESSAGE-THREAD ONLY (guaranteed by the engine's removeNode path). Detach the
    // listener before the inner is destroyed; the unique_ptr then frees it here.
    // Nothing to tear down for the deferred-latency path — it is a plain atomic flag
    // with no scheduled callback that could outlive us.
    if (inner != nullptr)
        inner->removeListener (this);
}

//==============================================================================
void HostedPluginNode::negotiateBusLayout ()
{
    // MESSAGE-THREAD ONLY. Prefer stereo out; adapt mono→stereo (duplicated in
    // processBlock); fall back to the inner's current layout if already valid;
    // finally try enableAllBuses. If nothing works, mark the node silent-but-valid
    // (renderActive == false) — a bus-lying plugin degrades one slot, never crashes.
    innerInChannels = 0;
    innerOutChannels = 0;
    fastStereoPath = false;

    if (inner == nullptr)
    {
        renderActive.store (false, std::memory_order_relaxed);
        return;
    }

    // Build a candidate layout: keep the inner's inputs, force the main output bus
    // to `out`, disable any extra output buses (we present a single stereo out).
    const auto trySetOutput = [this] (const AudioChannelSet& out) -> bool
    {
        auto layout = inner->getBusesLayout ();

        if (layout.outputBuses.isEmpty ())
            return false;

        layout.outputBuses.getReference (0) = out;
        for (int i = 1; i < layout.outputBuses.size (); ++i)
            layout.outputBuses.getReference (i) = AudioChannelSet::disabled ();

        return inner->setBusesLayout (layout);
    };

    // Short-circuit chain: '||' stops at the first success and evaluates side
    // effects strictly left-to-right, so this is behaviour-identical to the
    // previous if/else-if ladder (each branch bodied `ok = true`) while avoiding
    // the bugprone-branch-clone repeated-body diagnostic. The third clause keeps
    // whatever layout the plugin already has, if that is valid.
    const bool ok = trySetOutput (AudioChannelSet::stereo ())
                 || trySetOutput (AudioChannelSet::mono ())
                 || inner->checkBusesLayoutSupported (inner->getBusesLayout ())
                 || inner->enableAllBuses ();

    if (! ok)
    {
        // Exotic / refusing plugin (e.g. the bus-lying fake): stay valid and silent.
        renderActive.store (false, std::memory_order_relaxed);
        return;
    }

    innerInChannels = inner->getTotalNumInputChannels ();
    innerOutChannels = inner->getTotalNumOutputChannels ();

    if (innerOutChannels <= 0)
    {
        // No usable output — treat as silent rather than dereferencing nothing.
        renderActive.store (false, std::memory_order_relaxed);
        return;
    }

    // Fast path: a 0-in / 2-out instrument renders straight into the graph buffer.
    fastStereoPath = (innerInChannels == 0 && innerOutChannels == 2);
    renderActive.store (true, std::memory_order_relaxed);
}

//==============================================================================
void HostedPluginNode::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    // MESSAGE-THREAD ONLY. Re-prepare the inner only when SR/block actually change
    // (§6.2 adopts the already-prepared instance). Always (re)arm smoothers/scratch.
    if (inner != nullptr)
    {
        const bool changed = ! juce::exactlyEqual (sampleRate, lastPreparedSampleRate)
                          || (maximumExpectedSamplesPerBlock != lastPreparedBlockSize);

        if (changed)
        {
            inner->setRateAndBufferSizeDetails (sampleRate, maximumExpectedSamplesPerBlock);
            inner->prepareToPlay (sampleRate, maximumExpectedSamplesPerBlock);
            lastPreparedSampleRate = sampleRate;
            lastPreparedBlockSize = maximumExpectedSamplesPerBlock;
        }

        // Forward the (possibly updated) latency to the graph.
        setLatencySamples (inner->getLatencySamples ());
    }

    // Arm the ramps. Fade seeds at its resting target so a node the coordinator has
    // pre-armed with fadeOut() starts silent (→ later fadeIn() ramps up click-free).
    fadeGain.reset (sampleRate, kFadeSeconds);
    inputTrim.reset (sampleRate, kTrimSeconds);
    outputTrim.reset (sampleRate, kTrimSeconds);

    const bool silent = bypassed.load (std::memory_order_relaxed)
                     || fadedOut.load (std::memory_order_relaxed);
    fadeGain.setCurrentAndTargetValue (silent ? 0.0f : 1.0f);
    inputTrim.setCurrentAndTargetValue (inputGainTarget.load (std::memory_order_relaxed));
    outputTrim.setCurrentAndTargetValue (outputGainTarget.load (std::memory_order_relaxed));

    fadeOutComplete.store (silent, std::memory_order_release);

    // Preallocate the non-fast-path scratch (message-thread alloc). Sized to the
    // widest channel count the inner needs so the audio path never grows it.
    scratchChannels = jmax (2, innerInChannels, innerOutChannels);
    scratch.setSize (scratchChannels, jmax (1, maximumExpectedSamplesPerBlock), false, false, true);
}

void HostedPluginNode::releaseResources ()
{
    // MESSAGE-THREAD ONLY.
    if (inner != nullptr)
        inner->releaseResources ();

    lastPreparedSampleRate = 0.0;
    lastPreparedBlockSize = 0;
}

//==============================================================================
void HostedPluginNode::renderInner (juce::AudioBuffer<float>& buffer,
                                    juce::MidiBuffer& midiMessages,
                                    int numSamples) noexcept
{
    // RT-SAFE. Precondition: renderActive == true and inner != nullptr.
    if (fastStereoPath)
    {
        // 0-in / 2-out: clear (instruments must not accumulate) and render in place.
        buffer.clear ();
        inner->processBlock (buffer, midiMessages);
        return;
    }

    // General path via the preallocated scratch. Wrapping construction only — no
    // allocation. `chans` never exceeds scratchChannels (set in prepareToPlay).
    const int chans = jmax (innerInChannels, innerOutChannels);
    juce::AudioBuffer<float> work (scratch.getArrayOfWritePointers (), chans, numSamples);
    work.clear ();

    // Input trim seam: feed the inner's input channels (FX only — a 0-input synth
    // skips this and sees silence).
    if (innerInChannels > 0)
    {
        for (int ch = 0; ch < jmin (innerInChannels, buffer.getNumChannels ()); ++ch)
            work.copyFrom (ch, 0, buffer, ch, 0, numSamples);
        inputTrim.applyGain (work, numSamples);
    }

    inner->processBlock (work, midiMessages);

    // Fan the inner output out to our stereo bus.
    if (innerOutChannels >= 2)
    {
        buffer.copyFrom (0, 0, work, 0, 0, numSamples);
        buffer.copyFrom (1, 0, work, 1, 0, numSamples);
    }
    else // mono → duplicate.
    {
        buffer.copyFrom (0, 0, work, 0, 0, numSamples);
        buffer.copyFrom (1, 0, work, 0, 0, numSamples);
    }
}

void HostedPluginNode::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // RT-SAFE: denormal guard first; no alloc/lock/log in this wrapper's own code.
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples ();

    // Inner render (failure-isolated: a null / un-negotiated inner emits silence).
    if (renderActive.load (std::memory_order_relaxed) && inner != nullptr)
        renderInner (buffer, midiMessages, numSamples);
    else
        buffer.clear ();

    // Output trim (block-rate smoothed).
    outputTrim.setTargetValue (outputGainTarget.load (std::memory_order_relaxed));
    outputTrim.applyGain (buffer, numSamples);

    // Soft-bypass crossfade + swap fade share one ramp: silent when bypassed OR
    // faded out. This is the click-free swap ramp the coordinator drives.
    const bool wantSilent = bypassed.load (std::memory_order_relaxed)
                         || fadedOut.load (std::memory_order_relaxed);
    fadeGain.setTargetValue (wantSilent ? 0.0f : 1.0f);
    fadeGain.applyGain (buffer, numSamples);

    // Swap handshake: the node is "fade-out complete" once the ramp has reached 0
    // and stopped moving — i.e. the output is genuinely silent.
    const bool silentNow = wantSilent
                        && ! fadeGain.isSmoothing ()
                        && (fadeGain.getCurrentValue () <= kSilenceEpsilon);
    fadeOutComplete.store (silentNow, std::memory_order_release);

    // Output NaN/Inf scrub — UNCONDITIONAL (issue #3 mitigation). Runs on the
    // synth's output before it reaches the Master limiter downstream.
    for (int ch = 0; ch < buffer.getNumChannels (); ++ch)
    {
        auto* samples = buffer.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i)
            samples[i] = std::isfinite (samples[i]) ? samples[i] : 0.0f;
    }
}

//==============================================================================
void HostedPluginNode::setBypassed (bool shouldBypass) noexcept
{
    bypassed.store (shouldBypass, std::memory_order_relaxed);
}

void HostedPluginNode::fadeOut () noexcept
{
    fadedOut.store (true, std::memory_order_relaxed);
}

void HostedPluginNode::fadeIn () noexcept
{
    fadedOut.store (false, std::memory_order_relaxed);
}

void HostedPluginNode::setOutputGain (float linearGain) noexcept
{
    outputGainTarget.store (linearGain, std::memory_order_relaxed);
}

void HostedPluginNode::setInputGain (float linearGain) noexcept
{
    inputGainTarget.store (linearGain, std::memory_order_relaxed);
}

//==============================================================================
double HostedPluginNode::getTailLengthSeconds () const
{
    return inner != nullptr ? inner->getTailLengthSeconds () : 0.0;
}

bool HostedPluginNode::producesMidi () const
{
    return inner != nullptr && inner->producesMidi ();
}

int HostedPluginNode::getNumPrograms ()
{
    return inner != nullptr ? inner->getNumPrograms () : 1;
}

int HostedPluginNode::getCurrentProgram ()
{
    return inner != nullptr ? inner->getCurrentProgram () : 0;
}

void HostedPluginNode::setCurrentProgram (int index)
{
    if (inner != nullptr)
        inner->setCurrentProgram (index);
}

const juce::String HostedPluginNode::getProgramName (int index)
{
    return inner != nullptr ? inner->getProgramName (index) : juce::String {};
}

void HostedPluginNode::changeProgramName (int index, const juce::String& newName)
{
    if (inner != nullptr)
        inner->changeProgramName (index, newName);
}

void HostedPluginNode::getStateInformation (juce::MemoryBlock& destData)
{
    // Opaque blob — captured, never parsed (Phase 11 may also read the inner
    // directly via getWrappedInstance()).
    if (inner != nullptr)
        inner->getStateInformation (destData);
}

void HostedPluginNode::setStateInformation (const void* data, int sizeInBytes)
{
    if (inner != nullptr)
        inner->setStateInformation (data, sizeInBytes);
}

//==============================================================================
bool HostedPluginNode::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Fixed instrument layout: no audio input, stereo output.
    if (layouts.getMainInputChannels () != 0)
        return false;

    return layouts.getMainOutputChannelSet () == AudioChannelSet::stereo ();
}

//==============================================================================
void HostedPluginNode::audioProcessorChanged (juce::AudioProcessor* processor,
                                              const juce::AudioProcessorListener::ChangeDetails& details)
{
    // Mirror the inner's latency onto the graph whenever it changes (§6.3 B5).
    // setLatencySamples auto-fires updateHostDisplay(withLatencyChanged) → the graph
    // listener → triggerAsyncUpdate, which ultimately takes a CriticalSection and may
    // reallocate the system message queue. That is fine on the message thread, but a
    // hostile inner can call updateHostDisplay() from its OWN processBlock, landing us
    // here on the AUDIO THREAD (W3). We must not lock/alloc there — so the only
    // audio-thread action is a single lock-free atomic store; the owner drains the
    // flag on the message thread via pollPendingLatencyChange().
    if (processor != inner.get () || inner == nullptr || ! details.latencyChanged)
        return;

    if (juce::MessageManager::existsAndIsCurrentThread ())
        setLatencySamples (inner->getLatencySamples ()); // Message thread: direct.
    else
        latencyDirty.store (true, std::memory_order_release); // Audio thread: flag only.
}

void HostedPluginNode::pollPendingLatencyChange () noexcept
{
    // MESSAGE-THREAD ONLY. Drain the flag set by a non-message-thread
    // audioProcessorChanged. Taking the lock / allocating in setLatencySamples is
    // safe here. Coalesced: any number of audio-thread flags collapse into one
    // propagation of the inner's current latency.
    if (latencyDirty.exchange (false, std::memory_order_acquire) && inner != nullptr)
        setLatencySamples (inner->getLatencySamples ());
}

void HostedPluginNode::audioProcessorParameterChanged (juce::AudioProcessor*, int, float)
{
    // Not needed here; parameter enumeration/attachment for the mod matrix is
    // Phase 14. Present only to satisfy the AudioProcessorListener interface.
}
} // namespace arpbox::hosting
