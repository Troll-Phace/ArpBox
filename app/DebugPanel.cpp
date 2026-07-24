// TEMPORARY DEBUG UI — replaced by the real UI in Phase 15+.
#include "DebugPanel.h"

#include "hosting/PluginManager.h"

namespace arpbox::app
{
using namespace juce;

namespace
{
/** Formats a LINEAR amplitude as a dB string for display (handles -inf). */
String linearToDbString (float linear)
{
    return Decibels::toString (Decibels::gainToDecibels (linear), 1);
}

// ── Incremental scan-save throttle (issue #19 stopgap) ────────────────────────
// The vblank persists the KnownPluginList mid-scan after BOTH: the known-type count
// has grown by >= kScanSaveTypeGrowth since the last save, AND at least
// kScanSaveMinIntervalMs has elapsed. The growth gate is 1 (bank ANY new type),
// throttled to at most once per second: a coarser gate (e.g. 20) would defeat
// recovery in the clustered-crasher case — if fewer than the threshold clean
// plugins scan before each hostile one crashes the host, nothing would ever bank
// and every relaunch re-scans the same un-persisted types into the same crash, an
// infinite no-progress loop. At growth==1 a crash loses at most ~1 s of scanning
// and progress always accumulates across relaunches.
constexpr int kScanSaveTypeGrowth = 1;
constexpr std::uint32_t kScanSaveMinIntervalMs = 1000;

/** Maps a device-status level to a human-readable banner string. */
String statusText (std::uint8_t status)
{
    switch (status)
    {
        case engine::deviceStatusFellBackToDefault: return "DEVICE LOST — fell back to default";
        case engine::deviceStatusDead:              return "DEVICE DEAD — no output";
        case engine::deviceStatusOk:
        default:                                    return "DEVICE OK";
    }
}
} // namespace

// MESSAGE-THREAD ONLY.
DebugPanel::DebugPanel (AudioEngine& engine, hosting::PluginManager& plugins, bool& scanForceKilledSinkRef)
    : audioEngine (engine),
      pluginManager (plugins),
      scanForceKilledSink (scanForceKilledSinkRef),
      vblank (this, [this] { refreshFromEngine (); })
{
    // ── Test tone toggle ─────────────────────────────────────────────────────
    addAndMakeVisible (testToneButton);
    testToneButton.onClick = [this]
    {
        pushInt (engine::EngineCommandType::setTestToneEnabled,
                 testToneButton.getToggleState () ? 1 : 0);
    };

    // ── Frequency slider (Hz) ────────────────────────────────────────────────
    addAndMakeVisible (frequencyLabel);
    addAndMakeVisible (frequencySlider);
    frequencySlider.setRange (20.0, 5000.0, 1.0);
    frequencySlider.setSkewFactorFromMidPoint (440.0);
    frequencySlider.setValue (440.0, dontSendNotification);
    frequencySlider.setTextValueSuffix (" Hz");
    frequencySlider.onValueChange = [this]
    {
        pushFloat (engine::EngineCommandType::setTestToneFrequency,
                   static_cast<float> (frequencySlider.getValue ()));
    };

    // ── Master gain slider (dB) ──────────────────────────────────────────────
    addAndMakeVisible (masterGainLabel);
    addAndMakeVisible (masterGainSlider);
    masterGainSlider.setRange (-60.0, 6.0, 0.1);
    masterGainSlider.setValue (0.0, dontSendNotification);
    masterGainSlider.setTextValueSuffix (" dB");
    masterGainSlider.onValueChange = [this]
    {
        pushFloat (engine::EngineCommandType::setMasterGainDb,
                   static_cast<float> (masterGainSlider.getValue ()));
    };

    // ── Safety limiter toggle (default ON, §7) ───────────────────────────────
    addAndMakeVisible (limiterButton);
    limiterButton.setToggleState (true, dontSendNotification);
    limiterButton.onClick = [this]
    {
        pushInt (engine::EngineCommandType::setLimiterEnabled,
                 limiterButton.getToggleState () ? 1 : 0);
    };

    // ── Simulate device loss (dev-only) ──────────────────────────────────────
    addAndMakeVisible (simulateLossButton);
    simulateLossButton.onClick = [this] { audioEngine.simulateDeviceLoss (); };

    // ── Plugin scan trigger (DEV-ONLY; removed with this panel in Phase 15+) ──
    // Runs scanAll() on a BACKGROUND thread (it BLOCKS — never on the message
    // thread). Progress + completion travel back via the cross-thread atomics that
    // refreshFromEngine() reads on the vblank. Real scan UI is Phase 15+/20.
    addAndMakeVisible (scanButton);
    scanButton.onClick = [this] { startPluginScan (); };

    addAndMakeVisible (cancelScanButton);
    cancelScanButton.setEnabled (false);
    cancelScanButton.onClick = [this]
    {
        // Two cancel channels: the manager's own latch and the thread's exit flag
        // (the scan's cancel predicate polls the latter). Either aborts the pass.
        pluginManager.cancelScan ();
        scanThread.signalThreadShouldExit ();
    };

    // ── Readout labels ───────────────────────────────────────────────────────
    for (auto* label :
         { &deviceLabel, &statusBanner, &meterLabel, &blockLabel, &eventLabel, &pluginCountLabel, &scanStatusLabel })
        addAndMakeVisible (*label);

    // Initial known-plugin count reflects the list restored at launch (Main.cpp
    // calls PluginManager::restore() before this panel exists), so a relaunch after
    // a scan shows the persisted count immediately — no scan running here, safe read.
    pluginCountLabel.setText ("known plugins: " + String (pluginManager.getKnownPluginList ().getNumTypes ()),
                              dontSendNotification);
    scanStatusLabel.setText ("scan idle", dontSendNotification);

    deviceLabel.setText (audioEngine.getCurrentDeviceDescription (), dontSendNotification);
    statusBanner.setText (statusText (engine::deviceStatusOk), dontSendNotification);
    statusBanner.setColour (Label::backgroundColourId, Colours::darkgreen);
    meterLabel.setText ("peak L/R: - / -   rms L/R: - / -", dontSendNotification);
    blockLabel.setText ("block: 0", dontSendNotification);
    eventLabel.setText ("events: 0  |  dropped cmds: 0", dontSendNotification);

    // Sync the engine to the initial control values (engine defaults already
    // match, but push so the two never diverge on startup).
    pushFloat (engine::EngineCommandType::setTestToneFrequency, 440.0f);
    pushFloat (engine::EngineCommandType::setMasterGainDb, 0.0f);
    pushInt (engine::EngineCommandType::setLimiterEnabled, 1);
    pushInt (engine::EngineCommandType::setTestToneEnabled, 0);

    setSize (1280, 800);
}

// MESSAGE-THREAD ONLY. Teardown order matters: stop + join the scan worker FIRST
// so it can no longer touch this panel or the borrowed PluginManager, THEN let the
// members (including the vblank) destruct. stopThread signals threadShouldExit
// (the scan's cancel predicate) and waits for run() to return.
DebugPanel::~DebugPanel ()
{
    // stopThread returns true if the worker exited cleanly within the timeout,
    // false if it had to be FORCE-KILLED (hung >5s inside one plugin's in-process
    // scan). A force-kill can terminate the worker mid-mutation of the
    // KnownPluginList — while it holds the list's internal lock — leaving the list
    // locked/inconsistent. Report that up to the app (issue #14): the shutdown
    // save() must then NOT read the list (it would deadlock or serialize corruption).
    // The on-completion save() for normal scans already persisted a clean list.
    scanForceKilledSink = ! scanThread.stopThread (5000);
}

// ── Background plugin scan (DEV-ONLY) ─────────────────────────────────────────

DebugPanel::ScanThread::ScanThread (DebugPanel& ownerPanel)
    : juce::Thread ("arpbox-plugin-scan"), owner (ownerPanel)
{
}

// WORKER THREAD.
void DebugPanel::ScanThread::run ()
{
    owner.runPluginScan ();
}

// MESSAGE-THREAD ONLY.
void DebugPanel::startPluginScan ()
{
    if (scanThread.isThreadRunning ())
        return; // a scan is already in flight

    scanRunning.store (true, std::memory_order_relaxed);
    scanProgress.store (0.0f, std::memory_order_relaxed);
    {
        const ScopedLock sl (scanNameLock);
        currentScanName.clear ();
    }

    // Baseline the incremental-save throttle to "now / current size" so both the
    // growth (>= kScanSaveTypeGrowth new types) and the elapsed-time floor
    // (kScanSaveMinIntervalMs) are measured from the start of THIS scan pass
    // (issue #19 stopgap). getNumTypes() is internally locked — safe to read here
    // (no worker is running yet: we start the thread below).
    lastSavedTypeCount = pluginManager.getKnownPluginList ().getNumTypes ();
    lastSaveTimeMs = Time::getMillisecondCounter ();

    scanButton.setEnabled (false);
    cancelScanButton.setEnabled (true);
    scanStatusLabel.setText ("scanning…", dontSendNotification);

    scanThread.startThread ();
}

// WORKER THREAD. Never touches JUCE components — writes only the cross-thread
// atomics / lock-guarded string that the vblank reads on the message thread.
void DebugPanel::runPluginScan ()
{
    auto onProgress = [this] (const String& pluginBeingScanned, float progress)
    {
        scanProgress.store (progress, std::memory_order_relaxed);
        const ScopedLock sl (scanNameLock);
        currentScanName = pluginBeingScanned;
    };

    auto shouldCancel = [this] { return scanThread.threadShouldExit (); };

    // BLOCKING full pass across every registered format (VST3 + AU). Incremental
    // (rescanExisting = false) so a relaunch + rescan skips known files.
    const auto result = pluginManager.scanAll (/*rescanExisting*/ false, onProgress, shouldCancel);

    lastScannedTypeCount.store (result.numTypesInList, std::memory_order_relaxed);
    lastScanFailedCount.store (result.failedFiles.size (), std::memory_order_relaxed);
    scanRunning.store (false, std::memory_order_relaxed);

    // Release-store: publishes the completed list to the message thread, which then
    // persists it (save() is message-thread only) on the next vblank.
    scanJustFinished.store (true, std::memory_order_release);
}

// MESSAGE-THREAD ONLY.
void DebugPanel::paint (Graphics& g)
{
    g.fillAll (Colours::black);
    g.setColour (Colours::grey);
    g.setFont (16.0f);
    g.drawText ("ARPBOX — Phase 2 debug panel (temporary)",
                getLocalBounds ().removeFromTop (28).reduced (12, 4),
                Justification::centredLeft);
}

// MESSAGE-THREAD ONLY.
void DebugPanel::resized ()
{
    auto area = getLocalBounds ().reduced (12);
    area.removeFromTop (28); // title strip

    const auto row = [&area] (int h) { return area.removeFromTop (h).reduced (0, 4); };

    statusBanner.setBounds (row (32));
    deviceLabel.setBounds (row (24));
    meterLabel.setBounds (row (24));
    blockLabel.setBounds (row (24));
    eventLabel.setBounds (row (24));
    pluginCountLabel.setBounds (row (24));
    scanStatusLabel.setBounds (row (24));

    area.removeFromTop (12);

    testToneButton.setBounds (row (28));

    {
        auto r = row (28);
        frequencyLabel.setBounds (r.removeFromLeft (90));
        frequencySlider.setBounds (r);
    }
    {
        auto r = row (28);
        masterGainLabel.setBounds (r.removeFromLeft (90));
        masterGainSlider.setBounds (r);
    }

    limiterButton.setBounds (row (28));
    area.removeFromTop (12);
    simulateLossButton.setBounds (row (32).removeFromLeft (240));

    area.removeFromTop (12);
    {
        auto r = row (32);
        scanButton.setBounds (r.removeFromLeft (160));
        r.removeFromLeft (8);
        cancelScanButton.setBounds (r.removeFromLeft (160));
    }
}

// MESSAGE-THREAD ONLY (vblank ~60 fps).
void DebugPanel::refreshFromEngine ()
{
    const auto& snapshot = audioEngine.snapshots ().read ();

    // ── Meters (convert LINEAR → dB for display only) ────────────────────────
    meterLabel.setText ("peak L/R: " + linearToDbString (snapshot.peakL)
                            + " / " + linearToDbString (snapshot.peakR)
                            + "   rms L/R: " + linearToDbString (snapshot.rmsL)
                            + " / " + linearToDbString (snapshot.rmsR),
                        dontSendNotification);

    // ── Starvation: blockCounter must advance frame-over-frame ───────────────
    const bool advancing = snapshot.blockCounter != lastBlockCounter;
    lastBlockCounter = snapshot.blockCounter;
    blockLabel.setText ("block: " + String (snapshot.blockCounter)
                            + (advancing ? "" : "   [STARVED]"),
                        dontSendNotification);

    // ── Device-status banner (level field) ───────────────────────────────────
    statusBanner.setText (statusText (snapshot.deviceStatus), dontSendNotification);
    statusBanner.setColour (Label::backgroundColourId,
                            snapshot.deviceStatus == engine::deviceStatusOk ? Colours::darkgreen
                            : snapshot.deviceStatus == engine::deviceStatusFellBackToDefault
                                ? Colours::darkorange
                                : Colours::darkred);

    // Device description can change after a fallback.
    deviceLabel.setText (audioEngine.getCurrentDeviceDescription (), dontSendNotification);

    // ── Drain audio-thread events (we are the sole UI consumer) ──────────────
    audioEngine.events ().drain ([this] (const engine::EngineEvent& e)
    {
        ++engineEventCount;
        lastEventText = "type " + String (static_cast<int> (e.type))
                      + " (a=" + String (e.a) + ", b=" + String (e.b) + ")";
    });

    eventLabel.setText ("events: " + String (engineEventCount)
                            + " [" + lastEventText + "]"
                            + "  |  dropped cmds: "
                            + String (audioEngine.commands ().getDroppedCount ()),
                        dontSendNotification);

    // ── Plugin scan (DEV-ONLY) ───────────────────────────────────────────────
    // Persist on completion HERE, on the message thread: the acquire-load pairs
    // with the worker's release-store, so the KnownPluginList is fully written and
    // no longer being mutated. save() is message-thread only.
    if (scanJustFinished.exchange (false, std::memory_order_acquire))
    {
        pluginManager.save ();
        scanButton.setEnabled (true);
        cancelScanButton.setEnabled (false);
        scanStatusLabel.setText ("scan complete: " + String (lastScannedTypeCount.load (std::memory_order_relaxed))
                                     + " types, " + String (lastScanFailedCount.load (std::memory_order_relaxed))
                                     + " failed",
                                 dontSendNotification);
    }

    if (scanRunning.load (std::memory_order_relaxed))
    {
        // ── Incremental progress persistence (issue #19 stopgap) ─────────────
        // Reading the type count WHILE the worker scans is safe: getNumTypes()
        // takes KnownPluginList's internal typesArrayLock. Persist periodically so
        // a crash mid-scan (a hostile in-process AU segfaulting the host) keeps the
        // types accumulated so far. On the next launch restore() re-blacklists the
        // crasher from the dead-man's-pedal and an incremental rescan skips both the
        // already-scanned files and the blacklisted crasher, letting the user finish
        // across relaunches. Throttled (>= kScanSaveTypeGrowth new types AND
        // >= kScanSaveMinIntervalMs elapsed) so we never save every frame.
        //
        // save() → createXml() takes the same typesArrayLock for the types array,
        // so it is safe to run concurrently with the worker's scanAndAddFile. It
        // also reads the (unlocked) blacklist. That read is race-free today for
        // TWO reasons, BOTH required: (1) our InProcessScanner always returns
        // true, so scanAndAddFile never blacklists on the worker; and (2) the
        // worker DOES call addToBlacklist at the start of each format pass
        // (PluginDirectoryScanner's ctor re-applies the dead-man's-pedal), but
        // Main.cpp's restore() already applied the same pedal file on this thread
        // before any scan could start, and nothing removes blacklist entries
        // mid-session, so those worker-side calls are guaranteed no-ops
        // (contains() short-circuits before the mutating add). If either
        // invariant breaks — e.g. Phase 20's user-overridable blocklist removing
        // an id whose entry is still in the pedal file — this mid-scan save
        // becomes a data race on the blacklist and must be re-gated.
        const int liveCount = pluginManager.getKnownPluginList ().getNumTypes ();
        const std::uint32_t nowMs = Time::getMillisecondCounter ();

        if (liveCount - lastSavedTypeCount >= kScanSaveTypeGrowth
            && nowMs - lastSaveTimeMs >= kScanSaveMinIntervalMs)
        {
            pluginManager.save ();
            lastSavedTypeCount = liveCount;
            lastSaveTimeMs = nowMs;
        }

        // Show progress + the current file, plus the live (accumulating) count so
        // the user can watch progress being banked.
        String name;
        {
            const ScopedLock sl (scanNameLock);
            name = currentScanName;
        }
        scanStatusLabel.setText ("scanning "
                                     + String (roundToInt (scanProgress.load (std::memory_order_relaxed) * 100.0f))
                                     + "%   " + name,
                                 dontSendNotification);
        pluginCountLabel.setText ("known plugins: " + String (liveCount) + " (scanning…)",
                                  dontSendNotification);
    }
    else
    {
        // No worker running => safe to read the live count AND the blacklist. Show
        // the quarantined count next to the type count so the user can see what the
        // dead-man's-pedal blacklisted after a crash-recovery relaunch, and that
        // progress is accumulating (issue #19 stopgap). getBlacklistedFiles()
        // returns a raw, unlocked reference, so only read it off-scan like this.
        const auto& list = pluginManager.getKnownPluginList ();
        pluginCountLabel.setText ("known plugins: " + String (list.getNumTypes ())
                                      + "   quarantined: " + String (list.getBlacklistedFiles ().size ()),
                                  dontSendNotification);
    }
}

// MESSAGE-THREAD ONLY.
void DebugPanel::pushFloat (engine::EngineCommandType type, float value)
{
    engine::EngineCommand command;
    command.type = type;
    command.value.f = value;
    audioEngine.commands ().push (command);
}

// MESSAGE-THREAD ONLY.
void DebugPanel::pushInt (engine::EngineCommandType type, std::int32_t value)
{
    engine::EngineCommand command;
    command.type = type;
    command.value.i = value;
    audioEngine.commands ().push (command);
}
} // namespace arpbox::app
