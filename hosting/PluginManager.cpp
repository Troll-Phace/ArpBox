#include "hosting/PluginManager.h"

#include <memory>

namespace arpbox::hosting
{
using namespace juce;

// MESSAGE-THREAD ONLY.
PluginManager::PluginManager (AudioPluginFormatManager& formatManager, File settingsDirectory)
    : formats (formatManager)
    , settingsDir (std::move (settingsDirectory))
{
    // Register the ONE discovery seam. Ownership moves into the list. Every scan
    // below runs through PluginDirectoryScanner, which calls into this custom
    // scanner (KnownPluginList::scanAndAddFile delegates to it).
    knownList.setCustomScanner (std::make_unique<InProcessScanner> ());

    // One-time migration of pre-#13 persisted files from the historical location to
    // the corrected one (see migrateLegacySettingsIfNeeded). Runs BEFORE restore()
    // (Main.cpp calls restore() after construction), so the migrated files are the
    // ones read at launch. No-op for a custom (test) settings dir.
    migrateLegacySettingsIfNeeded ();
}

PluginManager::~PluginManager () = default;

// ── Search paths ─────────────────────────────────────────────────────────────

// MESSAGE-THREAD ONLY.
void PluginManager::addUserSearchPath (const File& directory)
{
    if (directory.isDirectory ())
        userSearchPaths.addIfNotAlreadyThere (directory);
}

// MESSAGE-THREAD ONLY.
FileSearchPath PluginManager::getSearchPathsForFormat (AudioPluginFormat& format) const
{
    // Start from the format's own defaults (VST3 → the two standard VST3 dirs; AU
    // → empty, since it enumerates via the system component registry regardless of
    // the path). Then append user paths (honoured by file-based formats only).
    FileSearchPath path = format.getDefaultLocationsToSearch ();

    for (int i = 0; i < userSearchPaths.getNumPaths (); ++i)
        path.addIfNotAlreadyThere (userSearchPaths[i]);

    return path;
}

// ── Scanning ─────────────────────────────────────────────────────────────────

// BLOCKING — SCANNING (WORKER) THREAD. Never the audio thread.
PluginManager::ScanResult PluginManager::scanFormat (AudioPluginFormat& format,
                                                     bool rescanExisting,
                                                     const ProgressCallback& onProgress,
                                                     const CancelCallback& shouldCancel)
{
    // Public entry: clear any stale cancel from a previous scan, then run the loop.
    cancelRequested.store (false, std::memory_order_relaxed);
    return scanOneFormat (format, rescanExisting, onProgress, shouldCancel);
}

// BLOCKING — SCANNING (WORKER) THREAD. Never the audio thread. Does NOT reset the
// cancel latch (callers do), so scanAll() can honour a cancel raised mid-pass.
PluginManager::ScanResult PluginManager::scanOneFormat (AudioPluginFormat& format,
                                                        bool rescanExisting,
                                                        const ProgressCallback& onProgress,
                                                        const CancelCallback& shouldCancel)
{
    ScanResult result;

    const FileSearchPath path = getSearchPathsForFormat (format);

    // deadMansPedalFile: PluginDirectoryScanner records the file it is ABOUT to
    // scan here and clears it on success. A crash mid-scan leaves the offender's
    // identifier behind; restore() re-applies it as a blacklisting. Phase 3 plumbs
    // this; Phase 20's subprocess scanner makes it an actual crash guard.
    //
    // allowPluginsWhichRequireAsynchronousInstantiation == false: AUv3 (async-only)
    // is post-MVP and behind a flag. VST3 + AUv2 do not require it. Flip to true
    // when AUv3 hosting lands.
    PluginDirectoryScanner directoryScanner (knownList,
                                             format,
                                             path,
                                             /*searchRecursively*/ true,
                                             getDeadMansPedalFile (),
                                             /*allowAsync*/ false);

    String nameBeingScanned;

    for (;;)
    {
        // Cancel at file granularity: our own latch (cancelScan) or the caller's
        // predicate. Discovery of a single file is a non-blocking call, so file
        // granularity is the finest cancellation the in-process path needs.
        if (cancelRequested.load (std::memory_order_relaxed) || (shouldCancel && shouldCancel ()))
            break;

        // dontRescanIfAlreadyInList == !rescanExisting: incremental scans skip
        // known files (mirrors pluginNeedsRescanning); a full rescan forces reload.
        const bool moreToScan = directoryScanner.scanNextFile (! rescanExisting, nameBeingScanned);

        if (onProgress)
            onProgress (nameBeingScanned, directoryScanner.getProgress ());

        if (! moreToScan)
            break;
    }

    result.failedFiles = directoryScanner.getFailedFiles ();
    result.numTypesInList = knownList.getNumTypes ();
    return result;
}

// BLOCKING — SCANNING (WORKER) THREAD. Never the audio thread.
PluginManager::ScanResult PluginManager::scanAll (bool rescanExisting,
                                                  const ProgressCallback& onProgress,
                                                  const CancelCallback& shouldCancel)
{
    ScanResult aggregate;

    // Clear any stale cancel ONCE for the whole multi-format pass; scanOneFormat
    // does not reset it, so a cancel raised while scanning format N still aborts
    // format N+1.
    cancelRequested.store (false, std::memory_order_relaxed);

    const int numFormats = formats.getNumFormats ();
    for (int i = 0; i < numFormats; ++i)
    {
        if (cancelRequested.load (std::memory_order_relaxed) || (shouldCancel && shouldCancel ()))
            break;

        auto* format = formats.getFormat (i);
        if (format == nullptr)
            continue;

        const ScanResult one = scanOneFormat (*format, rescanExisting, onProgress, shouldCancel);
        aggregate.failedFiles.addArray (one.failedFiles);
    }

    aggregate.numTypesInList = knownList.getNumTypes ();
    return aggregate;
}

// Any non-audio thread. Sticky until the next scan begins.
void PluginManager::cancelScan () noexcept
{
    cancelRequested.store (true, std::memory_order_relaxed);
}

// ── Persistence ──────────────────────────────────────────────────────────────

// MESSAGE-THREAD ONLY. Mirrors AudioEngine::saveDeviceState.
void PluginManager::save () const
{
    if (auto xml = knownList.createXml ()) // null only if the list is empty-and-untouched
        xml->writeTo (getPluginListFile ());
}

// MESSAGE-THREAD ONLY. Mirrors AudioEngine::initialiseDevice restore path.
void PluginManager::restore ()
{
    if (auto xml = parseXML (getPluginListFile ()))
        knownList.recreateFromXml (*xml);

    // Re-apply any plugins the dead-man's-pedal recorded as crash-on-scan so a
    // known-bad plugin stays blacklisted across launches (§6.1). Harmless when the
    // file is absent/empty.
    PluginDirectoryScanner::applyBlacklistingsFromDeadMansPedal (knownList, getDeadMansPedalFile ());
}

// ── Paths ────────────────────────────────────────────────────────────────────

// MESSAGE-THREAD ONLY. Mirrors AudioEngine::getSettingsFile (creates the dir).
File PluginManager::getSettingsDirectory () const
{
    // An explicit (test) directory wins; otherwise resolve the standard ARPBOX
    // app-data dir — identical to AudioEngine so both write to one place. On macOS
    // userApplicationDataDirectory maps to ~/Library, so we descend into
    // "Application Support/ARPBOX" (issue #13: the old code stopped at
    // ~/Library/ARPBOX, contradicting §6.1 and AudioEngine).
    auto dir = settingsDir != File ()
                 ? settingsDir
                 : File::getSpecialLocation (File::userApplicationDataDirectory)
                       .getChildFile ("Application Support")
                       .getChildFile ("ARPBOX");

    if (! dir.isDirectory ())
        dir.createDirectory ();

    return dir;
}

// MESSAGE-THREAD ONLY. One-time, non-destructive migration of the pre-#13 settings
// location (~/Library/ARPBOX — the historical userApplicationDataDirectory child)
// to the corrected ~/Library/Application Support/ARPBOX. The user can hold hard-won
// data there (a large scanned plugin list plus MANUAL blacklists), and a rescan can
// hit known crash-on-instantiate plugins, so this must preserve it: each known file
// is COPIED only when the destination does not yet exist, and the originals are
// LEFT IN PLACE as a safety fallback (never moved or deleted). No-op when a custom
// (test) settings dir is in use — we never touch the user's real files for a test.
void PluginManager::migrateLegacySettingsIfNeeded () const
{
    if (settingsDir != File ())
        return; // custom/test dir — do not migrate the user's real legacy files.

    const auto legacyDir = File::getSpecialLocation (File::userApplicationDataDirectory)
                               .getChildFile ("ARPBOX");
    const auto currentDir = getSettingsDirectory (); // ensures the new dir exists.

    // Defensive: if a platform ever maps both to the same place, or the legacy dir
    // was never created, there is nothing to migrate.
    if (legacyDir == currentDir || ! legacyDir.isDirectory ())
        return;

    for (const auto* fileName : { "plugin-list.xml", "plugin-list.xml.bak", "plugin-deadmanspedal" })
    {
        const auto legacyFile = legacyDir.getChildFile (fileName);
        const auto currentFile = currentDir.getChildFile (fileName);

        // Only fill a gap: never overwrite a file already at the new location, and
        // never move/delete the original.
        if (legacyFile.existsAsFile () && ! currentFile.exists ())
        {
            // A failed copy must NOT leave a partial destination: on macOS a mid-copy
            // failure is not guaranteed to clean up, and a truncated plugin-list.xml /
            // plugin-deadmanspedal would both (1) block this migration forever next
            // launch (currentFile.exists() would be true) and (2) silently degrade the
            // quarantine blacklist — risking an in-process instantiation of a known
            // crash-on-scan plugin. So on failure delete the partial dest and let the
            // next launch retry cleanly. The legacy source is untouched regardless.
            if (! legacyFile.copyFileTo (currentFile))
                currentFile.deleteFile ();
        }
    }
}

// MESSAGE-THREAD ONLY.
File PluginManager::getPluginListFile () const
{
    return getSettingsDirectory ().getChildFile ("plugin-list.xml");
}

// MESSAGE-THREAD ONLY.
File PluginManager::getDeadMansPedalFile () const
{
    return getSettingsDirectory ().getChildFile ("plugin-deadmanspedal");
}

// ── Production format helper ──────────────────────────────────────────────────

// MESSAGE-THREAD ONLY.
void addProductionFormats (AudioPluginFormatManager& formats)
{
    // Registers VST3 + AudioUnit on macOS, WITH editor/UI support (ARCHITECTURE
    // §6.1, §6.4). In JUCE 8 `AudioPluginFormatManager::addDefaultFormats()` is
    // deleted; the UI-bearing replacement is the free function below (the headless
    // engine would use addHeadlessDefaultFormatsToManager instead). The manager
    // stays caller-owned so the DI seam in PluginManager remains explicit.
    addDefaultFormatsToManager (formats);
}
} // namespace arpbox::hosting
