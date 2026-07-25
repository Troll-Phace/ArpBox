// ─────────────────────────────────────────────────────────────────────────────
// hosting-lab — INSTANTIATE path + fail-path (docs/INSTRUCTIONS.md Phase 3.3;
// ARCHITECTURE §6.2, §1.4).
//
// Drives PluginInstantiator's real async instantiation over the injected
// FakePluginFormat. Asserts: a good fake instantiates to a prepared, live instance
// (prepare-before-insertion); an unknown format yields Status::formatUnknown; a
// registered-format-but-bad-identifier yields Status::creationFailed; and in every
// failure the harness stays healthy and a subsequent good instantiation still
// succeeds. Delivery is async on the message thread — tests pump a bounded loop.
// Fakes only (.claude/rules/testing.md).
// ─────────────────────────────────────────────────────────────────────────────

#include "fakes/FakePluginFormat.h"
#include "fakes/FakePlugins.h"
#include "fakes/HostingLabSupport.h"

#include "hosting/InstantiationResult.h"
#include "hosting/PluginInstantiator.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>

using namespace arpbox::hosting;
using namespace arpbox::testing;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 128;

// Synchronously (from the test's perspective) instantiate `description` by pumping
// the message loop until the async callback fires. Returns the typed result.
InstantiationResult instantiateAndWait (PluginInstantiator& instantiator,
                                        const juce::PluginDescription& description,
                                        double sampleRate = kSampleRate,
                                        int blockSize = kBlockSize)
{
    std::atomic<bool> done { false };
    InstantiationResult captured;

    instantiator.instantiate (description,
                              sampleRate,
                              blockSize,
                              [&] (InstantiationResult result)
                              {
                                  captured = std::move (result);
                                  done.store (true, std::memory_order_release);
                              });

    REQUIRE (pumpUntil (done)); // false ⇒ timed out (hang guard)
    return captured;
}
} // namespace

TEST_CASE ("hosting/instantiate: good fake yields a prepared, live instance", "[hosting-lab]")
{
    MessageScope juceInit;

    juce::AudioPluginFormatManager formats;
    formats.addFormat (std::make_unique<FakePluginFormat> ());
    PluginInstantiator instantiator (formats);

    const auto description = makeDescription (specFor (FakeBehavior::baseline));
    auto result = instantiateAndWait (instantiator, description);

    SECTION ("result is ok with a non-null instance")
    {
        REQUIRE (result.ok ());
        REQUIRE (result.status == InstantiationResult::Status::ok);
        REQUIRE (result.instance != nullptr);
        REQUIRE (result.message.isEmpty ());
    }

    SECTION ("the instance was prepared at the requested rate/block")
    {
        auto* fake = dynamic_cast<FakePluginInstance*> (result.instance.get ());
        REQUIRE (fake != nullptr);
        REQUIRE (fake->getPrepareCallCount () == 1);
        REQUIRE (fake->getPreparedSampleRate () == kSampleRate);
        REQUIRE (fake->getPreparedBlockSize () == kBlockSize);
    }

    SECTION ("processBlock runs and the instance is live (non-silent under a note)")
    {
        juce::AudioBuffer<float> buffer (2, kBlockSize);
        buffer.clear ();

        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8)100), 0);

        result.instance->processBlock (buffer, midi); // must not crash

        REQUIRE (buffer.getMagnitude (0, buffer.getNumSamples ()) > 0.0f);
    }
}

TEST_CASE ("hosting/instantiate: unknown format yields a typed formatUnknown error", "[hosting-lab]")
{
    MessageScope juceInit;

    juce::AudioPluginFormatManager formats;
    formats.addFormat (std::make_unique<FakePluginFormat> ());
    PluginInstantiator instantiator (formats);

    // A description whose format name matches NO registered format.
    auto description = makeDescription (specFor (FakeBehavior::baseline), "NoSuchFormat");
    auto result = instantiateAndWait (instantiator, description);

    REQUIRE_FALSE (result.ok ());
    REQUIRE (result.status == InstantiationResult::Status::formatUnknown);
    REQUIRE (result.instance == nullptr);
    REQUIRE (result.message.isNotEmpty ());
}

TEST_CASE ("hosting/instantiate: bad identifier yields a typed creationFailed error", "[hosting-lab]")
{
    MessageScope juceInit;

    juce::AudioPluginFormatManager formats;
    formats.addFormat (std::make_unique<FakePluginFormat> ());
    PluginInstantiator instantiator (formats);

    // Registered format, but an identifier the format cannot map to a fake. It is
    // "fake-looking" so it passes fileMightContainThisPluginType and reaches the
    // format's own creation-failure branch.
    juce::PluginDescription description;
    description.pluginFormatName = kFakeFormatName;
    description.name = "Ghost";
    description.fileOrIdentifier = "arpbox.fake.does-not-exist";
    description.uniqueId = 0x0FA0EDED;
    description.deprecatedUid = 0x0FA0EDED;

    auto result = instantiateAndWait (instantiator, description);

    REQUIRE_FALSE (result.ok ());
    REQUIRE (result.status == InstantiationResult::Status::creationFailed);
    REQUIRE (result.instance == nullptr);
    REQUIRE (result.message.isNotEmpty ());
}

TEST_CASE ("hosting/instantiate: harness stays healthy after a failure", "[hosting-lab]")
{
    MessageScope juceInit;

    juce::AudioPluginFormatManager formats;
    formats.addFormat (std::make_unique<FakePluginFormat> ());
    PluginInstantiator instantiator (formats);

    // A failure first...
    auto badDescription = makeDescription (specFor (FakeBehavior::baseline), "NoSuchFormat");
    auto bad = instantiateAndWait (instantiator, badDescription);
    REQUIRE_FALSE (bad.ok ());

    // ...then a good instantiation on the same instantiator must still succeed
    // (no throw escaped, no state corrupted).
    auto good = instantiateAndWait (instantiator, makeDescription (specFor (FakeBehavior::baseline)));
    REQUIRE (good.ok ());
    REQUIRE (good.instance != nullptr);
}

TEST_CASE ("hosting/instantiate: every discoverable hostile fake instantiates and prepares", "[hosting-lab]")
{
    MessageScope juceInit;

    juce::AudioPluginFormatManager formats;
    formats.addFormat (std::make_unique<FakePluginFormat> ());
    PluginInstantiator instantiator (formats);

    // The full hostile corpus must at least CONSTRUCT + prepare cleanly this phase;
    // its hostile processBlock/latency/bus/state behavior is asserted later (Phase
    // 4/9/10/11). Here we only prove instantiation + prepare-before-insertion.
    for (const auto& spec : canonicalCorpus ())
    {
        if (! spec.discoverable)
            continue; // crash-on-scan exposes no type; covered by the scan tests.

        auto result = instantiateAndWait (instantiator, makeDescription (spec));

        INFO ("instantiating fake: " << spec.name);
        REQUIRE (result.ok ());
        REQUIRE (result.instance != nullptr);

        auto* fake = dynamic_cast<FakePluginInstance*> (result.instance.get ());
        REQUIRE (fake != nullptr);
        REQUIRE (fake->getPreparedSampleRate () == kSampleRate);
        REQUIRE (fake->getPreparedBlockSize () == kBlockSize);
    }
}
