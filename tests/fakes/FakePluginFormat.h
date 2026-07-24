// ─────────────────────────────────────────────────────────────────────────────
// FakePluginFormat — the injection vehicle for the hostile fake corpus.
//
// A minimal juce::AudioPluginFormat that synthesizes fake PluginDescriptions from
// synthetic identifiers and produces the tests/fakes instances. Registered on a
// caller-owned AudioPluginFormatManager (the PluginManager / PluginInstantiator
// DI seam), it lets the fakes travel the EXACT real scan + instantiate code path:
//
//   PluginManager::scanFormat  → PluginDirectoryScanner → InProcessScanner
//                              → format.findAllTypesForFile   (discovery)
//   PluginInstantiator::instantiate → manager.createPluginInstanceAsync
//                              → format.createPluginInstance   (instantiation)
//
// Scope: complete enough for those two paths (Phase 3). No real binaries touched.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "fakes/FakePlugins.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <map>
#include <vector>

namespace arpbox::testing
{
/** In-memory plugin format exposing the fake corpus. Construct with the default
    (the full canonical corpus: 5 discoverable + 1 crash-on-scan offender) or with
    an explicit spec set for narrower tests. Register it on an
    AudioPluginFormatManager and hand that manager to PluginManager /
    PluginInstantiator. */
class FakePluginFormat final : public juce::AudioPluginFormat
{
public:
    /** Uses the full canonical corpus. */
    FakePluginFormat ();

    /** Uses an explicit spec set (e.g. only well-behaved types). */
    explicit FakePluginFormat (std::vector<FakeSpec> specs);

    ~FakePluginFormat () override = default;

    // ── Discovery ──────────────────────────────────────────────────────────────
    juce::String getName () const override { return kFakeFormatName; }

    /** Appends a description for `fileOrIdentifier` IF it maps to a discoverable
        spec. The crash-on-scan offender maps to a spec but yields NO type (a
        simulated caught load failure) — the scanner then reports it in
        `failedFiles` while the rest of the scan completes (§1.4). */
    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results,
                              const juce::String& fileOrIdentifier) override;

    /** Ignores the search path (like AU) and returns the corpus identifiers — the
        set of "files" the scanner will walk. Includes the crash offender. */
    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath& directoriesToSearch,
                                             bool recursive,
                                             bool allowPluginsWhichRequireAsynchronousInstantiation) override;

    juce::FileSearchPath getDefaultLocationsToSearch () override { return {}; }

    bool fileMightContainThisPluginType (const juce::String& fileOrIdentifier) override;
    juce::String getNameOfPluginFromIdentifier (const juce::String& fileOrIdentifier) override;
    bool pluginNeedsRescanning (const juce::PluginDescription&) override { return false; }
    bool doesPluginStillExist (const juce::PluginDescription&) override { return true; }
    bool canScanForPlugins () const override { return true; }
    bool isTrivialToScan () const override { return true; }
    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override
    {
        return false;
    }

    // ── Load/instantiate instrumentation (test-only, not part of the format API) ─
    // The "was I actually loaded?" probe for the crash-recovery regression tests.
    // A blacklisted identifier must be skipped by KnownPluginList::scanAndAddFile
    // BEFORE the format is ever consulted, so its findAllTypesForFile counter must
    // stay at zero — that is the "no re-crash" guarantee. These maps are written on
    // the scanning/message thread and read by the test after the (single-threaded,
    // pumped) work completes; a CriticalSection guards them defensively. NOT audio
    // thread, NOT RT-relevant.

    /** How many times findAllTypesForFile (the scan-time load) ran for `identifier`. */
    int getFindAllTypesCallCount (const juce::String& identifier) const;

    /** How many times createPluginInstance (the instantiate-time load) ran for it. */
    int getCreateInstanceCallCount (const juce::String& identifier) const;

protected:
    // ── Instantiation ──────────────────────────────────────────────────────────
    /** Message-thread creation (per the JUCE contract). Maps the description's
        identifier back to a spec and produces the matching fake instance. An
        unmapped / non-instantiable identifier yields a null instance + error
        message — the typed creationFailed fail-path. The instance is NOT prepared
        here; PluginInstantiator applies prepareToPlay (prepare-before-insertion). */
    void createPluginInstance (const juce::PluginDescription& description,
                               double initialSampleRate,
                               int initialBufferSize,
                               PluginCreationCallback callback) override;

private:
    const FakeSpec* findSpecByIdentifier (const juce::String& identifier) const;

    std::vector<FakeSpec> specs;

    // Per-identifier load-invocation tallies (see the probe getters above).
    juce::CriticalSection callCountLock;
    std::map<juce::String, int> findAllTypesCalls;
    std::map<juce::String, int> createInstanceCalls;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FakePluginFormat)
};
} // namespace arpbox::testing
