// ─────────────────────────────────────────────────────────────────────────────
// hosted_node_unit — HostedPluginNode wrapper unit tests (docs/INSTRUCTIONS.md
// Phase 4.1/4.4; ARCHITECTURE §6.3). Drives the wrapper DIRECTLY (standalone
// AudioProcessor, no graph) so its OWN output buffer carries the result and can be
// asserted at sample resolution.
//
// Covers the four §6.3 wrapper guarantees the delegation calls out:
//   • Unconditional output NaN/Inf scrub (issue #3 boundary) — surgical, not blanket.
//   • Bus negotiation: bus-lying inner → silent-but-valid; mono inner → stereo-fanned.
//   • Latency forwarding: the inner's reported latency mirrors onto the wrapper.
//   • Fade handshake: fadeOut() → click-free ramp to silence + isFadeOutComplete();
//     fadeIn() restores.
// Plus a zero-allocation steady-state guard on the wrapper's own processBlock path
// (tagged [perf-budget] so it is excluded from the sanitizer `-L unit` runs — the
// operator-new override in AllocationCounter.cpp does not compose with ASan; see
// infra_alloc_guard.cpp for the same discipline).
//
// Fakes only (.claude/rules/testing.md): the baseline instrument fake, the corpus
// BusLyingFake / WrongLatencyFake, plus two TU-local fakes (NaN emitter, mono
// instrument) for behaviors the shared corpus does not yet expose.
// ─────────────────────────────────────────────────────────────────────────────

#include "fakes/FakePlugins.h"
#include "support/AllocationSentinel.h"

#include "hosting/HostedPluginNode.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>

using arpbox::hosting::HostedPluginNode;
using arpbox::test::AllocationSentinel;
using arpbox::testing::FakeBehavior;
using arpbox::testing::FakePluginInstance;
using arpbox::testing::makeFakeInstance;
using arpbox::testing::specFor;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 128;

// True if every sample in every channel is finite (no NaN / Inf).
bool allFinite (const juce::AudioBuffer<float>& buffer) noexcept
{
    for (int ch = 0; ch < buffer.getNumChannels (); ++ch)
    {
        const float* const d = buffer.getReadPointer (ch);
        for (int i = 0; i < buffer.getNumSamples (); ++i)
            if (! std::isfinite (d[i]))
                return false;
    }
    return true;
}

// Builds an already-prepared inner for `behavior`, wraps it, and prepares the
// wrapper at the standard rate/block — the ready-to-drive standalone node.
std::unique_ptr<HostedPluginNode> makePreparedNode (FakeBehavior behavior)
{
    auto inner = makeFakeInstance (specFor (behavior));
    inner->prepareToPlay (kSampleRate, kBlockSize); // ctor contract: adopt a prepared inner
    auto node = std::make_unique<HostedPluginNode> (std::move (inner));
    node->prepareToPlay (kSampleRate, kBlockSize);
    return node;
}

// A note-on the wrapper forwards to its inner so a well-behaved instrument sounds.
juce::MidiBuffer noteOnBuffer (int note = 60, int velocity = 100)
{
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) velocity), 0);
    return midi;
}

// ── TU-local fakes for behaviors the shared corpus does not expose ────────────

// Emits a deliberate mix of finite (0.5) and non-finite (NaN / Inf) samples every
// block. A 0-in / 2-out instrument so the wrapper takes the fast stereo path — the
// scrub must survive the fast path unconditionally.
class NanEmittingFake final : public FakePluginInstance
{
public:
    NanEmittingFake () : FakePluginInstance (specFor (FakeBehavior::baseline)) {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        for (int ch = 0; ch < buffer.getNumChannels (); ++ch)
        {
            float* const d = buffer.getWritePointer (ch);
            for (int i = 0; i < buffer.getNumSamples (); ++i)
            {
                if (i % 3 == 0)
                    d[i] = std::numeric_limits<float>::quiet_NaN ();
                else if (i % 3 == 1)
                    d[i] = std::numeric_limits<float>::infinity ();
                else
                    d[i] = 0.5f; // finite content that MUST survive the scrub
            }
        }
    }
};

// A minimal instrument with a MONO output bus that accepts ONLY mono — forcing the
// wrapper's mono→stereo adaptation path (the corpus baseline is stereo, so the
// wrapper would otherwise take the fast stereo path and never fan a mono channel).
class MonoInstrumentFake final : public juce::AudioPluginInstance
{
public:
    MonoInstrumentFake ()
        : juce::AudioPluginInstance (
              BusesProperties ().withOutput ("Output", juce::AudioChannelSet::mono (), true))
    {
    }

    const juce::String getName () const override { return "Mono Fake Synth"; }

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        // Only 0-in / mono-out: refusing stereo forces the wrapper to negotiate mono.
        return layouts.getMainInputChannels () == 0
            && layouts.getMainOutputChannelSet () == juce::AudioChannelSet::mono ();
    }

    void prepareToPlay (double, int) override {}
    void releaseResources () override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals noDenormals;
        for (const auto meta : midi)
        {
            const auto m = meta.getMessage ();
            if (m.isNoteOn ())
                ++voices;
            else if (m.isNoteOff ())
                voices = juce::jmax (0, voices - 1);
            else if (m.isAllNotesOff () || m.isAllSoundOff ())
                voices = 0;
        }

        const float amp = voices > 0 ? 0.25f : 0.0f;
        for (int ch = 0; ch < buffer.getNumChannels (); ++ch)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), amp, buffer.getNumSamples ());
    }

    double getTailLengthSeconds () const override { return 0.0; }
    bool acceptsMidi () const override { return true; }
    bool producesMidi () const override { return false; }
    juce::AudioProcessorEditor* createEditor () override { return nullptr; }
    bool hasEditor () const override { return false; }
    int getNumPrograms () override { return 1; }
    int getCurrentProgram () override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
    void fillInPluginDescription (juce::PluginDescription&) const override {}

private:
    int voices = 0;
};
} // namespace

//==============================================================================
TEST_CASE ("hosting/hosted-node: output NaN/Inf scrub keeps every sample finite", "[unit]")
{
    // issue #3 boundary guarantee: a NaN must never leave the wrapper toward the
    // Master limiter. The scrub must be SURGICAL — non-finite → 0, finite preserved.
    auto inner = std::make_unique<NanEmittingFake> ();
    inner->prepareToPlay (kSampleRate, kBlockSize);
    HostedPluginNode node (std::move (inner));
    node.prepareToPlay (kSampleRate, kBlockSize);

    juce::AudioBuffer<float> buffer (2, kBlockSize);
    juce::MidiBuffer midi;

    // Process a few blocks (unity default gain — no fade), asserting finiteness each.
    for (int i = 0; i < 4; ++i)
    {
        buffer.clear ();
        node.processBlock (buffer, midi);
        REQUIRE (allFinite (buffer));
    }

    // The finite 0.5 content survived the scrub (not blanket-zeroed): magnitude > 0.
    REQUIRE (buffer.getMagnitude (0, buffer.getNumSamples ()) > 0.0f);
}

TEST_CASE ("hosting/hosted-node: bus-lying inner degrades to silent-but-valid", "[unit]")
{
    // BusLyingFake refuses every layout → negotiation fails → renderActive == false.
    // The node must construct without crashing and emit finite silence forever.
    auto node = makePreparedNode (FakeBehavior::busLying);

    juce::AudioBuffer<float> buffer (2, kBlockSize);
    juce::MidiBuffer midi = noteOnBuffer (); // even with a note, it stays silent

    for (int i = 0; i < 8; ++i)
    {
        buffer.clear ();
        node->processBlock (buffer, midi);
        REQUIRE (allFinite (buffer));
        REQUIRE (buffer.getMagnitude (0, buffer.getNumSamples ()) == 0.0f);
    }
}

TEST_CASE ("hosting/hosted-node: mono inner is fanned to both stereo outputs", "[unit]")
{
    // The wrapper presents a stereo bus; a mono inner must be duplicated L==R.
    auto inner = std::make_unique<MonoInstrumentFake> ();
    inner->prepareToPlay (kSampleRate, kBlockSize);
    HostedPluginNode node (std::move (inner));
    node.prepareToPlay (kSampleRate, kBlockSize);

    juce::AudioBuffer<float> buffer (2, kBlockSize);

    // Strike a note on the first block; hold it (empty midi) on the rest.
    juce::MidiBuffer held = noteOnBuffer ();
    for (int i = 0; i < 6; ++i)
    {
        buffer.clear ();
        node.processBlock (buffer, held);
        held.clear (); // note-on only in the first block; the fake latches voices
        REQUIRE (allFinite (buffer));
    }

    const float l = buffer.getMagnitude (0, 0, buffer.getNumSamples ());
    const float r = buffer.getMagnitude (1, 0, buffer.getNumSamples ());
    REQUIRE (l > 0.0f);   // mono content reached the left output
    REQUIRE (r == l);     // ...and was duplicated to the right bit-for-bit (exact copy)
}

TEST_CASE ("hosting/hosted-node: latency mirrors the inner's reported value", "[unit]")
{
    // WrongLatencyFake reports 512 samples (a lie it never applies) — the wrapper's
    // job is only to FORWARD it so the graph's latency accounting is correct.
    auto node = makePreparedNode (FakeBehavior::wrongLatency);
    REQUIRE (node->getLatencySamples () == arpbox::testing::kWrongLatencySamples);

    // A well-behaved zero-latency inner forwards 0.
    auto zero = makePreparedNode (FakeBehavior::baseline);
    REQUIRE (zero->getLatencySamples () == 0);
}

TEST_CASE ("hosting/hosted-node: fade handshake ramps to silence click-free and restores", "[unit]")
{
    auto node = makePreparedNode (FakeBehavior::baseline);

    juce::AudioBuffer<float> buffer (2, kBlockSize);

    // Hold a note → steady audible DC (~0.25) at unity default gain.
    juce::MidiBuffer held = noteOnBuffer ();
    for (int i = 0; i < 8; ++i)
    {
        buffer.clear ();
        node->processBlock (buffer, held);
        held.clear ();
    }
    REQUIRE (node->getWrappedInstance () != nullptr);
    REQUIRE (node->isFadeOutComplete () == false);
    const float playingMag = buffer.getMagnitude (0, buffer.getNumSamples ());
    REQUIRE (playingMag > 0.1f);

    SECTION ("fadeOut ramps down click-free and reports completion, then fadeIn restores")
    {
        node->fadeOut ();

        // Advance the ramp, tracking the largest sample-to-sample delta on ch0 as a
        // click proxy at sample resolution. A discontinuity (click) would show up as
        // a large jump; a smoothed 10 ms ramp on a 0.25 DC source moves in tiny steps.
        constexpr int kBudgetBlocks = 32; // 10 ms fade ≈ 4 blocks; budget guards a hang
        float maxDelta = 0.0f;
        float prevLast = buffer.getSample (0, buffer.getNumSamples () - 1);
        bool completed = false;

        for (int b = 0; b < kBudgetBlocks && ! completed; ++b)
        {
            buffer.clear ();
            juce::MidiBuffer empty;
            node->processBlock (buffer, empty);
            REQUIRE (allFinite (buffer));

            const float* const d = buffer.getReadPointer (0);
            maxDelta = juce::jmax (maxDelta, std::abs (d[0] - prevLast));
            for (int i = 1; i < buffer.getNumSamples (); ++i)
                maxDelta = juce::jmax (maxDelta, std::abs (d[i] - d[i - 1]));
            prevLast = d[buffer.getNumSamples () - 1];

            completed = node->isFadeOutComplete ();
        }

        REQUIRE (completed);        // no hang
        REQUIRE (maxDelta < 0.05f); // click-free ramp (no discontinuity spike)

        // The completing block carries the ramp's silent tail; the NEXT block is
        // fully at gain 0. Assert genuine silence there.
        buffer.clear ();
        juce::MidiBuffer emptyAfter;
        node->processBlock (buffer, emptyAfter);
        REQUIRE (buffer.getMagnitude (0, buffer.getNumSamples ()) < 1.0e-4f);

        // fadeIn brings the (still-held) note back without a click.
        node->fadeIn ();
        for (int b = 0; b < 16; ++b)
        {
            buffer.clear ();
            juce::MidiBuffer empty;
            node->processBlock (buffer, empty);
            REQUIRE (allFinite (buffer));
        }
        REQUIRE (node->isFadeOutComplete () == false);
        REQUIRE (buffer.getMagnitude (0, buffer.getNumSamples ()) > 0.1f);
    }

    SECTION ("setBypassed silences the node and clears when un-bypassed")
    {
        node->setBypassed (true);
        for (int b = 0; b < 16; ++b)
        {
            buffer.clear ();
            juce::MidiBuffer empty;
            node->processBlock (buffer, empty);
            REQUIRE (allFinite (buffer));
        }
        REQUIRE (node->isBypassed ());
        REQUIRE (buffer.getMagnitude (0, buffer.getNumSamples ()) < 1.0e-4f);

        node->setBypassed (false);
        for (int b = 0; b < 16; ++b)
        {
            buffer.clear ();
            juce::MidiBuffer empty;
            node->processBlock (buffer, empty);
        }
        REQUIRE_FALSE (node->isBypassed ());
        REQUIRE (buffer.getMagnitude (0, buffer.getNumSamples ()) > 0.1f);
    }
}

//==============================================================================
// [perf-budget] — excluded from the sanitizer `-L unit` runs (the operator-new
// override does not compose with ASan; see infra_alloc_guard.cpp).
TEST_CASE ("hosting/hosted-node: zero allocations in steady-state processBlock", "[perf-budget]")
{
    auto node = makePreparedNode (FakeBehavior::baseline); // non-allocating inner

    // Everything the measured loop touches is pre-allocated before arming.
    juce::AudioBuffer<float> buffer (2, kBlockSize);
    juce::MidiBuffer held = noteOnBuffer ();

    // Warmup: absorb JUCE's one-time lazy allocations with a note held so the full
    // render path (inner fill + trims + fade + scrub) is exercised.
    constexpr int warmupBlocks = 64;
    for (int i = 0; i < warmupBlocks; ++i)
    {
        buffer.clear ();
        node->processBlock (buffer, held);
        held.clear ();
    }

    // Steady state: no note events, no fade — the wrapper's own hot path only.
    constexpr int measuredBlocks = 256;
    std::uint64_t allocations = 0;
    {
        juce::MidiBuffer empty;
        AllocationSentinel sentinel;
        for (int i = 0; i < measuredBlocks; ++i)
            node->processBlock (buffer, empty);
        allocations = sentinel.allocations ();
    }

    INFO ("steady-state wrapper allocations across " << measuredBlocks << " processBlock calls");
    REQUIRE (allocations == 0);
}
