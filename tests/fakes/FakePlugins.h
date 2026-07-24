// ─────────────────────────────────────────────────────────────────────────────
// tests/fakes — the hostile fake-plugin corpus (ARCHITECTURE §1.4 "plugins are
// hostile until proven otherwise", §6.1/§6.2; docs/INSTRUCTIONS.md Phase 3.3).
//
// In-memory juce::AudioPluginInstance fakes that travel the REAL hosting code
// path (PluginManager scan → PluginInstantiator instantiate) via FakePluginFormat
// (the dependency-injection seam). Real third-party plugins are NEVER instantiated
// in tests (.claude/rules/testing.md) — these fakes stand in for every hostile
// behavior the product must survive.
//
// SCOPE (Phase 3): the FULL corpus is BUILT here, but Phase 3 only ASSERTS
// scan / instantiate / fail-path. The processBlock/latency/bus/state hostility is
// exercised later — HostedPluginNode wrapper (Phase 4), FX rack (Phase 9), editor
// windows (Phase 10), persistence (Phase 11). The corpus just needs to EXIST now.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace arpbox::testing
{
/** The format name every fake description carries. Must match
    FakePluginFormat::getName() so PluginInstantiator's format-registration
    pre-check resolves it (a DIFFERENT name is how the formatUnknown fail-path
    test forces Status::formatUnknown). */
inline constexpr const char* kFakeFormatName = "ARPBOXFake";

/** A latency (samples) the wrong-latency fake reports but does not actually
    introduce — Phase 9's FX-rack latency compensation asserts against this. */
inline constexpr int kWrongLatencySamples = 512;

// ─────────────────────────────────────────────────────────────────────────────
// Corpus taxonomy
// ─────────────────────────────────────────────────────────────────────────────

/** The hostile behaviors the corpus covers (docs/INSTRUCTIONS.md Phase 3.3). */
enum class FakeBehavior : std::uint8_t
{
    baseline,          ///< Well-behaved instrument. The control case.
    crashOnScan,       ///< SCAN-ONLY: yields no type (a caught/failed load — see caveat).
    allocateInProcess, ///< Allocates on the heap inside processBlock (RT violation).
    wrongLatency,      ///< Reports a bogus latency it does not actually apply.
    stateCorrupting,   ///< getState/setState misbehave (unstable / non-restoring blob).
    busLying           ///< Refuses every bus layout offered to it.
};

/** One corpus entry. `discoverable == false` marks the crash-on-scan offender:
    the format yields NO PluginDescription for it, so the scanner reports it in
    `failedFiles` while the rest of the scan completes (§1.4 failure isolation).

    IN-PROCESS CRASH CAVEAT (plan "Coordination seams" #3): a genuine segfault
    cannot be survived in-process — that is what Phase 20's subprocess scanner is
    for. The crash-on-scan fake therefore SIMULATES failure by producing no types
    (a caught load failure), NOT by actually crashing. */
struct FakeSpec
{
    FakeBehavior behavior;
    const char* identifier; ///< The `fileOrIdentifier` string used end to end.
    const char* name;       ///< Human-readable plugin name.
    bool isInstrument;      ///< Instrument (MIDI in, audio out) vs effect (audio in/out).
    bool discoverable;      ///< False only for crashOnScan (yields no type on scan).
    int uid;                ///< Stable unique id for the PluginDescription.
};

/** The canonical corpus, in a stable order: 5 discoverable types + 1 crash
    offender. Shared by FakePluginFormat (scan + instantiate) and the tests so the
    two never drift. */
const std::vector<FakeSpec>& canonicalCorpus ();

/** Count of discoverable (non-crash) entries — the number of types a full scan of
    the canonical corpus must add to the KnownPluginList. */
int numDiscoverableInCorpus ();

/** Looks up a spec by behavior (asserts it exists). */
const FakeSpec& specFor (FakeBehavior behavior);

/** Builds the PluginDescription a scan would produce for `spec`, stamped with
    `formatName` (defaults to kFakeFormatName). Centralised so the format's
    findAllTypesForFile and the tests agree byte-for-byte. */
juce::PluginDescription makeDescription (const FakeSpec& spec,
                                         const juce::String& formatName = kFakeFormatName);

// ─────────────────────────────────────────────────────────────────────────────
// Fake instances
// ─────────────────────────────────────────────────────────────────────────────

/** Base fake: a minimal but complete juce::AudioPluginInstance implementing every
    AudioProcessor pure virtual. Well-behaved by default (the `baseline` case):
    instruments synthesize a fixed tone while a note is held; effects pass audio
    through untouched. Records the sample-rate/block it was prepared at so the
    instantiate-success test can assert prepare-before-insertion (§6.2). */
class FakePluginInstance : public juce::AudioPluginInstance
{
public:
    explicit FakePluginInstance (FakeSpec spec)
        : juce::AudioPluginInstance (makeBuses (spec.isInstrument))
        , spec (spec)
    {
    }

    // ── Introspection for tests (not part of the AudioProcessor surface) ──────
    double getPreparedSampleRate () const noexcept { return preparedSampleRate; }
    int getPreparedBlockSize () const noexcept { return preparedBlockSize; }
    int getPrepareCallCount () const noexcept { return prepareCalls; }
    FakeBehavior getBehavior () const noexcept { return spec.behavior; }

    // ── AudioPluginInstance ───────────────────────────────────────────────────
    void fillInPluginDescription (juce::PluginDescription& description) const override
    {
        description = makeDescription (spec);
    }

    // ── AudioProcessor ─────────────────────────────────────────────────────────
    const juce::String getName () const override { return spec.name; }

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override
    {
        // Record what we were prepared at — the instantiator calls this directly
        // (not via setRateAndBufferSizeDetails), so getSampleRate() would read 0.
        preparedSampleRate = sampleRate;
        preparedBlockSize = maximumExpectedSamplesPerBlock;
        ++prepareCalls;
    }

    void releaseResources () override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::ScopedNoDenormals noDenormals;
        renderBaseline (buffer, midiMessages);
    }

    double getTailLengthSeconds () const override { return 0.0; }
    bool acceptsMidi () const override { return spec.isInstrument; }
    bool producesMidi () const override { return false; }

    juce::AudioProcessorEditor* createEditor () override { return nullptr; }
    bool hasEditor () const override { return false; }

    int getNumPrograms () override { return 1; }
    int getCurrentProgram () override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override
    {
        // Well-behaved, stable state: a small identifiable blob.
        destData.reset ();
        destData.append (kStateTag, sizeof (kStateTag));
        const auto uid = static_cast<std::uint32_t> (spec.uid);
        destData.append (&uid, sizeof (uid));
    }

    void setStateInformation (const void* data, int sizeInBytes) override
    {
        lastRestoredState.replaceAll (data, static_cast<size_t> (juce::jmax (0, sizeInBytes)));
    }

protected:
    // The note-tracking baseline render, shared by well-behaved subclasses.
    void renderBaseline (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
    {
        for (const auto meta : midiMessages)
        {
            const auto message = meta.getMessage ();
            if (message.isNoteOn ())
                ++activeVoices;
            else if (message.isNoteOff ())
                activeVoices = juce::jmax (0, activeVoices - 1);
            else if (message.isAllNotesOff () || message.isAllSoundOff ())
                activeVoices = 0;
        }

        if (! spec.isInstrument)
            return; // Effect: pass audio through untouched (well-behaved).

        // Instrument: emit a fixed, deterministic tone while any note is held so
        // the instantiate-success test can assert the instance is live (non-silent).
        const float amplitude = activeVoices > 0 ? 0.25f : 0.0f;
        for (int channel = 0; channel < buffer.getNumChannels (); ++channel)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (channel),
                                               amplitude,
                                               buffer.getNumSamples ());
    }

    static constexpr char kStateTag[4] = { 'A', 'F', 'K', '1' };

    FakeSpec spec;
    juce::MemoryBlock lastRestoredState;
    int activeVoices = 0;
    double preparedSampleRate = 0.0;
    int preparedBlockSize = 0;
    int prepareCalls = 0;

private:
    static BusesProperties makeBuses (bool isInstrument)
    {
        if (isInstrument)
            return BusesProperties ().withOutput ("Output", juce::AudioChannelSet::stereo (), true);

        return BusesProperties ()
            .withInput ("Input", juce::AudioChannelSet::stereo (), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo (), true);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FakePluginInstance)
};

/** Allocates on the heap inside processBlock — the canonical RT-safety violation.
    Phase 4's HostedPluginNode wrapper + the allocation-guard harness catch this. */
class AllocateInProcessFake final : public FakePluginInstance
{
public:
    explicit AllocateInProcessFake (FakeSpec spec) : FakePluginInstance (spec) {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::ScopedNoDenormals noDenormals;
        // Deliberate audio-thread allocation (the hostile behavior). Grow a member
        // vector so the compiler cannot elide it, then touch it.
        heapChurn.resize (static_cast<size_t> (buffer.getNumSamples ()) + heapChurn.size () + 1u, 0.0f);
        heapChurn.back () = 1.0f;
        if (heapChurn.size () > 1u << 16)
            heapChurn.clear (); // Bound growth so a long soak does not OOM.

        renderBaseline (buffer, midiMessages);
    }

private:
    std::vector<float> heapChurn;
};

/** Reports kWrongLatencySamples of latency it never actually applies. Phase 9's
    dry/wet null test and latency-compensation assertions exercise this. */
class WrongLatencyFake final : public FakePluginInstance
{
public:
    explicit WrongLatencyFake (FakeSpec spec) : FakePluginInstance (spec)
    {
        setLatencySamples (kWrongLatencySamples);
    }

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override
    {
        FakePluginInstance::prepareToPlay (sampleRate, maximumExpectedSamplesPerBlock);
        // Re-assert the lie after prepare (some hosts reset latency on prepare).
        setLatencySamples (kWrongLatencySamples);
    }
};

/** Unstable, non-restoring state — getStateInformation differs between calls and
    setStateInformation drops the data. Phase 11's persistence torture tests use it. */
class StateCorruptingFake final : public FakePluginInstance
{
public:
    explicit StateCorruptingFake (FakeSpec spec) : FakePluginInstance (spec) {}

    void getStateInformation (juce::MemoryBlock& destData) override
    {
        // Hostile: emit a different blob every call (no stable state to persist).
        destData.reset ();
        ++saveCounter;
        destData.append (&saveCounter, sizeof (saveCounter));
    }

    void setStateInformation (const void*, int) override
    {
        // Hostile: silently discard the restored state instead of applying it.
    }

private:
    int saveCounter = 0;
};

/** Refuses every bus layout offered — the bus-lying case. Phase 4's bus
    negotiation (mono→stereo adaptation / graceful refusal) exercises this. */
class BusLyingFake final : public FakePluginInstance
{
public:
    explicit BusLyingFake (FakeSpec spec) : FakePluginInstance (spec) {}

    bool isBusesLayoutSupported (const BusesLayout&) const override
    {
        return false; // Lie: nothing is ever acceptable.
    }
};

/** Constructs the fake instance for a spec. Returns nullptr for a non-instantiable
    entry (crashOnScan) — the format turns that into a typed creationFailed. */
std::unique_ptr<juce::AudioPluginInstance> makeFakeInstance (const FakeSpec& spec);
} // namespace arpbox::testing
