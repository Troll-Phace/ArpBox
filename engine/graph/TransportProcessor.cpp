#include "TransportProcessor.h"

namespace arpbox::engine
{
namespace
{
    // Reserved capacity (bytes) for the outgoing MidiBuffer, ensured unconditionally
    // every block (ensureSize early-returns — no allocation — once capacity
    // suffices). This node emits nothing today, but it is the HEAD of the MIDI chain
    // and the buffer it is handed is a pooled render-sequence buffer that carries the
    // chain's events downstream, so warming it here costs nothing and pre-sizes the
    // buffer for the sequencer node arriving in task 5.2.
    //
    // UNCONDITIONAL on purpose (same rationale as MidiInputProcessor): the graph can
    // hand us a FRESH, small pool buffer after a render-sequence rebuild (e.g. a
    // synth swap), and re-checking every block re-warms that new buffer — closing the
    // post-rebuild gap a first-block-only flag would miss.
    constexpr int outgoingWarmupBytes = 1024;
} // namespace

TransportProcessor::TransportProcessor ()
    : juce::AudioProcessor (BusesProperties ()) // MIDI-only: no audio buses
{
}

// MESSAGE-THREAD ONLY:
void TransportProcessor::setSharedState (EngineCommandQueue* commands,
                                        Transport* transportToDrive) noexcept
{
    commandQueue = commands;
    transport = transportToDrive;
}

// MESSAGE-THREAD ONLY:
void TransportProcessor::setCommandSinks (
    std::array<ICommandSink*, maxCommandSinks> sinksToUse) noexcept
{
    sinks = sinksToUse;
}

// MESSAGE-THREAD ONLY:
void TransportProcessor::prepareToPlay (double sampleRate, int)
{
    if (transport != nullptr)
        transport->prepare (sampleRate);
}

// RT-SAFE:
void TransportProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // MIDI-only node: no audio to render. Clear the (zero-channel) buffer defensively.
    buffer.clear ();

    // Capacity warm-up for the MIDI chain's pooled buffer (see the note above). We
    // deliberately do NOT clear `midi` — the ordering edge to the MIDI-In node is a
    // dependency, not a data path, and swallowing upstream events would be a defect.
    midi.ensureSize (outgoingWarmupBytes);

    // 1. Drain UI→engine commands ONCE (ARCHITECTURE §4 step 1). This node is the
    //    SOLE consumer of the strict-SPSC queue; each command is fanned out to every
    //    registered sink in registration order. The lambda is invoked in place by
    //    SpscFifo::drain (no std::function), and every sink is contractually RT-safe.
    if (commandQueue != nullptr)
    {
        commandQueue->drain (
            [this] (const EngineCommand& command) noexcept
            {
                for (auto* sink : sinks)
                    if (sink != nullptr)
                        sink->applyCommand (command);
            });
    }

    // 2. Advance the transport and latch this block's musical position (§4 step 2).
    //    MUST come after the drain: play/stop/locate/tempo commands consumed above
    //    are what this latch publishes, which is how a tempo change lands exactly on
    //    a block boundary.
    if (transport != nullptr)
        transport->beginBlock (buffer.getNumSamples ());
}

// RT-SAFE:
void TransportProcessor::processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer&)
{
    // The root graph runs in single precision; this must never be invoked.
    jassertfalse; // graph is single precision
    buffer.clear ();
}

bool TransportProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // MIDI-only: accept only the empty (no main audio in/out) layout.
    return layouts.getMainInputChannelSet ().isDisabled ()
        && layouts.getMainOutputChannelSet ().isDisabled ();
}
} // namespace arpbox::engine
