// ─────────────────────────────────────────────────────────────────────────────
// hosting-lab — SCAN path (docs/INSTRUCTIONS.md Phase 3.3; ARCHITECTURE §6.1).
//
// Drives PluginManager's real scan + persistence code path with the injected
// FakePluginFormat (the DI seam). Asserts: a fake format populates the known list;
// the crash-on-scan offender is skipped and reported while the rest still scan;
// incremental rescan does not re-add known types; a saved list restores in a fresh
// manager (the headless proof of "persists across launches"). Fakes only — no real
// third-party binaries (.claude/rules/testing.md).
// ─────────────────────────────────────────────────────────────────────────────

#include "fakes/FakePluginFormat.h"
#include "fakes/FakePlugins.h"
#include "fakes/HostingLabSupport.h"

#include "hosting/PluginManager.h"

#include <catch2/catch_test_macros.hpp>

using namespace arpbox::hosting;
using namespace arpbox::testing;

namespace
{
// Registers a FakePluginFormat on a caller-owned format manager and returns the
// live format pointer (owned by the manager) for scanFormat().
juce::AudioPluginFormat& addFakeFormat (juce::AudioPluginFormatManager& formats)
{
    auto fake = std::make_unique<FakePluginFormat> ();
    auto& ref = *fake;
    formats.addFormat (std::move (fake));
    return ref;
}

// Names of the discoverable (non-crash) corpus entries.
juce::StringArray discoverableNames ()
{
    juce::StringArray names;
    for (const auto& spec : canonicalCorpus ())
        if (spec.discoverable)
            names.add (spec.name);
    return names;
}
} // namespace

TEST_CASE ("hosting/scan: fake format populates the known list with its types", "[hosting-lab]")
{
    MessageScope juceInit;
    UniqueTempDir temp;

    juce::AudioPluginFormatManager formats;
    auto& fakeFormat = addFakeFormat (formats);

    PluginManager manager (formats, temp.dir);
    const auto result = manager.scanFormat (fakeFormat, /*rescanExisting*/ true);

    SECTION ("all discoverable types are added")
    {
        REQUIRE (result.numTypesInList == numDiscoverableInCorpus ());
        REQUIRE (manager.getKnownPluginList ().getNumTypes () == numDiscoverableInCorpus ());
    }

    SECTION ("every discoverable plugin is present by name")
    {
        juce::StringArray found;
        for (const auto& type : manager.getKnownPluginList ().getTypes ())
            found.add (type.name);

        for (const auto& expected : discoverableNames ())
            REQUIRE (found.contains (expected));
    }
}

TEST_CASE ("hosting/scan: crash-on-scan fake is skipped and reported", "[hosting-lab]")
{
    MessageScope juceInit;
    UniqueTempDir temp;

    juce::AudioPluginFormatManager formats;
    auto& fakeFormat = addFakeFormat (formats);

    PluginManager manager (formats, temp.dir);
    const auto result = manager.scanFormat (fakeFormat, /*rescanExisting*/ true);

    const auto& crashSpec = specFor (FakeBehavior::crashOnScan);

    SECTION ("the offender lands in failedFiles")
    {
        REQUIRE (result.failedFiles.contains (crashSpec.identifier));
    }

    SECTION ("the rest of the scan still completes")
    {
        // Failure isolation (§1.4): one bad file does not abort the pass.
        REQUIRE (result.numTypesInList == numDiscoverableInCorpus ());
    }

    SECTION ("the offender is not added as a usable type")
    {
        for (const auto& type : manager.getKnownPluginList ().getTypes ())
            REQUIRE (type.fileOrIdentifier != juce::String (crashSpec.identifier));
    }
}

TEST_CASE ("hosting/scan: incremental rescan does not re-add known types", "[hosting-lab]")
{
    MessageScope juceInit;
    UniqueTempDir temp;

    juce::AudioPluginFormatManager formats;
    auto& fakeFormat = addFakeFormat (formats);

    PluginManager manager (formats, temp.dir);

    const auto first = manager.scanFormat (fakeFormat, /*rescanExisting*/ true);
    REQUIRE (first.numTypesInList == numDiscoverableInCorpus ());

    // rescanExisting == false ⇒ incremental: known files are skipped
    // (pluginNeedsRescanning is false), so the count must not grow.
    const auto second = manager.scanFormat (fakeFormat, /*rescanExisting*/ false);

    REQUIRE (second.numTypesInList == numDiscoverableInCorpus ());
    REQUIRE (manager.getKnownPluginList ().getNumTypes () == numDiscoverableInCorpus ());
}

TEST_CASE ("hosting/scan: scanAll discovers the fake format", "[hosting-lab]")
{
    MessageScope juceInit;
    UniqueTempDir temp;

    juce::AudioPluginFormatManager formats;
    addFakeFormat (formats);

    PluginManager manager (formats, temp.dir);
    const auto result = manager.scanAll (/*rescanExisting*/ true);

    REQUIRE (result.numTypesInList == numDiscoverableInCorpus ());
    REQUIRE (result.failedFiles.contains (specFor (FakeBehavior::crashOnScan).identifier));
}

TEST_CASE ("hosting/persistence: saved list restores in a fresh manager", "[hosting-lab]")
{
    MessageScope juceInit;
    UniqueTempDir temp; // one directory shared by both manager instances

    // First launch: scan and save.
    juce::StringArray savedNames;
    {
        juce::AudioPluginFormatManager formats;
        auto& fakeFormat = addFakeFormat (formats);

        PluginManager manager (formats, temp.dir);
        manager.scanFormat (fakeFormat, /*rescanExisting*/ true);

        for (const auto& type : manager.getKnownPluginList ().getTypes ())
            savedNames.add (type.name);

        REQUIRE (savedNames.size () == numDiscoverableInCorpus ());
        manager.save ();
        REQUIRE (manager.getPluginListFile ().existsAsFile ());
    }

    // Second launch: a fresh format manager + fresh PluginManager over the SAME
    // settings directory. restore() must reproduce the list WITHOUT rescanning.
    {
        juce::AudioPluginFormatManager formats;
        addFakeFormat (formats);

        PluginManager manager (formats, temp.dir);
        manager.restore ();

        REQUIRE (manager.getKnownPluginList ().getNumTypes () == numDiscoverableInCorpus ());

        juce::StringArray restoredNames;
        for (const auto& type : manager.getKnownPluginList ().getTypes ())
            restoredNames.add (type.name);

        for (const auto& name : savedNames)
            REQUIRE (restoredNames.contains (name));
    }
}
