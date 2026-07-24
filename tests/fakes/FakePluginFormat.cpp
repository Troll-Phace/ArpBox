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

void FakePluginFormat::findAllTypesForFile (OwnedArray<PluginDescription>& results,
                                            const String& fileOrIdentifier)
{
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
