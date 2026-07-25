// ─────────────────────────────────────────────────────────────────────────────
// hosting-lab — CRASH-RECOVERY regression (issue #19 in-process-scan stopgap).
//
// The stopgap that just landed persists scan progress + the dead-man's-pedal so a
// relaunch after a hostile-plugin crash resumes WITHOUT re-loading the offender.
// The throwaway piece (DebugPanel throttle timing) is UI and is deliberately NOT
// tested here. The DURABLE recovery guarantees live in PluginManager /
// KnownPluginList and are headless-testable — these tests lock THOSE in:
//
//   1. Dead-man's-pedal → restore() blacklists the offender → a later scan SKIPS it
//      WITHOUT ever loading it (findAllTypesForFile never runs for it). This is the
//      "no re-crash" contract: a plugin that killed the last scan is not touched.
//   2. save()/restore() round-trips BOTH the scanned types and the blacklist, so
//      the quarantine survives a relaunch (the whole point of persistence).
//   3. An incremental resume (rescanExisting == false) skips already-known plugins
//      (isListingUpToDate), so completed scan work is not redone — while a genuinely
//      new plugin is still picked up.
//
// Everything stays inside a UniqueTempDir; nothing touches ~/Library or real
// binaries. Fakes only (.claude/rules/testing.md). The fake's per-identifier
// load-invocation counter (FakePluginFormat::getFindAllTypesCallCount) is the probe
// that makes "was it actually loaded?" observable.
// ─────────────────────────────────────────────────────────────────────────────

#include "fakes/FakePluginFormat.h"
#include "fakes/FakePlugins.h"
#include "fakes/HostingLabSupport.h"

#include "hosting/PluginManager.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

using namespace arpbox::hosting;
using namespace arpbox::testing;

namespace
{
// Registers a FakePluginFormat on a caller-owned format manager and returns the
// live format pointer (owned by the manager) so tests can both scanFormat() through
// it and read its per-identifier load-invocation counters.
FakePluginFormat& addFakeFormat (juce::AudioPluginFormatManager& formats)
{
    auto fake = std::make_unique<FakePluginFormat> ();
    auto& ref = *fake;
    formats.addFormat (std::move (fake));
    return ref;
}

// Same, but exposing an explicit spec set (used to introduce a brand-new plugin
// that is absent from a previously-saved list).
FakePluginFormat& addFakeFormat (juce::AudioPluginFormatManager& formats, std::vector<FakeSpec> specs)
{
    auto fake = std::make_unique<FakePluginFormat> (std::move (specs));
    auto& ref = *fake;
    formats.addFormat (std::move (fake));
    return ref;
}

juce::String identifierFor (FakeBehavior behavior)
{
    return specFor (behavior).identifier;
}

bool listContainsIdentifier (const juce::KnownPluginList& list, const juce::String& identifier)
{
    for (const auto& type : list.getTypes ())
        if (type.fileOrIdentifier == identifier)
            return true;
    return false;
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1. Dead-man's-pedal → blacklist → skipped WITHOUT loading (no re-crash).
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE ("hosting/crash-recovery: dead-man's-pedal blacklists the offender and the "
           "next scan skips it without loading it",
           "[hosting-lab]")
{
    MessageScope juceInit;
    UniqueTempDir temp;

    juce::AudioPluginFormatManager formats;
    auto& fakeFormat = addFakeFormat (formats);

    // The offender: a plugin that WOULD load fine (baseline is discoverable), but
    // that the previous session recorded as crash-on-load in the dead-man's-pedal.
    // Using a loadable fake makes the guarantee strict — the ONLY reason it must not
    // load this session is the blacklist, not that it is un-loadable anyway.
    const auto offender = identifierFor (FakeBehavior::baseline);

    // Seed the dead-man's-pedal exactly as JUCE would leave it after a crash: the
    // file PluginManager::getDeadMansPedalFile() reads, newline-joined identifiers.
    PluginManager seed (formats, temp.dir);
    const auto pedalFile = seed.getDeadMansPedalFile ();
    REQUIRE (pedalFile.replaceWithText (offender + "\n"));

    // restore() reads the pedal and blacklists the offender.
    PluginManager manager (formats, temp.dir);
    manager.restore ();

    SECTION ("restore() moved the offender onto the blacklist")
    {
        REQUIRE (manager.getKnownPluginList ().getBlacklistedFiles ().contains (offender));
    }

    // Now scan the whole corpus. The blacklisted offender must be skipped by
    // KnownPluginList::scanAndAddFile BEFORE the format is consulted.
    const auto result = manager.scanFormat (fakeFormat, /*rescanExisting*/ true);

    SECTION ("the blacklisted offender was NEVER loaded (no re-crash)")
    {
        // The load probe: findAllTypesForFile is the scan-time load. It must not
        // have run for the blacklisted identifier even once.
        REQUIRE (fakeFormat.getFindAllTypesCallCount (offender) == 0);
    }

    SECTION ("the blacklisted offender is not added as a usable type")
    {
        REQUIRE_FALSE (listContainsIdentifier (manager.getKnownPluginList (), offender));
    }

    SECTION ("failure isolation: the rest of the corpus still scanned normally")
    {
        // Every other discoverable fake WAS loaded and added — one bad (blacklisted)
        // entry does not abort the pass (§1.4). List size = discoverable minus the
        // one blacklisted offender.
        REQUIRE (manager.getKnownPluginList ().getNumTypes () == numDiscoverableInCorpus () - 1);

        const auto goodNeighbour = identifierFor (FakeBehavior::wrongLatency);
        REQUIRE (fakeFormat.getFindAllTypesCallCount (goodNeighbour) >= 1);
        REQUIRE (listContainsIdentifier (manager.getKnownPluginList (), goodNeighbour));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. save() / restore() round-trips the blacklist (quarantine survives relaunch).
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE ("hosting/crash-recovery: save/restore round-trips the blacklist alongside "
           "the scanned types",
           "[hosting-lab]")
{
    MessageScope juceInit;
    UniqueTempDir temp; // one directory shared by both manager instances

    // A quarantined identifier that survives across the relaunch. Use the
    // crash-on-scan offender's id — the realistic "this one crashed, keep it out"
    // case (it yields no type of its own, so it never appears as a real type).
    const auto quarantined = identifierFor (FakeBehavior::crashOnScan);

    // First launch: scan the good fakes so there ARE real types, blacklist the
    // offender, then persist.
    {
        juce::AudioPluginFormatManager formats;
        auto& fakeFormat = addFakeFormat (formats);

        PluginManager manager (formats, temp.dir);
        const auto result = manager.scanFormat (fakeFormat, /*rescanExisting*/ true);
        REQUIRE (result.numTypesInList == numDiscoverableInCorpus ());

        manager.getKnownPluginList ().addToBlacklist (quarantined);
        REQUIRE (manager.getKnownPluginList ().getBlacklistedFiles ().contains (quarantined));

        REQUIRE (manager.save ().wasOk ());
        REQUIRE (manager.getPluginListFile ().existsAsFile ());
    }

    // Second launch: a fresh format manager + fresh PluginManager over the SAME
    // directory. restore() must reproduce BOTH the types and the blacklist without
    // rescanning.
    {
        juce::AudioPluginFormatManager formats;
        addFakeFormat (formats);

        PluginManager manager (formats, temp.dir);
        manager.restore ();

        SECTION ("the scanned types survived the round-trip")
        {
            REQUIRE (manager.getKnownPluginList ().getNumTypes () == numDiscoverableInCorpus ());
        }

        SECTION ("the blacklisted identifier survived the round-trip")
        {
            REQUIRE (manager.getKnownPluginList ().getBlacklistedFiles ().contains (quarantined));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Resume skips already-known plugins (progress persistence is meaningful).
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE ("hosting/crash-recovery: an incremental resume skips already-known plugins "
           "and still picks up a new one",
           "[hosting-lab]")
{
    MessageScope juceInit;
    UniqueTempDir temp; // shared by both launches

    // First launch: scan the canonical corpus and persist the resulting list.
    {
        juce::AudioPluginFormatManager formats;
        auto& fakeFormat = addFakeFormat (formats);

        PluginManager manager (formats, temp.dir);
        const auto result = manager.scanFormat (fakeFormat, /*rescanExisting*/ true);
        REQUIRE (result.numTypesInList == numDiscoverableInCorpus ());

        REQUIRE (manager.save ().wasOk ());
        REQUIRE (manager.getPluginListFile ().existsAsFile ());
    }

    // Second launch: fresh managers over the same dir. The format now exposes the
    // canonical corpus PLUS one brand-new plugin that is absent from the saved list.
    auto specs = canonicalCorpus ();
    const FakeSpec newcomerSpec {
        FakeBehavior::baseline, "arpbox.fake.newcomer", "ARPBOX Fake Newcomer", true, true, 0x0FA0E099
    };
    specs.push_back (newcomerSpec);
    const juce::String newcomer = newcomerSpec.identifier;

    juce::AudioPluginFormatManager formats;
    auto& fakeFormat = addFakeFormat (formats, specs);

    PluginManager manager (formats, temp.dir);
    manager.restore ();

    SECTION ("restore() reproduced the previously-scanned types (no rescan needed)")
    {
        REQUIRE (manager.getKnownPluginList ().getNumTypes () == numDiscoverableInCorpus ());
    }

    // Incremental resume: rescanExisting == false ⇒ known files are skipped via
    // isListingUpToDate, so they are NOT re-loaded.
    const auto result = manager.scanFormat (fakeFormat, /*rescanExisting*/ false);

    SECTION ("already-known plugins were NOT re-loaded on resume")
    {
        // The load probe on THIS launch's fresh format starts at zero; a skipped
        // known plugin must leave it at zero. Check a representative known fake.
        REQUIRE (fakeFormat.getFindAllTypesCallCount (identifierFor (FakeBehavior::baseline)) == 0);
        REQUIRE (fakeFormat.getFindAllTypesCallCount (identifierFor (FakeBehavior::wrongLatency)) == 0);
    }

    SECTION ("a genuinely new plugin IS loaded and added on resume")
    {
        REQUIRE (fakeFormat.getFindAllTypesCallCount (newcomer) >= 1);
        REQUIRE (listContainsIdentifier (manager.getKnownPluginList (), newcomer));
        // The list grew by exactly the one newcomer.
        REQUIRE (manager.getKnownPluginList ().getNumTypes () == numDiscoverableInCorpus () + 1);
    }
}
