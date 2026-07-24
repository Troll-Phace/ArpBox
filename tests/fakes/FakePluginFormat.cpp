#include "fakes/FakePluginFormat.h"

namespace arpbox::testing
{
using namespace juce;

FakePluginFormat::FakePluginFormat ()
    : specs (canonicalCorpus ())
{
}

FakePluginFormat::FakePluginFormat (std::vector<FakeSpec> specsToExpose)
    : specs (std::move (specsToExpose))
{
}

const FakeSpec* FakePluginFormat::findSpecByIdentifier (const String& identifier) const
{
    for (const auto& spec : specs)
        if (identifier == spec.identifier)
            return &spec;

    return nullptr;
}

int FakePluginFormat::getFindAllTypesCallCount (const String& identifier) const
{
    const ScopedLock sl (callCountLock);
    const auto it = findAllTypesCalls.find (identifier);
    return it != findAllTypesCalls.end () ? it->second : 0;
}

int FakePluginFormat::getCreateInstanceCallCount (const String& identifier) const
{
    const ScopedLock sl (callCountLock);
    const auto it = createInstanceCalls.find (identifier);
    return it != createInstanceCalls.end () ? it->second : 0;
}

void FakePluginFormat::findAllTypesForFile (OwnedArray<PluginDescription>& results,
                                            const String& fileOrIdentifier)
{
    // Record the scan-time load attempt. A blacklisted identifier never reaches
    // here (scanAndAddFile short-circuits on the blacklist), which is exactly what
    // the crash-recovery regression asserts.
    {
        const ScopedLock sl (callCountLock);
        ++findAllTypesCalls[fileOrIdentifier];
    }

    const auto* spec = findSpecByIdentifier (fileOrIdentifier);

    // Unknown identifier, or the crash-on-scan offender: yield NO type. For the
    // offender this simulates a caught load failure (the in-process crash caveat —
    // a real segfault is Phase 20). The scanner reports it in failedFiles.
    if (spec == nullptr || ! spec->discoverable)
        return;

    results.add (new PluginDescription (makeDescription (*spec, getName ())));
}

StringArray FakePluginFormat::searchPathsForPlugins (const FileSearchPath&,
                                                     bool,
                                                     bool)
{
    // Path is ignored (AU-style). Return every corpus identifier — the crash
    // offender included, so the scanner walks it and reports the failure.
    StringArray identifiers;
    for (const auto& spec : specs)
        identifiers.add (spec.identifier);

    return identifiers;
}

bool FakePluginFormat::fileMightContainThisPluginType (const String& fileOrIdentifier)
{
    // Lenient prefix match: any fake-looking identifier routes to this format so
    // the manager reaches createPluginInstance (an unmapped one then fails there,
    // exercising the format's own creation-failure branch). Real corpus entries
    // obviously match; a bogus "arpbox.fake.*" id does too.
    return fileOrIdentifier.startsWith ("arpbox.fake.");
}

String FakePluginFormat::getNameOfPluginFromIdentifier (const String& fileOrIdentifier)
{
    if (const auto* spec = findSpecByIdentifier (fileOrIdentifier))
        return spec->name;

    return fileOrIdentifier;
}

void FakePluginFormat::createPluginInstance (const PluginDescription& description,
                                             double initialSampleRate,
                                             int initialBufferSize,
                                             PluginCreationCallback callback)
{
    // Record the instantiate-time load attempt (companion to the scan-time probe).
    {
        const ScopedLock sl (callCountLock);
        ++createInstanceCalls[description.fileOrIdentifier];
    }

    const auto* spec = findSpecByIdentifier (description.fileOrIdentifier);

    if (spec == nullptr)
    {
        callback (nullptr,
                  "FakePluginFormat: no fake plugin with identifier '" + description.fileOrIdentifier + "'");
        return;
    }

    auto instance = makeFakeInstance (*spec);

    if (instance == nullptr)
    {
        // e.g. the crash-on-scan spec is not instantiable.
        callback (nullptr, "FakePluginFormat: '" + String (spec->name) + "' cannot be instantiated");
        return;
    }

    // Mirror real formats: stamp rate/block details on the raw instance. prepareToPlay
    // is applied by PluginInstantiator (prepare-before-insertion, §6.2), not here.
    instance->setRateAndBufferSizeDetails (initialSampleRate, initialBufferSize);

    callback (std::move (instance), {});
}
} // namespace arpbox::testing
