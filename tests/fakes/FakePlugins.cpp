#include "fakes/FakePlugins.h"

namespace arpbox::testing
{
using namespace juce;

const std::vector<FakeSpec>& canonicalCorpus ()
{
    // 5 discoverable types + 1 crash-on-scan offender. Order is stable so tests
    // can reason about it. UIDs are arbitrary but fixed.
    static const std::vector<FakeSpec> corpus {
        { FakeBehavior::baseline, "arpbox.fake.baseline", "ARPBOX Fake Baseline Synth", true, true, 0x0FA0E001 },
        { FakeBehavior::crashOnScan,
          "arpbox.fake.crash-on-scan",
          "ARPBOX Fake Crash-On-Scan",
          true,
          false,
          0x0FA0E002 },
        { FakeBehavior::allocateInProcess,
          "arpbox.fake.allocate",
          "ARPBOX Fake Allocator FX",
          false,
          true,
          0x0FA0E003 },
        { FakeBehavior::wrongLatency,
          "arpbox.fake.wrong-latency",
          "ARPBOX Fake Wrong-Latency FX",
          false,
          true,
          0x0FA0E004 },
        { FakeBehavior::stateCorrupting,
          "arpbox.fake.state-corrupting",
          "ARPBOX Fake State-Corrupter",
          true,
          true,
          0x0FA0E005 },
        { FakeBehavior::busLying, "arpbox.fake.bus-lying", "ARPBOX Fake Bus-Liar FX", false, true, 0x0FA0E006 },
    };
    return corpus;
}

int numDiscoverableInCorpus ()
{
    int count = 0;
    for (const auto& spec : canonicalCorpus ())
        if (spec.discoverable)
            ++count;
    return count;
}

const std::vector<FakeSpec>& auxiliaryCorpus ()
{
    // NOT part of canonicalCorpus() on purpose — see the header note. These are
    // instrumentation instances, constructed directly by the tests that need them;
    // FakePluginFormat never enumerates them, so no scan-count assertion moves.
    // UIDs continue the canonical sequence so the two lists can never collide.
    static const std::vector<FakeSpec> corpus {
        { FakeBehavior::playHeadObserving,
          "arpbox.fake.playhead-observer",
          "ARPBOX Fake PlayHead Observer",
          true,
          false,
          0x0FA0E101 },
    };
    return corpus;
}

const FakeSpec& specFor (FakeBehavior behavior)
{
    for (const auto& spec : canonicalCorpus ())
        if (spec.behavior == behavior)
            return spec;

    for (const auto& spec : auxiliaryCorpus ())
        if (spec.behavior == behavior)
            return spec;

    jassertfalse; // Asked for a behavior in neither corpus.
    return canonicalCorpus ().front ();
}

PluginDescription makeDescription (const FakeSpec& spec, const String& formatName)
{
    PluginDescription description;
    description.name = spec.name;
    description.descriptiveName = spec.name;
    description.pluginFormatName = formatName;
    description.category = spec.isInstrument ? "Synth" : "Effect";
    description.manufacturerName = "ARPBOX Test";
    description.version = "1.0.0";
    description.fileOrIdentifier = spec.identifier;
    description.lastFileModTime = Time ();
    description.lastInfoUpdateTime = Time ();
    description.isInstrument = spec.isInstrument;
    description.numInputChannels = spec.isInstrument ? 0 : 2;
    description.numOutputChannels = 2;
    description.hasSharedContainer = false;
    description.uniqueId = spec.uid;
    description.deprecatedUid = spec.uid;
    return description;
}

std::unique_ptr<AudioPluginInstance> makeFakeInstance (const FakeSpec& spec)
{
    switch (spec.behavior)
    {
    case FakeBehavior::baseline:
    case FakeBehavior::stateCorrupting:
        // stateCorrupting overrides state I/O; baseline is the plain instrument.
        if (spec.behavior == FakeBehavior::stateCorrupting)
            return std::make_unique<StateCorruptingFake> (spec);
        return std::make_unique<FakePluginInstance> (spec);

    case FakeBehavior::allocateInProcess:
        return std::make_unique<AllocateInProcessFake> (spec);

    case FakeBehavior::wrongLatency:
        return std::make_unique<WrongLatencyFake> (spec);

    case FakeBehavior::busLying:
        return std::make_unique<BusLyingFake> (spec);

    case FakeBehavior::playHeadObserving:
        // Auxiliary instrumentation fake (auxiliaryCorpus): never reached via a
        // scan, only via a direct makeFakeInstance (specFor (...)) in a test.
        return std::make_unique<PlayHeadObservingFake> (spec);

    case FakeBehavior::crashOnScan:
    default:
        // Not instantiable: the crash offender exposes no type, so it should
        // never reach here via a scan. If asked directly, signal failure.
        return nullptr;
    }
}
} // namespace arpbox::testing
