// TEMPORARY DEBUG UI — replaced by the real UI in Phase 15+.
#include "DebugPanel.h"

#include "engine/sequencer/PatternDocument.h"
#include "engine/sequencer/PatternTypes.h"
#include "engine/sequencer/StepLogic.h"
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

    /** The §12.2 display name of a trig condition, for the DEV-ONLY COND combo.

        THE 30 `A:B` LABELS ARE DERIVED, NOT TYPED. `abCycleFor` (engine/sequencer/
        StepLogic.h) is the engine's own ordinal→(a, b) decode and is `static_assert`ed
        exhaustive over the whole A:B block, so routing the labels through it means the
        combo cannot disagree with the engine about which ordinal is `3:4` — the exact
        class of silent off-by-one issue #62 was about. Only the 8 named conditions and
        `none` are spelled out, because those have no arithmetic to derive them from.

        INDEXED, NOT SWITCHED. A `switch` over `TrigCondition` trips `-Wswitch-enum`
        (warnings are errors here) unless all 39 enumerators are listed explicitly —
        even beside a `default:` — which would mean typing out the 30 A:B cases the
        first line exists to avoid. */
    String trigConditionLabel (engine::TrigCondition cond)
    {
        if (const auto cycle = engine::abCycleFor (cond); cycle.b != 0)
            return String (cycle.a) + ":" + String (cycle.b);

        constexpr int firstNamed = static_cast<int> (engine::TrigCondition::first);
        constexpr int numNamed = static_cast<int> (engine::TrigCondition::notNei) - firstNamed + 1;

        // §12.2's 8 named conditions, in `TrigCondition` ordinal order from `first`.
        static constexpr const char* namedLabels[numNamed] = { "1ST", "!1ST", "FILL", "!FILL",
                                                               "PRE", "!PRE", "NEI",  "!NEI" };

        // A too-LONG initialiser is already a compile error; this catches a too-SHORT
        // one, which would otherwise zero-fill the tail into null `char*`s.
        static_assert (namedLabels[numNamed - 1] != nullptr,
                       "Every named TrigCondition from `first` to `notNei` needs a label here.");

        const int ordinal = static_cast<int> (cond);

        if (ordinal == static_cast<int> (engine::TrigCondition::none))
            return "--";

        if (const int named = ordinal - firstNamed; named >= 0 && named < numNamed)
            return namedLabels[named];

        // Unreachable while the caller stops at `numTrigConditions` and every ordinal
        // below it is either `none`, A:B or named. A future append AFTER `notNei` lands
        // here and shows its ordinal, so the missing label is visible, not a blank item.
        return "cond " + String (ordinal);
    }

    /** Maps a device-status level to a human-readable banner string. */
    String statusText (std::uint8_t status)
    {
        switch (status)
        {
        case engine::deviceStatusFellBackToDefault:
            return "DEVICE LOST — fell back to default";
        case engine::deviceStatusDead:
            return "DEVICE DEAD — no output";
        case engine::deviceStatusOk:
        default:
            return "DEVICE OK";
        }
    }
} // namespace

// MESSAGE-THREAD ONLY.
DebugPanel::DebugPanel (AudioEngine& engine, hosting::PluginManager& plugins, bool& scanForceKilledSinkRef)
    : audioEngine (engine)
    , pluginManager (plugins)
    , scanForceKilledSink (scanForceKilledSinkRef)
    , synthSlot (engine, plugins.getFormatManager ())
    , vblank (this, [this] { refreshFromEngine (); })
{
    // ── Test tone toggle ─────────────────────────────────────────────────────
    addAndMakeVisible (testToneButton);
    testToneButton.onClick = [this]
    { pushInt (engine::EngineCommandType::setTestToneEnabled, testToneButton.getToggleState () ? 1 : 0); };

    // ── Frequency slider (Hz) ────────────────────────────────────────────────
    addAndMakeVisible (frequencyLabel);
    addAndMakeVisible (frequencySlider);
    frequencySlider.setRange (20.0, 5000.0, 1.0);
    frequencySlider.setSkewFactorFromMidPoint (440.0);
    frequencySlider.setValue (440.0, dontSendNotification);
    frequencySlider.setTextValueSuffix (" Hz");
    frequencySlider.onValueChange = [this]
    { pushFloat (engine::EngineCommandType::setTestToneFrequency, static_cast<float> (frequencySlider.getValue ())); };

    // ── Master gain slider (dB) ──────────────────────────────────────────────
    addAndMakeVisible (masterGainLabel);
    addAndMakeVisible (masterGainSlider);
    masterGainSlider.setRange (-60.0, 6.0, 0.1);
    masterGainSlider.setValue (0.0, dontSendNotification);
    masterGainSlider.setTextValueSuffix (" dB");
    masterGainSlider.onValueChange = [this]
    { pushFloat (engine::EngineCommandType::setMasterGainDb, static_cast<float> (masterGainSlider.getValue ())); };

    // ── Safety limiter toggle (default ON, §7) ───────────────────────────────
    addAndMakeVisible (limiterButton);
    limiterButton.setToggleState (true, dontSendNotification);
    limiterButton.onClick = [this]
    { pushInt (engine::EngineCommandType::setLimiterEnabled, limiterButton.getToggleState () ? 1 : 0); };

    // ── Transport (DEV-ONLY; the real header transport lands in Phase 15.3) ───
    // Play/Stop and tempo are plain commands on the canonical queue; the readout
    // comes from the snapshot's transport fields, which the master publishes every
    // block. STOP also rewinds to PPQ 0 (see engine/graph/Transport.h).
    addAndMakeVisible (playButton);
    playButton.onClick = [this] { pushBare (engine::EngineCommandType::transportPlay); };

    addAndMakeVisible (stopButton);
    stopButton.onClick = [this] { pushBare (engine::EngineCommandType::transportStop); };

    addAndMakeVisible (bpmLabel);
    addAndMakeVisible (bpmSlider);
    bpmSlider.setRange (engine::Transport::minBpm, engine::Transport::maxBpm, 0.01);
    bpmSlider.setValue (engine::Transport::defaultBpm, dontSendNotification);
    bpmSlider.setTextValueSuffix (" BPM");
    bpmSlider.onValueChange = [this] { pushDouble (engine::EngineCommandType::setTempoBpm, bpmSlider.getValue ()); };

    // ── Pattern switch (DEV-ONLY; the real 16-pad strip lands in Phase 17.3) ──
    // "Make Audible" is a DOCUMENT edit (message thread, direct — snapshots reach the
    // audio thread by pointer swap); "Queue Switch" is a COMMAND (§3.4 channel 1) the
    // sequencer node resolves to a step index and fires at the chosen boundary.
    addAndMakeVisible (patternSelect);
    for (int i = 0; i < engine::maxPatterns; ++i)
        patternSelect.addItem ("pattern " + String (i), i + 1); // item ids are 1-based
    patternSelect.setSelectedId (2, dontSendNotification);      // pattern 1: the switch target

    addAndMakeVisible (quantizeSelect);
    // Item id == QuantizeMode ordinal + 1 (0 is "nothing selected" in a ComboBox).
    quantizeSelect.addItem ("instant", static_cast<int> (engine::QuantizeMode::instant) + 1);
    quantizeSelect.addItem ("beat", static_cast<int> (engine::QuantizeMode::beat) + 1);
    quantizeSelect.addItem ("bar", static_cast<int> (engine::QuantizeMode::bar) + 1);
    quantizeSelect.addItem ("pattern end", static_cast<int> (engine::QuantizeMode::patternEnd) + 1);
    quantizeSelect.setSelectedId (static_cast<int> (engine::QuantizeMode::bar) + 1, dontSendNotification);

    addAndMakeVisible (switchPatternButton);
    switchPatternButton.onClick = [this] { queueSelectedPatternSwitch (); };

    addAndMakeVisible (fillPatternButton);
    fillPatternButton.onClick = [this] { fillSelectedPattern (); };

    // Every Phase 7 control below writes to whatever `patternSelect` names, so a
    // change of selection has to pull that pattern's actual values back into them.
    patternSelect.onChange = [this] { syncStepLogicControls (); };

    // ── Phase 7 step logic (DEV-ONLY; real UI is Phase 16.3 / 17.1) ───────────
    // Project-level document fields first, then the uniform per-lane writes, then the
    // momentary FILL command. Ranges come from the engine's own constants — never a
    // literal repeated here (see the block comment in DebugPanel.h).
    // The heading text is set by `syncStepLogicControls` because it NAMES the pattern
    // the controls write — see the foot-gun note there.
    addAndMakeVisible (stepLogicHeading);
    stepLogicHeading.setColour (Label::backgroundColourId, Colours::darkslateblue);

    addAndMakeVisible (swingLabel);
    addAndMakeVisible (swingSlider);
    // 50 is EXACTLY straight (`swingShiftSteps` returns bit-zero there), so say so in
    // the readout rather than leaving the user to guess what the bottom of the range
    // means.
    //
    // INSTALLED BEFORE `setRange`, WHICH IS NOT COSMETIC ORDERING. `setRange` clamps
    // the slider's initial 0.0 up into the range and refreshes the text box on the way;
    // the later `setValue (50)` is then a NO-OP (`Slider::setValue` early-returns when
    // the value is unchanged) and refreshes nothing. A formatter attached after either
    // call therefore would not appear until the user first MOVED the slider — so the
    // one value that needs the annotation, the default, is the one value that would
    // never show it. Verified on screen, not reasoned about: it read "50.0" until this
    // moved above `setRange`.
    swingSlider.textFromValueFunction = [] (double value)
    { return value <= engine::minSwingPct ? String ("50 (straight)") : String (value, 1); };
    swingSlider.setRange (engine::minSwingPct, engine::maxSwingPct, 0.5);
    swingSlider.setValue (engine::defaultSwingPct, dontSendNotification);
    swingSlider.onValueChange = [this] { audioEngine.patterns ().setSwing (swingSlider.getValue ()); };

    addAndMakeVisible (ratchetRampLabel);
    addAndMakeVisible (ratchetRampSlider);
    ratchetRampSlider.setRange (engine::minRatchetVelocityRampPct, engine::maxRatchetVelocityRampPct, 1.0);
    ratchetRampSlider.setValue (engine::defaultRatchetVelocityRampPct, dontSendNotification);
    ratchetRampSlider.setTextValueSuffix (" %");
    ratchetRampSlider.onValueChange = [this]
    { audioEngine.patterns ().setRatchetVelocityRamp (ratchetRampSlider.getValue ()); };

    // The four numeric lanes. Each slider's range IS `laneRange (lane)` — bounds are
    // read from the engine, and the step is 1 because every §12.1 lane is integral.
    const auto configureLaneSlider = [this] (Slider& slider, Label& label, engine::LaneId lane)
    {
        const auto range = engine::laneRange (lane);
        addAndMakeVisible (label);
        addAndMakeVisible (slider);
        slider.setRange (range.lo, range.hi, 1.0);
        slider.setValue (engine::laneDefault (lane), dontSendNotification);
        slider.onValueChange = [this, &slider, lane] { writeLaneUniform (lane, roundToInt (slider.getValue ())); };
    };

    configureLaneSlider (ratchetLaneSlider, ratchetLaneLabel, engine::LaneId::ratchet);
    configureLaneSlider (microLaneSlider, microLaneLabel, engine::LaneId::micro);
    configureLaneSlider (probLaneSlider, probLaneLabel, engine::LaneId::prob);
    configureLaneSlider (modALaneSlider, modALaneLabel, engine::LaneId::modA);

    // COND: all 39 §12.2 conditions, in ordinal order. Item id == ordinal + 1, the
    // same convention `quantizeSelect` uses (0 means "nothing selected" in a ComboBox).
    addAndMakeVisible (condLabel);
    addAndMakeVisible (condSelect);
    for (int ordinal = 0; ordinal < engine::numTrigConditions; ++ordinal)
        condSelect.addItem (trigConditionLabel (static_cast<engine::TrigCondition> (ordinal)), ordinal + 1);
    condSelect.setSelectedId (static_cast<int> (engine::TrigCondition::none) + 1, dontSendNotification);
    condSelect.onChange = [this]
    {
        const int ordinal = condSelect.getSelectedId () - 1;
        if (ordinal < 0 || ordinal >= engine::numTrigConditions)
            return; // nothing selected (id 0)

        writeLaneUniform (engine::LaneId::cond, ordinal);
    };

    // FILL — MOMENTARY, deliberately not a ToggleButton. §12.2's FILL is pad 16 HELD,
    // so the flag must follow the mouse button down AND up; `onClick` fires once per
    // completed click and would latch the flag on forever.
    //
    // `onStateChange` + `isDown()` is the momentary hook: JUCE's `Button::updateState`
    // only reports `buttonDown` while the mouse is BOTH down AND over the button, so
    // dragging off a held button drops the flag on its own — which is what stops FILL
    // latching permanently when the mouse leaves mid-press. (It also covers the
    // keyboard press-and-hold path, via `isKeyDown`.) The state message additionally
    // fires on plain hover transitions, hence the de-duplication.
    addAndMakeVisible (fillHeldButton);
    fillHeldButton.onStateChange = [this]
    {
        const bool held = fillHeldButton.isDown ();
        if (held != fillHeldPushed)
            pushFillHeld (held);
    };

    // ── Simulate device loss (dev-only) ──────────────────────────────────────
    addAndMakeVisible (simulateLossButton);
    simulateLossButton.onClick = [this] { audioEngine.simulateDeviceLoss (); };

    // ── On-screen / QWERTY keyboard (note input → engine note FIFO) ──────────
    // Clicks AND computer-keyboard keypresses fire the MidiKeyboardState listener
    // callbacks below, which push NoteEvents onto the lock-free note queue.
    addAndMakeVisible (keyboard);
    keyboardState.addListener (this);

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

    // ── Synth slot (DEV-ONLY; real Sound-column UI is Phase 17) ──────────────
    // load/swap/remove a hosted instrument + gain trim, all through SynthSlot (the
    // async, failure-isolated, click-free-swap coordinator).
    addAndMakeVisible (synthList);
    synthList.setTextWhenNoChoicesAvailable ("no instruments — scan first");
    synthList.setTextWhenNothingSelected ("select an instrument");
    refreshSynthList ();

    addAndMakeVisible (loadSynthButton);
    loadSynthButton.onClick = [this]
    {
        const int idx = synthList.getSelectedItemIndex ();
        if (idx >= 0 && idx < instrumentDescriptions.size ())
            synthSlot.load (instrumentDescriptions.getReference (idx));
    };

    addAndMakeVisible (removeSynthButton);
    removeSynthButton.onClick = [this] { synthSlot.remove (); };

    addAndMakeVisible (synthGainLabel);
    addAndMakeVisible (synthGainSlider);
    synthGainSlider.setRange (-60.0, 6.0, 0.1);
    synthGainSlider.setValue (0.0, dontSendNotification);
    synthGainSlider.setTextValueSuffix (" dB");
    synthGainSlider.onValueChange = [this] { synthSlot.setGainDb (static_cast<float> (synthGainSlider.getValue ())); };

    addAndMakeVisible (synthStatusLabel);
    synthStatusLabel.setText ("synth: none", dontSendNotification);

    // ── Readout labels ───────────────────────────────────────────────────────
    for (auto* label : { &deviceLabel,
                         &statusBanner,
                         &meterLabel,
                         &transportLabel,
                         &voiceLabel,
                         &blockLabel,
                         &eventLabel,
                         &pluginCountLabel,
                         &scanStatusLabel })
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
    transportLabel.setText ("transport: STOPPED   ppq 0.000   120.00 BPM", dontSendNotification);
    voiceLabel.setText ("voices: 0", dontSendNotification);
    blockLabel.setText ("block: 0", dontSendNotification);
    eventLabel.setText ("events: 0  |  dropped cmds: 0", dontSendNotification);

    // Sync the engine to the initial control values (engine defaults already
    // match, but push so the two never diverge on startup).
    pushFloat (engine::EngineCommandType::setTestToneFrequency, 440.0f);
    pushFloat (engine::EngineCommandType::setMasterGainDb, 0.0f);
    pushInt (engine::EngineCommandType::setLimiterEnabled, 1);
    pushInt (engine::EngineCommandType::setTestToneEnabled, 0);
    pushDouble (engine::EngineCommandType::setTempoBpm, engine::Transport::defaultBpm);
    pushFillHeld (false);

    // Pull the initially-selected pattern's step-logic values into the controls. The
    // engine defaults already match `laneDefault`, but going through the same sync path
    // the pattern-change handler uses means there is only ONE place that can be wrong.
    syncStepLogicControls ();

    setSize (1280, 800);
}

// MESSAGE-THREAD ONLY. Teardown order matters: stop + join the scan worker FIRST
// so it can no longer touch this panel or the borrowed PluginManager, THEN let the
// members (including the vblank) destruct. stopThread signals threadShouldExit
// (the scan's cancel predicate) and waits for run() to return.
DebugPanel::~DebugPanel ()
{
    // Detach the keyboard listener first so no late callback pushes notes during
    // teardown.
    keyboardState.removeListener (this);

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
    : juce::Thread ("arpbox-plugin-scan")
    , owner (ownerPanel)
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

    // Release-store, pairing with the vblank's acquire-load in refreshFromEngine().
    // This DIRECTION publishes nothing worker-written (the message thread both writes
    // and reads `true`), but the flag carries one ordering discipline at every site so
    // the pairing is legible where it matters — the worker's release-store of `false`
    // (issue #15).
    scanRunning.store (true, std::memory_order_release);
    scanProgress.store (0.0f, std::memory_order_relaxed);

    // Defensive clear (issue #15): a stale `true` would make the vblank run the
    // completion block — save + re-enable the scan button — WHILE this pass is in
    // flight. It cannot happen today, because scanButton is only ever re-enabled
    // inside that completion block, so reaching this line proves the previous flag was
    // consumed. Belt-and-braces rather than resting on that implicit invariant. Note
    // it is safe even if the invariant broke: a discarded save loses nothing durable,
    // since the list only grows within a session and this pass always ends with
    // another save (runPluginScan sets the flag even on cancel).
    scanJustFinished.store (false, std::memory_order_relaxed);
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
    lastPersistFailed = false; // fresh pass — do not carry a previous pass's marker.

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

    // Plain relaxed stores: they are read only AFTER an acquire on one of the two
    // flags below, and both flags are release-stored after these lines, so the
    // acquiring reader is guaranteed to see them.
    lastScannedTypeCount.store (result.numTypesInList, std::memory_order_relaxed);
    lastScanFailedCount.store (result.failedFiles.size (), std::memory_order_relaxed);

    // Release-store: publishes EVERYTHING this worker wrote — every KnownPluginList
    // mutation made inside scanAll() plus the two counters above — to any thread that
    // observes `false` through an acquire-load. The vblank's "no worker running ⇒ safe
    // to read the live type count AND the unlocked blacklist" branch rests on exactly
    // that happens-before edge; relaxed gave it none, so the comment there claimed a
    // guarantee the code did not provide (issue #15). Sequenced BEFORE the
    // scanJustFinished store below, so a reader that acquires "finished" can never
    // still observe "running".
    scanRunning.store (false, std::memory_order_release);

    // Release-store: publishes the completed list to the message thread, which then
    // persists it (save() is message-thread only) on the next vblank. Paired with the
    // acquire-exchange in refreshFromEngine().
    scanJustFinished.store (true, std::memory_order_release);
}

// ── Pattern switch (DEV-ONLY) ─────────────────────────────────────────────────

// MESSAGE-THREAD ONLY. Pushes `queuePatternSwitch` onto the canonical command queue.
// The sequencer node RECORDS it during the transport head node's drain and resolves
// the boundary at the top of the next block (see SequencerProcessor.h) — the panel
// does no timing arithmetic of its own.
void DebugPanel::queueSelectedPatternSwitch ()
{
    const int patternIndex = patternSelect.getSelectedId () - 1;
    const int quantizeOrdinal = quantizeSelect.getSelectedId () - 1;

    // Nothing selected yet (ComboBox id 0) ⇒ negative here. The sequencer rejects an
    // out-of-range command anyway, but do not push a malformed one.
    if (patternIndex < 0 || patternIndex >= engine::maxPatterns || quantizeOrdinal < 0 ||
        quantizeOrdinal > static_cast<int> (engine::QuantizeMode::patternEnd))
        return;

    pushTargeted (engine::EngineCommandType::queuePatternSwitch,
                  static_cast<std::uint16_t> (patternIndex),
                  static_cast<std::uint32_t> (quantizeOrdinal));
}

// MESSAGE-THREAD ONLY. Direct `PatternDocument` edit — the §4 message-thread edit
// flow, NOT the command queue. The document republishes automatically (the graph
// attached the channel as its publish target), the audio thread adopts the new
// snapshot at its next block head, and the retired one comes back through the
// retirement queue that the vblank reclaims.
//
// WHY THIS BUTTON EXISTS: the default document leaves patterns 1–15 with GATE all
// off, so switching to pattern 1 out of the box produces SILENCE — which cannot tell
// the user whether the switch landed on the bar line or three bars later. This makes
// the destination loudly different from pattern 0's ascending scaffold: every step
// gated, one octave up, descending. Removed with this panel in Phase 15+.
void DebugPanel::fillSelectedPattern ()
{
    const int patternIndex = patternSelect.getSelectedId () - 1;
    if (patternIndex < 0 || patternIndex >= engine::maxPatterns)
        return;

    auto& document = audioEngine.patterns ();

    // One transaction ⇒ one undo entry and ONE snapshot build/publish for the whole
    // gesture, instead of 193 of them (PatternDocument.h). Every step of the lane's
    // 64-slot STORAGE is written, not just the 16 currently active: values at or
    // beyond `length` are stored but never played, so filling them costs nothing and
    // keeps the pattern coherent if the lane is later lengthened.
    document.beginTransaction ();

    for (int step = 0; step < engine::maxSteps; ++step)
    {
        document.setLaneValue (patternIndex, engine::LaneId::gate, step, 1);
        document.setLaneValue (patternIndex, engine::LaneId::oct, step, 1);
        document.setLaneValue (patternIndex, engine::LaneId::vel, step, 120);
    }

    document.setDirection (patternIndex, engine::DirectionMode::down);

    document.endTransaction ();
}

// ── Phase 7 step logic (DEV-ONLY) ─────────────────────────────────────────────

// MESSAGE-THREAD ONLY. A DOCUMENT edit (§3.4 channel 3 / §4's message-thread edit
// flow), identical in kind to `fillSelectedPattern` — mutate the authoritative
// `PatternDocument`, which rebuilds and republishes an immutable `PatternSnapshot`
// that the audio thread adopts at its next block head. Never the command queue,
// never touched from the audio thread, and no fourth cross-thread channel.
//
// THE TRANSACTION IS LOAD-BEARING, NOT TIDINESS. Without it a single slider tick
// would push 64 undo entries and build+publish 64 ~120 KB snapshots, which is a
// VISIBLE stutter while dragging and would flood the retirement queue that the
// vblank drains (`refreshFromEngine`) — a dropped retirement is a leaked snapshot.
// One transaction ⇒ one undo entry, one build, one publish, one retirement.
void DebugPanel::writeLaneUniform (engine::LaneId lane, int value)
{
    const int patternIndex = patternSelect.getSelectedId () - 1;
    if (patternIndex < 0 || patternIndex >= engine::maxPatterns)
        return; // nothing selected (ComboBox id 0), or out of range

    auto& document = audioEngine.patterns ();

    document.beginTransaction ();

    // `setLaneValue` takes the lane's STORAGE index (0..maxSteps-1) and CLAMPS the
    // value into `laneRange (lane)` rather than rejecting it, so the loop cannot fail
    // on a bound the slider and the engine disagree about. Writing all 64 slots (not
    // just `[0, length)`) matches `fillSelectedPattern`: slots at or beyond the lane's
    // length are stored and never played, so they cost nothing and keep the value the
    // sliders read back truthful if the lane is later lengthened.
    for (int step = 0; step < engine::maxSteps; ++step)
        document.setLaneValue (patternIndex, lane, step, value);

    document.endTransaction ();
}

// MESSAGE-THREAD ONLY. Read-only against the document — no edit, no publish.
void DebugPanel::syncStepLogicControls ()
{
    const int patternIndex = patternSelect.getSelectedId () - 1;
    if (patternIndex < 0 || patternIndex >= engine::maxPatterns)
        return;

    const auto& state = audioEngine.patterns ().state ();

    // ── NAME THE PATTERN, BECAUSE THE DEFAULT SELECTION IS A FOOT-GUN ────────
    // `patternSelect` defaults to pattern 1 — it was added in Phase 6 as the SWITCH
    // TARGET — while the transport starts on `startPatternIndex` (pattern 0). So out of
    // the box every control in this block writes a pattern that is NOT sounding, and a
    // user who drags PROB to 0 expecting silence hears nothing change and concludes the
    // control is broken. Spelling both indices out is the cheap fix; the honest fix is
    // the real Phase 17 UI.
    //
    // ASCII ONLY: the em dash in `paint`'s title renders as mojibake in this panel's
    // default font, so a heading that has to be READ uses hyphens and pipes.
    //
    // `startPatternIndex` is where the transport STARTS, not necessarily what is
    // sounding right now — a queued switch (or a `patternEnd` chain) moves the audible
    // pattern and nothing here can see that. `EngineSnapshot` carries no active-pattern
    // field to read (engine/graph/EngineSnapshot.h), and adding one is a snapshot layout
    // change owned by the sequencer, not this throwaway panel.
    stepLogicHeading.setText ("PHASE 7 STEP LOGIC (dev)  |  writes pattern " + String (patternIndex) +
                                  "  |  transport starts on pattern " + String (state.startPatternIndex),
                              dontSendNotification);

    // Project-level fields: NOT per pattern (see the swing / ramp notes in
    // PatternTypes.h), so these two are the same whichever pattern is selected. Synced
    // here anyway so one function is the whole answer to "do the controls match?".
    swingSlider.setValue (state.swingPct, dontSendNotification);
    ratchetRampSlider.setValue (state.ratchetVelocityRampPct, dontSendNotification);

    // Per-lane: step 0 stands for the lane, because `writeLaneUniform` is the only
    // thing that writes these lanes and it writes them uniformly. If a later phase
    // gives the panel a non-uniform writer, this readback becomes a half-truth and
    // needs to show a range instead.
    const auto& pattern = state.patterns[static_cast<std::size_t> (patternIndex)];
    const auto laneStep0 = [&pattern] (engine::LaneId lane) { return engine::laneOf (pattern, lane).values[0]; };

    ratchetLaneSlider.setValue (laneStep0 (engine::LaneId::ratchet), dontSendNotification);
    microLaneSlider.setValue (laneStep0 (engine::LaneId::micro), dontSendNotification);
    probLaneSlider.setValue (laneStep0 (engine::LaneId::prob), dontSendNotification);
    modALaneSlider.setValue (laneStep0 (engine::LaneId::modA), dontSendNotification);

    // COND ordinals are clamped into `laneRange (cond)` on the way in, so this id
    // always names a real item. `dontSendNotification` matters: a notified change here
    // would re-enter `writeLaneUniform` and edit the pattern we are only reading.
    condSelect.setSelectedId (laneStep0 (engine::LaneId::cond) + 1, dontSendNotification);
}

// MESSAGE-THREAD ONLY. A COMMAND (§3.4 channel 1), not document state — FILL changes
// on both press AND release, and carrying it on `PatternSnapshot` would rebuild and
// republish the whole document twice per pad tap (see `EngineCommandType::setFillHeld`).
void DebugPanel::pushFillHeld (bool held)
{
    fillHeldPushed = held;
    pushInt (engine::EngineCommandType::setFillHeld, held ? 1 : 0);
}

// ── Synth slot (DEV-ONLY) ─────────────────────────────────────────────────────

// MESSAGE-THREAD ONLY. Repopulates the combo from the known-plugin list, keeping
// only instruments (synths). `instrumentDescriptions[i]` maps 1:1 to combo item i.
void DebugPanel::refreshSynthList ()
{
    // Remember the currently-selected description so the selection survives a
    // refresh (e.g. after a scan adds more instruments).
    const int prevIndex = synthList.getSelectedItemIndex ();
    const bool hadSelection = prevIndex >= 0 && prevIndex < instrumentDescriptions.size ();
    const String prevIdentifier =
        hadSelection ? instrumentDescriptions.getReference (prevIndex).createIdentifierString () : String {};

    instrumentDescriptions.clearQuick ();
    synthList.clear (dontSendNotification);

    int itemId = 1;
    int restoreIndex = -1;
    for (const auto& desc : pluginManager.getKnownPluginList ().getTypes ())
    {
        if (! desc.isInstrument)
            continue;

        if (hadSelection && desc.createIdentifierString () == prevIdentifier)
            restoreIndex = instrumentDescriptions.size ();

        instrumentDescriptions.add (desc);
        synthList.addItem (desc.name, itemId++);
    }

    if (restoreIndex >= 0)
        synthList.setSelectedItemIndex (restoreIndex, dontSendNotification);
}

// MESSAGE-THREAD ONLY. Reflects the SynthSlot's current name / latency / pending /
// error state in the readout label.
void DebugPanel::refreshSynthStatus ()
{
    String text;
    if (synthSlot.isLoaded ())
        text = "synth: " + synthSlot.getCurrentSynthName () + "  (latency " + String (synthSlot.getLatencySamples ()) +
               " smp)";
    else
        text = "synth: none";

    if (synthSlot.isPending ())
        text += "  [loading…]";

    const auto err = synthSlot.getLastError ();
    if (err.isNotEmpty ())
        text += "  ERROR: " + err;

    synthStatusLabel.setText (text, dontSendNotification);
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
//
// ── TWO COLUMNS SINCE PHASE 7, AND IT HAD TO BECOME TWO ──────────────────────
// The single vertical stack was already at exactly zero slack: summed against the
// 800 px minimum window height (MainWindow's setResizeLimits) the Phase-6 rows left
// the keyboard its 60 px floor and not a pixel more. Phase 7's controls add ~230 px,
// which a one-column stack cannot absorb at the minimum size — the rows above would
// be squeezed to zero height rather than the panel scrolling. So the readouts and the
// existing controls take the left column, the synth slot and the Phase 7 block take
// the right, and the keyboard keeps the full-width strip along the bottom.
void DebugPanel::resized ()
{
    auto content = getLocalBounds ().reduced (12);
    content.removeFromTop (28); // title strip

    // On-screen / QWERTY keyboard: the full-width bottom strip, 60..96 px. Reserved
    // BEFORE the columns are split so it can never be squeezed out by either of them.
    // The floor keeps it playable at the minimum window size; extra height the user
    // drags out widens it up to 96.
    auto keyboardStrip = content.removeFromBottom (jlimit (60, 96, content.getHeight () / 5));
    keyboard.setBounds (keyboardStrip.reduced (0, 4));

    auto area = content.removeFromLeft (content.getWidth () / 2 - 8);
    content.removeFromLeft (16); // gutter
    auto rightColumn = content;

    const auto row = [&area] (int h) { return area.removeFromTop (h).reduced (0, 4); };
    const auto rightRow = [&rightColumn] (int h) { return rightColumn.removeFromTop (h).reduced (0, 4); };

    statusBanner.setBounds (row (32));
    deviceLabel.setBounds (row (24));
    meterLabel.setBounds (row (24));
    transportLabel.setBounds (row (24));
    voiceLabel.setBounds (row (24));
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

    // ── Transport (DEV-ONLY) ─────────────────────────────────────────────────
    area.removeFromTop (12);
    {
        auto r = row (32);
        playButton.setBounds (r.removeFromLeft (120));
        r.removeFromLeft (8);
        stopButton.setBounds (r.removeFromLeft (120));
    }
    {
        auto r = row (28);
        bpmLabel.setBounds (r.removeFromLeft (90));
        bpmSlider.setBounds (r);
    }

    // ── Pattern switch (DEV-ONLY) ────────────────────────────────────────────
    area.removeFromTop (12);
    {
        auto r = row (30);
        patternSelect.setBounds (r.removeFromLeft (140));
        r.removeFromLeft (8);
        quantizeSelect.setBounds (r.removeFromLeft (140));
        r.removeFromLeft (8);
        switchPatternButton.setBounds (r.removeFromLeft (150));
        r.removeFromLeft (8);
        fillPatternButton.setBounds (r.removeFromLeft (150));
    }

    area.removeFromTop (12);
    simulateLossButton.setBounds (row (32).removeFromLeft (240));

    area.removeFromTop (12);
    {
        auto r = row (32);
        scanButton.setBounds (r.removeFromLeft (160));
        r.removeFromLeft (8);
        cancelScanButton.setBounds (r.removeFromLeft (160));
    }

    // ── RIGHT COLUMN ─────────────────────────────────────────────────────────

    // ── Synth slot (DEV-ONLY) ────────────────────────────────────────────────
    synthList.setBounds (rightRow (28));
    {
        auto r = rightRow (28);
        loadSynthButton.setBounds (r.removeFromLeft (160));
        r.removeFromLeft (8);
        removeSynthButton.setBounds (r.removeFromLeft (160));
    }
    {
        auto r = rightRow (28);
        synthGainLabel.setBounds (r.removeFromLeft (90));
        synthGainSlider.setBounds (r);
    }
    synthStatusLabel.setBounds (rightRow (24));

    // ── Phase 7 step logic (DEV-ONLY) ────────────────────────────────────────
    // Grouped under a visible heading so it is obvious at a glance which controls
    // belong to Phase 7 and which pattern they write.
    rightColumn.removeFromTop (12);
    stepLogicHeading.setBounds (rightRow (28));

    const auto labelledRow = [&rightRow] (Label& label, Component& control)
    {
        auto r = rightRow (28);
        label.setBounds (r.removeFromLeft (110));
        control.setBounds (r);
    };

    labelledRow (swingLabel, swingSlider);
    labelledRow (ratchetRampLabel, ratchetRampSlider);

    rightColumn.removeFromTop (8);

    labelledRow (ratchetLaneLabel, ratchetLaneSlider);
    labelledRow (microLaneLabel, microLaneSlider);
    labelledRow (probLaneLabel, probLaneSlider);
    labelledRow (modALaneLabel, modALaneSlider);
    {
        auto r = rightRow (30);
        condLabel.setBounds (r.removeFromLeft (110));
        condSelect.setBounds (r.removeFromLeft (160));
        r.removeFromLeft (12);
        fillHeldButton.setBounds (r.removeFromLeft (160));
    }
}

// MESSAGE-THREAD ONLY (vblank ~60 fps).
void DebugPanel::refreshFromEngine ()
{
    // Advance the synth-slot swap/remove state machine on the UI tick (NOT a
    // juce::Timer): this polls the outgoing node's isFadeOutComplete() handshake and
    // performs the graph edit only once it is genuinely silent.
    synthSlot.poll ();
    refreshSynthStatus ();

    // Drain §3.4 channel 3's return path on the SAME UI tick (again: NOT a
    // juce::Timer). The audio thread retires a superseded `PatternSnapshot` on every
    // adoption and never frees one; this is where those ~100 KB objects actually die.
    // `PatternDocument::publishTo` reclaims too, but only when the user EDITS — a long
    // playback session with no edits would otherwise let the retirement queue fill and
    // start DROPPING (i.e. leaking) retirements. Cheap when empty.
    audioEngine.reclaimRetiredPatterns ();

    const auto& snapshot = audioEngine.snapshots ().read ();

    // ── Meters (convert LINEAR → dB for display only) ────────────────────────
    meterLabel.setText ("peak L/R: " + linearToDbString (snapshot.peakL) + " / " + linearToDbString (snapshot.peakR) +
                            "   rms L/R: " + linearToDbString (snapshot.rmsL) + " / " +
                            linearToDbString (snapshot.rmsR),
                        dontSendNotification);

    // ── Transport (Phase 5.1 snapshot fields; the engine is the source of truth) ──
    transportLabel.setText (String (snapshot.isPlaying ? "transport: PLAYING" : "transport: STOPPED") + "   ppq " +
                                String (snapshot.ppqPosition, 3) + "   " + String (snapshot.bpm, 2) + " BPM",
                            dontSendNotification);

    // ── Live MIDI-in voice count (interim; sequencer owns it in Phase 8) ─────
    voiceLabel.setText ("voices: " + String (snapshot.voiceCount), dontSendNotification);

    // ── Starvation: blockCounter must advance frame-over-frame ───────────────
    const bool advancing = snapshot.blockCounter != lastBlockCounter;
    lastBlockCounter = snapshot.blockCounter;
    blockLabel.setText ("block: " + String (snapshot.blockCounter) + (advancing ? "" : "   [STARVED]"),
                        dontSendNotification);

    // ── Device-status banner (level field) ───────────────────────────────────
    statusBanner.setText (statusText (snapshot.deviceStatus), dontSendNotification);
    statusBanner.setColour (Label::backgroundColourId,
                            snapshot.deviceStatus == engine::deviceStatusOk                  ? Colours::darkgreen
                            : snapshot.deviceStatus == engine::deviceStatusFellBackToDefault ? Colours::darkorange
                                                                                             : Colours::darkred);

    // Device description can change after a fallback.
    deviceLabel.setText (audioEngine.getCurrentDeviceDescription (), dontSendNotification);

    // ── Drain audio-thread events (we are the sole UI consumer) ──────────────
    audioEngine.events ().drain (
        [this] (const engine::EngineEvent& e)
        {
            ++engineEventCount;
            lastEventText =
                "type " + String (static_cast<int> (e.type)) + " (a=" + String (e.a) + ", b=" + String (e.b) + ")";
        });

    // Engine bookkeeping line. `snap dropped` is NOT a statistic: the retirement queue
    // deliberately never frees on the audio thread, so every dropped retirement is a
    // LEAKED PatternSnapshot. It must read 0 — a non-zero value means this tick is not
    // reclaiming fast enough (or is not running at all). `pending` is advisory and is
    // normally 0 or 1 between ticks.
    const auto droppedSnapshots = audioEngine.getDroppedRetirementCount ();

    eventLabel.setText ("events: " + String (engineEventCount) + " [" + lastEventText + "]" +
                            "  |  dropped cmds: " + String (audioEngine.commands ().getDroppedCount ()) +
                            "  |  snap pending: " + String (audioEngine.getNumPendingRetirements ()) +
                            "  dropped: " + String (droppedSnapshots) +
                            (droppedSnapshots != 0 ? String ("  [SNAPSHOT LEAK]") : String ()),
                        dontSendNotification);

    // ── Plugin scan (DEV-ONLY) ───────────────────────────────────────────────
    // Persist on completion HERE, on the message thread: the acquire-load pairs
    // with the worker's release-store, so the KnownPluginList is fully written and
    // no longer being mutated. save() is message-thread only.
    if (scanJustFinished.exchange (false, std::memory_order_acquire))
    {
        // save() logs its own failure (issue #17); the panel's job is to make it
        // VISIBLE, since a user whose list silently fails to persist just loses the
        // scan. No DBG here — it compiles out under NDEBUG.
        const auto saved = pluginManager.save ();
        lastPersistFailed = ! saved.wasOk ();

        refreshSynthList (); // newly-scanned instruments can now be loaded.
        scanButton.setEnabled (true);
        cancelScanButton.setEnabled (false);
        scanStatusLabel.setText ("scan complete: " + String (lastScannedTypeCount.load (std::memory_order_relaxed)) +
                                     " types, " + String (lastScanFailedCount.load (std::memory_order_relaxed)) +
                                     " failed" +
                                     (lastPersistFailed ? String ("   [SAVE FAILED — see log]") : String ()),
                                 dontSendNotification);
    }

    // Acquire-load, pairing with the worker's release-store of `false` in
    // runPluginScan: whichever branch we take below, we have a real happens-before on
    // everything the worker wrote (issue #15).
    if (scanRunning.load (std::memory_order_acquire))
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

        if (liveCount - lastSavedTypeCount >= kScanSaveTypeGrowth && nowMs - lastSaveTimeMs >= kScanSaveMinIntervalMs)
        {
            // save() logs the failure itself (issue #17); here we only react to it.
            const auto saved = pluginManager.save ();
            lastPersistFailed = ! saved.wasOk ();

            // ALWAYS advance the time floor, so a persistently failing write retries at
            // most once per kScanSaveMinIntervalMs instead of hammering the disk every
            // vblank. Advance the banked count ONLY on success: a failed save has
            // banked nothing, so leaving lastSavedTypeCount behind keeps the growth
            // gate satisfied and the next interval retries.
            lastSaveTimeMs = nowMs;

            if (saved.wasOk ())
                lastSavedTypeCount = liveCount;
        }

        // Show progress + the current file, plus the live (accumulating) count so
        // the user can watch progress being banked.
        String name;
        {
            const ScopedLock sl (scanNameLock);
            name = currentScanName;
        }
        scanStatusLabel.setText (
            "scanning " + String (roundToInt (scanProgress.load (std::memory_order_relaxed) * 100.0f)) + "%   " + name +
                (lastPersistFailed ? String ("   [SAVE FAILED — see log]") : String ()),
            dontSendNotification);
        pluginCountLabel.setText ("known plugins: " + String (liveCount) + " (scanning…)", dontSendNotification);
    }
    else
    {
        // No worker running => safe to read the live count AND the blacklist. That
        // now holds for a real reason: the acquire-load above synchronises-with the
        // worker's release-store of scanRunning == false, so every list mutation the
        // worker made happens-before this read and the worker is provably done
        // mutating. (getNumTypes() would be internally locked either way; the
        // blacklist read is NOT locked, so it is this edge — not the lock — that makes
        // reading getBlacklistedFiles() here safe. Only ever read it off-scan.) Show
        // the quarantined count next to the type count so the user can see what the
        // dead-man's-pedal blacklisted after a crash-recovery relaunch, and that
        // progress is accumulating (issue #19 stopgap).
        const auto& list = pluginManager.getKnownPluginList ();
        pluginCountLabel.setText ("known plugins: " + String (list.getNumTypes ()) +
                                      "   quarantined: " + String (list.getBlacklistedFiles ().size ()),
                                  dontSendNotification);
    }
}

// MESSAGE-THREAD ONLY. On-screen click or QWERTY keypress → engine note queue.
void DebugPanel::handleNoteOn (juce::MidiKeyboardState*, int midiChannel, int midiNoteNumber, float velocity)
{
    audioEngine.pushNoteOn (midiChannel, midiNoteNumber, jlimit (1, 127, roundToInt (velocity * 127.0f)));
}

// MESSAGE-THREAD ONLY.
void DebugPanel::handleNoteOff (juce::MidiKeyboardState*, int midiChannel, int midiNoteNumber, float)
{
    audioEngine.pushNoteOff (midiChannel, midiNoteNumber);
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

// MESSAGE-THREAD ONLY.
void DebugPanel::pushDouble (engine::EngineCommandType type, double value)
{
    engine::EngineCommand command;
    command.type = type;
    command.value.d = value;
    audioEngine.commands ().push (command);
}

// MESSAGE-THREAD ONLY.
void DebugPanel::pushBare (engine::EngineCommandType type)
{
    engine::EngineCommand command; // value stays default-initialised (unused)
    command.type = type;
    audioEngine.commands ().push (command);
}

// MESSAGE-THREAD ONLY.
void DebugPanel::pushTargeted (engine::EngineCommandType type, std::uint16_t targetId, std::uint32_t value)
{
    engine::EngineCommand command;
    command.type = type;
    command.targetId = targetId;
    command.value.u = value;
    audioEngine.commands ().push (command);
}
} // namespace arpbox::app
