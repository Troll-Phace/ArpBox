// ─────────────────────────────────────────────────────────────────────────────
// midi_render_harness — tests for tests/support/MidiRenderHarness.h itself
// (INSTRUCTIONS Phase 5.3a). The harness is the foundation the Phase-6 golden-MIDI
// determinism suite is built on, so its own arithmetic has to be pinned down FIRST:
// an off-by-a-block bug in `blockBase + meta.samplePosition` would silently make
// every future golden wrong in the same direction, and therefore self-consistent and
// invisible.
//
// What is asserted here:
//   • Absolute positions are exactly blockBase + within-block offset.
//   • The SAME absolute-sample schedule renders BYTE-IDENTICALLY at 32/128/512/2048
//     samples per block — the Phase 5.3 property the two pre-existing MIDI idioms
//     (a discarded MidiBuffer; a dropped samplePosition) structurally could not
//     express, and the reason this header exists.
//   • The canonical byte stream matches iff the performances match (golden target).
//   • firstDifference/describeDifference actually locate a divergence.
//   • The bars()/samples() block arithmetic.
//   • MidiCaptureNode taps MIDI generated INSIDE a real EngineGraph at absolute
//     positions, without growing its storage.
// ─────────────────────────────────────────────────────────────────────────────

#include "support/MidiRenderHarness.h"

#include "fakes/HostedSynthGraphSupport.h"

#include "engine/graph/EngineGraph.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_events/juce_events.h>

#include <cstdint>
#include <memory>
#include <vector>

using arpbox::engine::EngineGraph;
using arpbox::testing::kGraphBlockSize;
using arpbox::testing::kGraphSampleRate;
using arpbox::testing::MidiCaptureNode;
using arpbox::testing::MidiRenderConfig;
using arpbox::testing::MidiRenderResult;
using arpbox::testing::pushNoteOn;
using arpbox::testing::renderProcessor;
using arpbox::testing::settleGraphEdits;
using arpbox::testing::TimedMidiEvent;

namespace
{
constexpr double kSampleRate = 48000.0;

/** Emits one note-on at each ABSOLUTE sample in a fixed schedule, whatever block
    size it is rendered at. Deliberately absolute-timed: that is what makes it a
    valid probe for buffer-size independence (a node that emitted "at offset 3 of
    every block" would be block-size DEPENDENT by construction and could not
    distinguish a correct harness from a broken one). */
class ScheduledMidiSource final : public juce::AudioProcessor
{
public:
    explicit ScheduledMidiSource (std::vector<std::int64_t> absoluteSamples)
        : juce::AudioProcessor (BusesProperties ().withOutput ("Output", juce::AudioChannelSet::stereo (), true))
        , schedule (std::move (absoluteSamples))
    {
    }

    const juce::String getName () const override { return "ScheduledMidiSource"; }
    void prepareToPlay (double, int) override { rewind (); }
    void releaseResources () override {}

    /** Restarts the schedule at absolute sample 0. */
    void rewind () noexcept
    {
        position = 0;
        nextIndex = 0;
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        buffer.clear ();

        const auto blockEnd = position + static_cast<std::int64_t> (buffer.getNumSamples ());

        while (nextIndex < schedule.size () && schedule[nextIndex] < blockEnd)
        {
            const auto offset = schedule[nextIndex] - position;
            if (offset >= 0)
                midi.addEvent (juce::MidiMessage::noteOn (1,
                                                          60 + static_cast<int> (nextIndex % 12u),
                                                          static_cast<juce::uint8> (100)),
                               static_cast<int> (offset));
            ++nextIndex;
        }

        position = blockEnd;
    }

    void processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) override { buffer.clear (); }

    juce::AudioProcessorEditor* createEditor () override { return nullptr; }
    bool hasEditor () const override { return false; }
    bool acceptsMidi () const override { return false; }
    bool producesMidi () const override { return true; }
    double getTailLengthSeconds () const override { return 0.0; }
    int getNumPrograms () override { return 1; }
    int getCurrentProgram () override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    std::vector<std::int64_t> schedule;
    std::int64_t position = 0;
    std::size_t nextIndex = 0;
};

/** Passes any input MIDI straight through, so a hook-injected event is collected. */
class MidiPassThrough final : public juce::AudioProcessor
{
public:
    MidiPassThrough ()
        : juce::AudioProcessor (BusesProperties ().withOutput ("Output", juce::AudioChannelSet::stereo (), true))
    {
    }

    const juce::String getName () const override { return "MidiPassThrough"; }
    void prepareToPlay (double, int) override {}
    void releaseResources () override {}
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override { buffer.clear (); }
    void processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) override { buffer.clear (); }
    juce::AudioProcessorEditor* createEditor () override { return nullptr; }
    bool hasEditor () const override { return false; }
    bool acceptsMidi () const override { return true; }
    bool producesMidi () const override { return true; }
    double getTailLengthSeconds () const override { return 0.0; }
    int getNumPrograms () override { return 1; }
    int getCurrentProgram () override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
};

const std::vector<std::int64_t> kSchedule { 0, 1, 127, 128, 129, 511, 4096, 8191 };
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Absolute-position accounting
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("harness/midi: events are collected at absolute sample positions", "[unit]")
{
    ScheduledMidiSource source { kSchedule };
    const auto config = MidiRenderConfig::samples (8192, kSampleRate, 128);
    source.prepareToPlay (config.sampleRate, config.blockSize);

    const auto result = renderProcessor (source, config);
    INFO (result.describe ());

    REQUIRE (result.numBlocks == 64); // 8192 / 128
    REQUIRE (result.numSamples == 8192);
    REQUIRE (result.size () == kSchedule.size ());
    REQUIRE (result.isSampleSorted ());

    // The whole point: within-block offsets have been lifted into absolute positions.
    for (std::size_t i = 0; i < kSchedule.size (); ++i)
    {
        INFO ("event " << i << ": " << result[i].describe ());
        REQUIRE (result[i].absoluteSample == kSchedule[i]);
        REQUIRE (result[i].message.isNoteOn ());
    }

    // Sanity on the arithmetic direction: an event at 129 is in block 1 at offset 1,
    // so a harness that forgot blockBase would report 1, and one that double-counted
    // would report 257.
    REQUIRE (result[4].absoluteSample == 129);
}

TEST_CASE ("harness/midi: an absolute schedule renders byte-identically at every block size", "[determinism]")
{
    // THE Phase 5.3 property, at the harness level: identical event sample positions
    // across 32…2048-sample blocks. Every later golden comparison leans on this being
    // true of the harness itself, not just of the engine.
    MidiRenderResult reference;
    std::vector<std::uint8_t> referenceBytes;

    for (const int blockSize : { 32, 128, 512, 2048 })
    {
        ScheduledMidiSource source { kSchedule };
        const auto config = MidiRenderConfig::samples (8192, kSampleRate, blockSize);
        source.prepareToPlay (config.sampleRate, config.blockSize);

        const auto result = renderProcessor (source, config);
        REQUIRE (result.numSamples == 8192);
        REQUIRE (result.size () == kSchedule.size ());

        if (reference.empty ())
        {
            reference = result;
            referenceBytes = result.toByteStream ();
            REQUIRE (! referenceBytes.empty ());
        }
        else
        {
            INFO ("block size " << blockSize << "\n" << reference.describeDifference (result));
            REQUIRE (result == reference);                      // byte-for-byte events
            REQUIRE (result.toByteStream () == referenceBytes); // and canonical stream
        }
    }
}

TEST_CASE ("harness/midi: the canonical byte stream distinguishes different performances", "[determinism]")
{
    // A golden target that compared equal for DIFFERENT performances would be worse
    // than no golden at all, so pin both directions.
    ScheduledMidiSource sourceA { kSchedule };
    ScheduledMidiSource sourceB { { 0, 1, 127, 128, 130, 511, 4096, 8191 } }; // 129 → 130

    const auto config = MidiRenderConfig::samples (8192, kSampleRate, 128);
    sourceA.prepareToPlay (config.sampleRate, config.blockSize);
    sourceB.prepareToPlay (config.sampleRate, config.blockSize);

    const auto a = renderProcessor (sourceA, config);
    const auto b = renderProcessor (sourceB, config);

    REQUIRE (a.size () == b.size ());
    REQUIRE (a != b);
    REQUIRE (a.toByteStream () != b.toByteStream ());

    // ...and the diagnostic actually locates the divergence — a one-sample timing
    // difference 4 events into a 200-event stream is precisely the failure the golden
    // suite must be able to read.
    const auto diff = a.firstDifference (b);
    REQUIRE (diff.has_value ());
    REQUIRE (*diff == 4u);

    const auto report = a.describeDifference (b);
    INFO (report);
    REQUIRE (report.contains ("first difference at index 4"));
    REQUIRE (report.contains ("@129"));
    REQUIRE (report.contains ("@130"));

    // Identical renders report no difference at all.
    REQUIRE (! a.firstDifference (a).has_value ());
    REQUIRE (a.describeDifference (a).contains ("BYTE-IDENTICAL"));
}

TEST_CASE ("harness/midi: a shorter stream that is a prefix of a longer one differs at the end", "[determinism]")
{
    ScheduledMidiSource full { kSchedule };
    ScheduledMidiSource truncated { { 0, 1, 127 } };

    const auto config = MidiRenderConfig::samples (8192, kSampleRate, 128);
    full.prepareToPlay (config.sampleRate, config.blockSize);
    truncated.prepareToPlay (config.sampleRate, config.blockSize);

    const auto a = renderProcessor (full, config);
    const auto b = renderProcessor (truncated, config);

    REQUIRE (a != b);
    const auto diff = a.firstDifference (b);
    REQUIRE (diff.has_value ());
    REQUIRE (*diff == 3u); // the length of the shorter stream
    REQUIRE (a.describeDifference (b).contains ("<end of stream>"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Config arithmetic
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("harness/midi: bar and sample configs compute the right block counts", "[unit]")
{
    // 2 bars of 4/4 at 120 BPM = 8 quarters = 4 s = 192 000 samples at 48 kHz.
    REQUIRE (MidiRenderConfig::samplesForBars (2.0, 120.0, 48000.0) == 192000);

    const auto twoBars = MidiRenderConfig::bars (2.0, 120.0, 48000.0, 128);
    REQUIRE (twoBars.numBlocks == 1500);
    REQUIRE (twoBars.totalSamples () == 192000);

    // Rounds UP to a whole block, never truncating musical time away.
    REQUIRE (MidiRenderConfig::numBlocksForSamples (1, 128) == 1);
    REQUIRE (MidiRenderConfig::numBlocksForSamples (128, 128) == 1);
    REQUIRE (MidiRenderConfig::numBlocksForSamples (129, 128) == 2);
    REQUIRE (MidiRenderConfig::numBlocksForSamples (0, 128) == 0);

    // A non-integer bar length at an awkward tempo still covers at least the span.
    const auto odd = MidiRenderConfig::bars (1.0, 137.0, 44100.0, 512);
    REQUIRE (odd.totalSamples () >= MidiRenderConfig::samplesForBars (1.0, 137.0, 44100.0));
    REQUIRE (odd.totalSamples () - MidiRenderConfig::samplesForBars (1.0, 137.0, 44100.0) < 512);
}

TEST_CASE ("harness/midi: the block hook can inject input MIDI at a chosen block", "[unit]")
{
    // The hook is how Phase-6 tests will push transport/pattern commands mid-render;
    // prove it lands in the block it was given, and that injected input is collected
    // at the right absolute position.
    MidiPassThrough node;
    const auto config = MidiRenderConfig::blocks (8, kSampleRate, 128);
    node.prepareToPlay (config.sampleRate, config.blockSize);

    std::vector<std::int64_t> seenBases;

    const auto result = renderProcessor (
        node,
        config,
        [&] (const arpbox::testing::RenderBlockContext& context)
        {
            seenBases.push_back (context.blockBase);

            if (context.blockIndex == 3)
                context.midi->addEvent (juce::MidiMessage::noteOn (1, 64, static_cast<juce::uint8> (99)), 17);
        });

    REQUIRE (seenBases.size () == 8u);
    for (int i = 0; i < 8; ++i)
        REQUIRE (seenBases[static_cast<std::size_t> (i)] == static_cast<std::int64_t> (i) * 128);

    REQUIRE (result.size () == 1u);
    REQUIRE (result[0].absoluteSample == 3 * 128 + 17);
    REQUIRE (result[0].message.isNoteOn ());
    REQUIRE (result[0].message.getNoteNumber () == 64);
}

// ─────────────────────────────────────────────────────────────────────────────
// Graph-internal tap
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("harness/graph: MidiCaptureNode records graph-internal MIDI at absolute positions", "[hosting-lab]")
{
    // The second render idiom: MIDI generated INSIDE the graph (here by the MIDI-In
    // node draining the QWERTY/pad FIFO) never reaches the caller's MidiBuffer, so it
    // is captured from within the render sequence instead. Phase 6 will use exactly
    // this shape to capture what the sequencer node hands the synth.
    EngineGraph graph;
    graph.prepareToPlay (kGraphSampleRate, kGraphBlockSize);

    auto captureOwner = std::make_unique<MidiCaptureNode> ();
    captureOwner->setPlayConfigDetails (0, 2, kGraphSampleRate, kGraphBlockSize);
    captureOwner->prepareToPlay (kGraphSampleRate, kGraphBlockSize);
    auto* capture = captureOwner.get ();

    graph.setSynth (std::move (captureOwner));
    settleGraphEdits ();

    juce::AudioBuffer<float> buffer (2, kGraphBlockSize);
    juce::MidiBuffer midi;

    // Warm up so the async insertion is definitely live, then rebase to sample 0.
    for (int i = 0; i < 2; ++i)
    {
        buffer.clear ();
        graph.getProcessor ().processBlock (buffer, midi);
    }
    REQUIRE (capture->blocksRendered () == 2);
    capture->resetCapture ();

    // A note pushed before block i is drained by the MIDI-In node in block i and
    // emitted at offset 0 (MidiInputProcessor), so it must land at absolute i*block.
    constexpr int blocks = 6;
    for (int i = 0; i < blocks; ++i)
    {
        if (i == 1 || i == 3)
            pushNoteOn (graph, 60 + i, 100);

        buffer.clear ();
        graph.getProcessor ().processBlock (buffer, midi);
    }

    const auto& captured = capture->result ();
    INFO (captured.describe ());

    REQUIRE (capture->droppedEvents () == 0); // never grew, never silently truncated
    REQUIRE (capture->blocksRendered () == blocks);
    REQUIRE (captured.numSamples == static_cast<std::int64_t> (blocks) * kGraphBlockSize);
    REQUIRE (captured.isSampleSorted ());

    const auto notes = captured.select ([] (const TimedMidiEvent& event) { return event.message.isNoteOn (); });
    REQUIRE (notes.size () == 2u);
    REQUIRE (notes[0].absoluteSample == 1 * kGraphBlockSize);
    REQUIRE (notes[0].message.getNoteNumber () == 61);
    REQUIRE (notes[1].absoluteSample == 3 * kGraphBlockSize);
    REQUIRE (notes[1].message.getNoteNumber () == 63);

    graph.removeSynth ();
    settleGraphEdits ();
}
