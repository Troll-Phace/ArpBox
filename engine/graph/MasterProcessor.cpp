#include "MasterProcessor.h"

#include "EngineSnapshot.h"

#include <cmath>

namespace arpbox::engine
{
namespace
{
    // Safety-limiter defaults: a hair below 0 dBFS so the internal hard clip at
    // 0 dB is a true last resort, with a musical release for resonant spikes.
    constexpr float limiterThresholdDb = -1.0f;
    constexpr float limiterReleaseMs = 100.0f;

    // Gain smoothing time — click-free response to master-gain commands.
    constexpr double gainSmoothingSeconds = 0.02;
} // namespace

MasterProcessor::MasterProcessor ()
    : juce::AudioProcessor (BusesProperties ()
                                .withInput ("Input", juce::AudioChannelSet::stereo (), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo (), true))
{
}

// MESSAGE-THREAD ONLY:
void MasterProcessor::setSharedState (EngineSnapshotBuffer* snapshots, ToneControl* tone) noexcept
{
    snapshotBuffer = snapshots;
    toneControl = tone;
}

// MESSAGE-THREAD ONLY:
void MasterProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    const auto sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    const auto blockSize = static_cast<juce::uint32> (juce::jmax (1, maximumExpectedSamplesPerBlock));

    const juce::dsp::ProcessSpec spec { sr, blockSize, 2 };

    outputGain.prepare (spec);
    outputGain.setRampDurationSeconds (gainSmoothingSeconds);
    outputGain.setGainLinear (1.0f); // unity until a setMasterGainDb command arrives

    limiter.prepare (spec);
    limiter.setThreshold (limiterThresholdDb);
    limiter.setRelease (limiterReleaseMs);
    limiter.reset ();
}

// RT-SAFE:
void MasterProcessor::applyCommand (const EngineCommand& command) noexcept
{
    switch (command.type)
    {
    case EngineCommandType::setMasterGainDb:
        // Sanitize the target gain (issue #3 residual). A non-finite dB target
        // makes dsp::Gain emit NaN AFTER the input scrub and BEFORE the limiter,
        // re-poisoning the limiter's ballistics into permanent silence. Drop a
        // non-finite value (keep the current gain); otherwise clamp to a sane
        // master range before applying. isfinite + jlimit only — no alloc/lock.
        if (std::isfinite (command.value.f))
            outputGain.setGainDecibels (juce::jlimit (-100.0f, 24.0f, command.value.f)); // smoothed to target
        break;

    case EngineCommandType::setLimiterEnabled:
        limiterEnabled = command.value.i != 0;
        break;

    case EngineCommandType::setTestToneEnabled:
        if (toneControl != nullptr)
            toneControl->enabled.store (command.value.i != 0, std::memory_order_relaxed);
        break;

    case EngineCommandType::setTestToneFrequency:
        // Drop a non-finite frequency (issue #3 residual): the tone node's own
        // [20, 20000] clamp is jlimit-based, and jlimit(NaN) is ill-defined.
        if (toneControl != nullptr && std::isfinite (command.value.f))
            toneControl->frequencyHz.store (command.value.f, std::memory_order_relaxed);
        break;

    case EngineCommandType::none:
    case EngineCommandType::transportPlay:
    case EngineCommandType::transportStop:
    case EngineCommandType::transportLocate:
    case EngineCommandType::setTempoBpm:
    default:
        break; // not ours (Transport owns the transport commands) — ignore
    }
}

// RT-SAFE:
void MasterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // NOTE (Phase 5.1): there is NO command drain here any more. `TransportProcessor`
    // is the queue's single consumer and has already called this node's
    // `applyCommand` for every command in this block, earlier in this same callback.
    // Adding a second `drain()` here would split the SPSC stream between two
    // consumers — each command would reach only one of them.

    // 1b. Graph-latency reporting. Poll the source (the root graph) and emit a
    //     latencyChanged event ONLY on a change. The audio thread is the sole
    //     producer of EngineEventQueue (EngineEvent.h), so this must happen here,
    //     not on the message thread that edits topology. Reading getLatencySamples()
    //     is a lock-free int read; a one-block-stale value is fine for a UI badge.
    if (latencySource != nullptr && eventQueue != nullptr)
    {
        const std::int32_t latency = latencySource->getLatencySamples ();
        if (latency != lastReportedLatency)
        {
            lastReportedLatency = latency;
            EngineEvent ev;
            ev.type = EngineEventType::latencyChanged;
            ev.a = static_cast<std::uint32_t> (juce::jmax (0, latency));
            eventQueue->push (ev);
        }
    }

    const int numChannels = buffer.getNumChannels ();
    const int numSamples = buffer.getNumSamples ();

    // RT-SAFE:
    // 1c. INPUT NaN/Inf scrub (issue #3, defense-in-depth). A single non-finite
    //     input sample poisons dsp::Limiter's internal ballistics (the envelope
    //     goes NaN) and — because the graph-boundary scrub below runs AFTER the
    //     limiter — the master would stay PERMANENTLY silent until the next
    //     prepareToPlay. Scrub here, before gain/limiter, so the limiter never sees
    //     a non-finite sample and recovers on the next clean block. The post-limiter
    //     scrub (step 4) stays as the output guard. Per-sample std::isfinite; no
    //     alloc, no lock. Branch-free select.
    for (int ch = 0; ch < numChannels; ++ch)
    {
        float* const data = buffer.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i)
        {
            const float v = data[i];
            data[i] = std::isfinite (v) ? v : 0.0f;
        }
    }

    // 2. Output gain + 3. safety limiter, in place over the whole block.
    if (numChannels > 0 && numSamples > 0)
    {
        juce::dsp::AudioBlock<float> block (buffer);
        juce::dsp::ProcessContextReplacing<float> context (block);

        outputGain.process (context);

        if (limiterEnabled)
            limiter.process (context);
    }

    // 4. NaN/Inf scrub — THE graph-boundary scrub point. A non-finite sample
    //    reaching CoreAudio can wedge the driver; force every sample finite once,
    //    after gain+limiter, before metering and output. Branch-free select.
    for (int ch = 0; ch < numChannels; ++ch)
    {
        float* const data = buffer.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i)
        {
            const float v = data[i];
            data[i] = std::isfinite (v) ? v : 0.0f;
        }
    }

    // 5. Meter tap — POST-limiter/POST-scrub, per-block LINEAR peak + RMS. O(n),
    //    no per-sample atomics, no growing window.
    float peak[2] = { 0.0f, 0.0f };
    float rms[2] = { 0.0f, 0.0f };

    for (int ch = 0; ch < juce::jmin (numChannels, 2); ++ch)
    {
        const float* const data = buffer.getReadPointer (ch);
        float chPeak = 0.0f;
        double sumSquares = 0.0;

        for (int i = 0; i < numSamples; ++i)
        {
            const float v = data[i];
            const float a = std::abs (v);
            if (a > chPeak)
                chPeak = a;
            sumSquares += static_cast<double> (v) * static_cast<double> (v);
        }

        peak[ch] = chPeak;
        rms[ch] =
            numSamples > 0 ? static_cast<float> (std::sqrt (sumSquares / static_cast<double> (numSamples))) : 0.0f;
    }

    // Mono safety: mirror L→R so the UI shows matching meters if ever mono.
    if (numChannels == 1)
    {
        peak[1] = peak[0];
        rms[1] = rms[0];
    }

    // 6. Publish the snapshot. Build a fresh, fully-defined snapshot so no stale
    //    slot data leaks. Transport fields (Phase 5.1) come from the injected
    //    Transport's LATCHED block-start values — the master renders after the
    //    transport head node, so these describe THIS block. The generative fields
    //    (seed) stay zero until Phase 12.
    ++blockCounter;
    if (snapshotBuffer != nullptr)
    {
        EngineSnapshot snap;
        if (transportSource != nullptr)
        {
            snap.ppqPosition = transportSource->blockStartPpq ();
            snap.bpm = transportSource->bpm ();
            snap.isPlaying = transportSource->isPlaying ();
        }
        snap.peakL = peak[0];
        snap.peakR = peak[1];
        snap.rmsL = rms[0];
        snap.rmsR = rms[1];
        snap.deviceStatus = deviceStatus.load (std::memory_order_relaxed);
        snap.voiceCount = voiceCountSource != nullptr ? voiceCountSource->load (std::memory_order_relaxed)
                                                      : static_cast<std::uint16_t> (0);
        snap.blockCounter = blockCounter;

        snapshotBuffer->beginWrite () = snap;
        snapshotBuffer->commit ();
    }
}

// RT-SAFE:
void MasterProcessor::processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer&)
{
    // The root graph runs in single precision; this must never be invoked. Mirror
    // TestToneProcessor's double path: assert for debug visibility, and clear the
    // buffer so a stray double-precision host cannot leak unscrubbed/unmetered
    // garbage through this node (the float path does the scrub + metering).
    jassertfalse; // graph is single precision
    buffer.clear ();
}

bool MasterProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet () == juce::AudioChannelSet::stereo () &&
           layouts.getMainOutputChannelSet () == juce::AudioChannelSet::stereo ();
}
} // namespace arpbox::engine
