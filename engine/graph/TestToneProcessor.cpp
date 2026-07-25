#include "TestToneProcessor.h"

#include <cmath>

namespace arpbox::engine
{
namespace
{
    constexpr double twoPi = 2.0 * juce::MathConstants<double>::pi;

    // Clamp the control frequency to an audible, safe range before synthesis so a
    // stray command value can never produce a DC term or an aliasing scream.
    constexpr float minToneHz = 20.0f;
    constexpr float maxToneHz = 20000.0f;
} // namespace

TestToneProcessor::TestToneProcessor ()
    : juce::AudioProcessor (BusesProperties ().withOutput ("Output", juce::AudioChannelSet::stereo (), true))
{
}

// MESSAGE-THREAD ONLY:
void TestToneProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    phase = 0.0;
}

// RT-SAFE:
void TestToneProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Source node: start from silence every block so an unconnected/disabled tone
    // never leaks stale samples downstream.
    buffer.clear ();

    // No control block wired, or tone disabled → leave silence.
    if (toneControl == nullptr || ! toneControl->enabled.load (std::memory_order_relaxed))
        return;

    const int numChannels = buffer.getNumChannels ();
    const int numSamples = buffer.getNumSamples ();
    if (numChannels <= 0 || numSamples <= 0)
        return;

    const float freqHz = juce::jlimit (minToneHz, maxToneHz, toneControl->frequencyHz.load (std::memory_order_relaxed));
    const double increment = twoPi * static_cast<double> (freqHz) / currentSampleRate;

    // Synthesise once into channel 0, then copy to the rest — one sin() per sample.
    float* const first = buffer.getWritePointer (0);
    double localPhase = phase;

    for (int i = 0; i < numSamples; ++i)
    {
        first[i] = static_cast<float> (std::sin (localPhase)) * toneLevel;
        localPhase += increment;
        if (localPhase >= twoPi)
            localPhase -= twoPi;
    }

    for (int ch = 1; ch < numChannels; ++ch)
        buffer.copyFrom (ch, 0, first, numSamples);

    phase = localPhase; // persist for phase-continuous synthesis across blocks
}

// RT-SAFE:
void TestToneProcessor::processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer&)
{
    // The root graph runs in single precision; this must never be invoked.
    jassertfalse;
    buffer.clear ();
}

bool TestToneProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // No input bus; stereo output only.
    if (layouts.getMainInputChannels () != 0)
        return false;

    return layouts.getMainOutputChannelSet () == juce::AudioChannelSet::stereo ();
}
} // namespace arpbox::engine
