#pragma once

#include "hosting/InProcessScanner.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>

namespace arpbox::hosting
{
/** Owns the app's `KnownPluginList` and drives VST3/AU discovery + persistence
    (ARCHITECTURE §6.1). This is the message/worker-thread hub for "what plugins
    exist"; instantiation of a chosen plugin lives in `PluginInstantiator` (§6.2).

    ┌─ THE DEPENDENCY-INJECTION SEAM (do not remove) ────────────────────────────┐
    │ The manager does NOT build its own `AudioPluginFormatManager`. It borrows   │
    │ one by reference from the caller. Production wiring passes a manager loaded  │
    │ with VST3 + AudioUnit (see `addProductionFormats`); the hosting-lab passes  │
    │ one registering a `FakePluginFormat`, so hostile fakes travel the exact     │
    │ real scan/instantiate code path. The referenced format manager MUST outlive │
    │ this object.                                                                 │
    └─────────────────────────────────────────────────────────────────────────────┘

    SCANNING is failure-isolated and flows through ONE seam: the
    `InProcessScanner` (`KnownPluginList::CustomScanner`), registered on the list
    in the constructor. `PluginDirectoryScanner` calls into that scanner, so
    Phase 20's out-of-process swap replaces only `InProcessScanner`.

    DEAD-MAN'S-PEDAL: a `plugin-deadmanspedal` file is passed to every
    `PluginDirectoryScanner` and re-applied on `restore`. Phase 3 only PLUMBS it;
    the quarantine UI + crash-driven blacklisting arrive with the subprocess
    scanner in Phase 20.

    THREADING: everything here is MESSAGE or WORKER thread. `scanFormat`/`scanAll`
    BLOCK and MUST be called off the message thread (and never the audio thread) —
    the app owns a `juce::Thread` for that. Nothing here reaches into the audio
    graph. Persistence and search-path edits are message-thread. */
class PluginManager final
{
public:
    // ── Scan reporting ───────────────────────────────────────────────────────

    /** Aggregate outcome of a scan pass. `numTypesInList` is the full list size
        after the pass (not a delta); `failedFiles` are identifiers the scanner
        could not load (they do NOT abort the rest of the scan — §1.4). */
    struct ScanResult
    {
        int numTypesInList { 0 };
        juce::StringArray failedFiles;
    };

    /** Invoked between files with the plugin currently being scanned and overall
        progress in [0, 1]. Called on the SCANNING thread. May be empty. */
    using ProgressCallback = std::function<void (const juce::String& pluginBeingScanned, float progress)>;

    /** Polled between files; return true to abort the scan early. Called on the
        SCANNING thread. May be empty (never cancels). */
    using CancelCallback = std::function<bool ()>;

    // ── Construction ─────────────────────────────────────────────────────────

    /** Borrows a caller-owned `AudioPluginFormatManager` (the DI seam above) and
        registers the `InProcessScanner` on the internal `KnownPluginList`.

        @param formats            Format manager to scan/instantiate through; MUST
                                  outlive this object. Not owned.
        @param settingsDirectory  Where `plugin-list.xml` + `plugin-deadmanspedal`
                                  live. Empty (default) resolves to the standard
                                  `~/Library/Application Support/ARPBOX` dir, matching
                                  `AudioEngine`. Tests pass a temp dir to avoid
                                  touching real user files. MESSAGE-THREAD ONLY. */
    explicit PluginManager (juce::AudioPluginFormatManager& formats, juce::File settingsDirectory = {});

    ~PluginManager ();

    // ── Search paths ─────────────────────────────────────────────────────────

    /** Adds a user search path appended to every format's default locations.
        File-based formats (VST3) honour it; AU ignores paths (component registry).
        MESSAGE-THREAD ONLY. */
    void addUserSearchPath (const juce::File& directory);

    /** Effective search path for a format: the format's default locations
        (e.g. `~/Library/Audio/Plug-Ins/VST3`, `/Library/Audio/Plug-Ins/VST3`)
        plus any user-added paths. MESSAGE-THREAD ONLY. */
    juce::FileSearchPath getSearchPathsForFormat (juce::AudioPluginFormat& format) const;

    // ── Scanning (BLOCKING — call off the message/audio thread) ──────────────

    /** Scans a single format into the known list. When `rescanExisting` is false
        the scan is incremental (skips files already in the list, mirroring
        `pluginNeedsRescanning`); true forces a full re-read. Failed files are
        collected, not fatal. */
    ScanResult scanFormat (juce::AudioPluginFormat& format,
                           bool rescanExisting,
                           const ProgressCallback& onProgress = {},
                           const CancelCallback& shouldCancel = {});

    /** Scans every format registered on the injected format manager (production:
        VST3 + AU). `rescanExisting == false` is the incremental rescan; true is
        the explicit full rescan. Aggregates failed files across formats. */
    ScanResult scanAll (bool rescanExisting,
                        const ProgressCallback& onProgress = {},
                        const CancelCallback& shouldCancel = {});

    /** Requests cooperative early-exit from an in-progress scan. The scan loop
        checks this between files, so cancellation is honoured at file granularity.
        Callable from any non-audio thread. Cleared automatically when the next
        scan begins. */
    void cancelScan () noexcept;

    // ── Persistence (MESSAGE-THREAD ONLY) ────────────────────────────────────

    /** Writes the known-plugin list to `plugin-list.xml`. Mirrors
        `AudioEngine::saveDeviceState` (createXml → writeTo). */
    void save () const;

    /** Restores the known-plugin list from `plugin-list.xml` (if present) and
        re-applies dead-man's-pedal blacklistings from `plugin-deadmanspedal`.
        Mirrors `AudioEngine::initialiseDevice` (parseXML → recreate). */
    void restore ();

    // ── Accessors ────────────────────────────────────────────────────────────

    /** The authoritative known-plugin list (for a picker UI + tests). */
    juce::KnownPluginList& getKnownPluginList () noexcept { return knownList; }
    const juce::KnownPluginList& getKnownPluginList () const noexcept { return knownList; }

    /** The injected format manager (for the instantiation service + tests). */
    juce::AudioPluginFormatManager& getFormatManager () noexcept { return formats; }

    /** Directory holding the persisted files (created if missing). */
    juce::File getSettingsDirectory () const;
    /** `plugin-list.xml` under the settings directory. */
    juce::File getPluginListFile () const;
    /** `plugin-deadmanspedal` under the settings directory. */
    juce::File getDeadMansPedalFile () const;

private:
    // Borrowed, not owned (the DI seam). Must outlive this object.
    juce::AudioPluginFormatManager& formats;

    // Empty => resolve the standard ARPBOX app-data dir lazily (getSettingsDirectory).
    juce::File settingsDir;

    juce::KnownPluginList knownList;

    // User-added search paths appended to each format's defaults.
    juce::FileSearchPath userSearchPaths;

    // Cooperative-cancel latch. Set by cancelScan() from any non-audio thread,
    // polled by the scan loop between files, reset at the start of each scan.
    std::atomic<bool> cancelRequested { false };

    // The single scan loop shared by scanFormat() and scanAll(). Does NOT reset
    // the cancel latch — the public entry points do, so a cancel raised between
    // formats during scanAll() is not clobbered.
    ScanResult scanOneFormat (juce::AudioPluginFormat& format,
                              bool rescanExisting,
                              const ProgressCallback& onProgress,
                              const CancelCallback& shouldCancel);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginManager)
};

/** Registers the production plugin formats (VST3 + AudioUnit on macOS) on a
    caller-owned format manager, ready to hand to `PluginManager`. Thin wrapper
    over `AudioPluginFormatManager::addDefaultFormats` kept here so the app has a
    single production entry point and the DI seam stays explicit. The manager is
    owned by the caller (declare it as a member, not returned by value).
    MESSAGE-THREAD ONLY. */
void addProductionFormats (juce::AudioPluginFormatManager& formats);
} // namespace arpbox::hosting
