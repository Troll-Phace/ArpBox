#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace arpbox::hosting
{
/** In-process plugin type discovery behind the `KnownPluginList::CustomScanner`
    seam (ARCHITECTURE §6.1).

    ┌─ PHASE 20 SEAM — READ BEFORE CHANGING ─────────────────────────────────────┐
    │ This class is the ONE place plugin type-discovery happens. Phase 20 replaces │
    │ ONLY this class with a `ChildProcessCoordinator`-backed scanner that runs    │
    │ the discovery in the `scanner-helper` child process (crash-isolated). All    │
    │ scanning already flows through here via `KnownPluginList::setCustomScanner`  │
    │ + `PluginDirectoryScanner`, so that swap stays localized. Do NOT add a       │
    │ second discovery path elsewhere.                                            │
    └─────────────────────────────────────────────────────────────────────────────┘

    IN-PROCESS CRASH CAVEAT: a genuine segfault in a hostile plugin CANNOT be
    survived in-process — that is exactly why Phase 20 exists. Here
    `findPluginTypesFor` performs the load in the host process and returns `true`
    (loaded); the dead-man's-pedal file plumbed by `PluginManager` is what Phase
    20 turns into a real blacklist-on-crash. A `false` return (crash marker) is
    reserved for the subprocess scanner.

    NOTE on cancellation: `CustomScanner::shouldExit()` is a non-virtual base
    helper (it polls the current `ThreadPoolJob`), NOT an override point. Our
    discovery call is a single non-blocking `findAllTypesForFile`, so there is
    nothing to poll mid-file; `PluginManager` cancels at file granularity in its
    own scan loop.

    MESSAGE/WORKER THREAD ONLY. Never touched from the audio thread. */
class InProcessScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    InProcessScanner () = default;
    ~InProcessScanner () override = default;

    /** Discovers all plugin types the given format finds at `fileOrIdentifier`,
        appending them to `result`. In-process discovery: delegates to the format's
        own `findAllTypesForFile`. Returns true (loaded) — see the crash caveat
        above; the subprocess scanner (Phase 20) returns false on a child crash. */
    bool findPluginTypesFor (juce::AudioPluginFormat& format,
                             juce::OwnedArray<juce::PluginDescription>& result,
                             const juce::String& fileOrIdentifier) override;

    /** Called by the list when a scan pass completes, for resource clean-up.
        No-op in-process; the Phase 20 subprocess scanner tears down the child here. */
    void scanFinished () override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InProcessScanner)
};
} // namespace arpbox::hosting
