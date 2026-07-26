// TEMPORARY DEBUG UI — replaced by the real UI in Phase 15+.
#pragma once

#include "AudioEngine.h"
#include "SynthSlot.h"
#include "engine/sequencer/PatternTypes.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <cstdint>

namespace arpbox::hosting
{
// Forward decl for the DEV-ONLY plugin-scan trigger below (borrowed, not owned).
class PluginManager;
} // namespace arpbox::hosting

namespace arpbox::app
{
/** Throwaway Phase-2 debug surface (INSTRUCTIONS.md Phase 2 success criteria):
    proves the test tone runs through the graph to the device and shows live
    meter values / device status in a plain readout.

    Deliberately crude default-JUCE look — no `ui/Tokens.h` (that arrives in
    Phase 15). It talks to the engine ONLY through the canonical channels (§10.1):
    it PUSHES `EngineCommand`s and READS the `EngineSnapshot`/event queue; it never
    touches audio-thread state directly.

    The 60 fps read uses `juce::VBlankAttachment`, NOT `juce::Timer` (prohibited —
    see .claude/rules/code-style.md).

    A `juce::MidiKeyboardComponent` gives on-screen click AND computer-keyboard
    (QWERTY) note input: its `MidiKeyboardState::Listener` callbacks push
    `NoteEvent`s onto the engine's lock-free note queue (message thread → note FIFO
    → MIDI-In node), the same path the real pad/QWERTY UI uses in Phase 17.

    MESSAGE-THREAD ONLY. */
class DebugPanel final : public juce::Component, private juce::MidiKeyboardState::Listener
{
public:
    // MESSAGE-THREAD ONLY: builds the controls and starts the vblank read.
    /** @param engine  the app audio backbone (non-owning; must outlive this).
        @param plugins the app plugin manager, for the DEV-ONLY plugin-scan
                       trigger (non-owning; must outlive this).
        @param scanForceKilledSink app-owned flag set true in the destructor if the
                       background scan worker had to be FORCE-KILLED at teardown
                       (see ~DebugPanel). Must outlive this panel — the app reads it
                       after destroying the window to decide whether its shutdown
                       save() is safe. */
    DebugPanel (AudioEngine& engine, hosting::PluginManager& plugins, bool& scanForceKilledSink);

    // MESSAGE-THREAD ONLY: stops + joins the background scan thread BEFORE the
    // borrowed PluginManager can be destroyed, then tears down.
    ~DebugPanel () override;

    /** Lays the debug controls out in a simple vertical stack. */
    void resized () override;

    /** Paints the plain background. */
    void paint (juce::Graphics& g) override;

private:
    // MESSAGE-THREAD ONLY (vblank): reads the newest snapshot, drains events, and
    // refreshes the meter / status / starvation / voice readouts.
    void refreshFromEngine ();

    // ── MidiKeyboardState::Listener (message thread) ─────────────────────────
    // On-screen keyboard / QWERTY note-on/off → push onto the engine note queue.

    // MESSAGE-THREAD ONLY: queues a note-on for the pressed key.
    void handleNoteOn (juce::MidiKeyboardState* source, int midiChannel, int midiNoteNumber, float velocity) override;

    // MESSAGE-THREAD ONLY: queues a note-off for the released key.
    void handleNoteOff (juce::MidiKeyboardState* source, int midiChannel, int midiNoteNumber, float velocity) override;

    // MESSAGE-THREAD ONLY: repopulates the synth combo from the known-plugin list,
    // filtered to instruments (isInstrument). Called at construction and after a
    // scan completes.
    void refreshSynthList ();

    // MESSAGE-THREAD ONLY: refreshes the synth-slot status readout (name / latency /
    // pending / error) from the SynthSlot.
    void refreshSynthStatus ();

    // MESSAGE-THREAD ONLY: kicks off the background plugin scan (dev-only trigger).
    void startPluginScan ();

    // WORKER THREAD (ScanThread::run): runs the BLOCKING scanAll, then flags the
    // message thread to persist the list. Never touches JUCE components directly —
    // it writes only the cross-thread atomics / lock-guarded string below.
    void runPluginScan ();

    // MESSAGE-THREAD ONLY: pushes a float-payload command onto the queue.
    void pushFloat (engine::EngineCommandType type, float value);

    // MESSAGE-THREAD ONLY: pushes an int-payload command onto the queue.
    void pushInt (engine::EngineCommandType type, std::int32_t value);

    // MESSAGE-THREAD ONLY: pushes a double-payload command onto the queue. Tempo and
    // locate targets need full precision — they feed the transport's exact PPQ
    // arithmetic (engine/graph/Transport.h), so a float round-trip is not acceptable.
    void pushDouble (engine::EngineCommandType type, double value);

    // MESSAGE-THREAD ONLY: pushes a command whose payload is unused (transport
    // play/stop).
    void pushBare (engine::EngineCommandType type);

    // MESSAGE-THREAD ONLY: pushes a command carrying BOTH a target id and an unsigned
    // payload. `queuePatternSwitch` is the first such command (targetId = destination
    // pattern index, value.u = QuantizeMode).
    void pushTargeted (engine::EngineCommandType type, std::uint16_t targetId, std::uint32_t value);

    // MESSAGE-THREAD ONLY (DEV-ONLY): queues the selected pattern switch at the
    // selected quantize boundary. The COMMAND path (§3.4 channel 1).
    void queueSelectedPatternSwitch ();

    // MESSAGE-THREAD ONLY (DEV-ONLY): rewrites the selected pattern into something
    // audibly distinct from the default pattern-0 scaffold, so a quantized switch can
    // actually be HEARD landing on its boundary. The DOCUMENT path (§3.4 channel 3) —
    // a direct `PatternDocument` edit, never the command queue, and never from audio.
    void fillSelectedPattern ();

    // ── Phase 7 step logic (DEV-ONLY) ────────────────────────────────────────

    // MESSAGE-THREAD ONLY (DEV-ONLY): writes `value` into EVERY storage step of
    // `lane` in the selected pattern, as ONE transaction — one undo entry and ONE
    // snapshot build/publish for the whole gesture instead of 64 of each (see
    // PatternDocument::beginTransaction). The DOCUMENT path (§3.4 channel 3), exactly
    // like `fillSelectedPattern` — never the command queue, never from audio.
    //
    // UNIFORM ACROSS THE LANE IS THE POINT: one value everywhere makes a single §12.1
    // feature audible on its own, instead of mixed into a hand-drawn contour nobody
    // can hear through. All `maxSteps` STORAGE slots are written, not just the active
    // `[0, length)` — matching `fillSelectedPattern`, so the value the sliders read
    // back stays truthful if a lane is later lengthened.
    void writeLaneUniform (engine::LaneId lane, int value);

    // MESSAGE-THREAD ONLY (DEV-ONLY): re-reads the selected pattern's step-logic lane
    // values (plus the project-level swing / ratchet ramp) back into the controls, so a
    // control can never silently describe a different pattern than the one it writes.
    // Called at construction and whenever `patternSelect` changes. Reads step 0 of each
    // lane, which IS the whole lane's value while only this panel writes it.
    void syncStepLogicControls ();

    // MESSAGE-THREAD ONLY (DEV-ONLY): pushes the momentary FILL flag (§12.2 pad 16).
    // A COMMAND (§3.4 channel 1), NOT a document edit — see `EngineCommandType::setFillHeld`.
    void pushFillHeld (bool held);

    AudioEngine& audioEngine;              ///< Non-owning; owned by the application.
    hosting::PluginManager& pluginManager; ///< Non-owning; owned by the application.
    bool& scanForceKilledSink;             ///< App-owned; set true iff the scan worker was force-killed at teardown.

    // ── Controls (push commands) ─────────────────────────────────────────────
    juce::ToggleButton testToneButton { "Test Tone" };
    juce::Slider frequencySlider;  ///< Tone frequency in Hz.
    juce::Slider masterGainSlider; ///< Master gain in dB.
    juce::ToggleButton limiterButton { "Safety Limiter" };
    juce::TextButton simulateLossButton { "Simulate Device Loss" };

    // ── Transport (DEV-ONLY; the real header transport is Phase 15.3) ─────────
    juce::TextButton playButton { "Play" };
    juce::TextButton stopButton { "Stop" };
    juce::Slider bpmSlider; ///< Tempo in BPM (20..300, §12.1 / Transport).
    juce::Label bpmLabel { {}, "BPM" };

    // ── Pattern switch (DEV-ONLY scaffolding; real pad strip is Phase 17.3) ───
    // Exercises BOTH Phase-6 paths from the UI: the quantized switch COMMAND and a
    // crude `PatternDocument` edit that makes the destination pattern audible (the
    // default document leaves patterns 1–15 GATE-off, i.e. silent, so switching to
    // one would prove nothing about where the switch landed).
    juce::ComboBox patternSelect;  ///< Destination pattern 0..15.
    juce::ComboBox quantizeSelect; ///< instant | beat | bar | patternEnd.
    juce::TextButton switchPatternButton { "Queue Switch" };
    juce::TextButton fillPatternButton { "Make Audible" };

    // ── Phase 7 step logic (DEV-ONLY; removed with this panel in Phase 15+) ───
    // Without these the Phase 7 engine surface — ratchets, micro-timing, swing,
    // probability, the 39 trig conditions and FILL — is UNREACHABLE from the app: the
    // default document leaves every one of those lanes at `laneDefault`, which is
    // precisely the "as if Phase 7 never landed" configuration. The real lane strip is
    // Phase 16.3 and the real pad-16 FILL is Phase 17.3; this is the interim way to
    // HEAR any of it.
    //
    // THREE DIFFERENT CHANNELS SIT IN THIS BLOCK — do not collapse them:
    //   * Swing / ratchet ramp   → PROJECT-LEVEL document fields (one value, whole
    //                              project — see the swing note in PatternTypes.h).
    //   * RATCHET/MICRO/PROB/COND/MOD A → per-step LANES, written uniformly across the
    //                              selected pattern. Also document edits.
    //   * FILL                   → a momentary COMMAND (§3.4 channel 1). Not state.
    //
    // Every slider takes its bounds from `laneRange (…)` / the §8.1 swing + ramp
    // constants rather than repeating a literal, so a range change in PatternTypes.h
    // cannot silently desync this panel from what the engine accepts.
    juce::Label stepLogicHeading;

    juce::Slider swingSlider;                          ///< Project swing %, 50 (straight) .. 75.
    juce::Label swingLabel { {}, "Swing %" };          ///< 50 = straight.
    juce::Slider ratchetRampSlider;                    ///< Project ratchet velocity ramp %, -100..+100 (0 = flat).
    juce::Label ratchetRampLabel { {}, "Ratch Ramp" }; ///< Applied to the LAST ratchet child.

    juce::Slider ratchetLaneSlider;                    ///< RATCHET lane, laneRange = 1..8.
    juce::Label ratchetLaneLabel { {}, "RATCHET" };    ///< Children per step.
    juce::Slider microLaneSlider;                      ///< MICRO lane, laneRange = -50..+50 % step.
    juce::Label microLaneLabel { {}, "MICRO %" };      ///< Swing composes on top.
    juce::Slider probLaneSlider;                       ///< PROB lane, laneRange = 0..100 %.
    juce::Label probLaneLabel { {}, "PROB %" };        ///< 100 consumes no randomness.
    juce::Slider modALaneSlider;                       ///< MOD A lane, laneRange = 0..127.
    juce::Label modALaneLabel { {}, "MOD A" };         ///< >= 64 is what NEI/!NEI reads (D7).
    juce::ComboBox condSelect;                         ///< COND lane: all 39 §12.2 conditions.
    juce::Label condLabel { {}, "COND" };              ///< Gates BEFORE the probability roll.
    juce::TextButton fillHeldButton { "FILL (hold)" }; ///< MOMENTARY — pad 16, not a toggle.

    /** Last FILL state pushed as a command. `Button::onStateChange` also fires for
        hover transitions (buttonNormal ↔ buttonOver), which must not push a spurious
        release; this de-duplicates so only real down/up edges reach the queue. */
    bool fillHeldPushed = false;

    // ── On-screen / QWERTY keyboard (note input → engine note FIFO) ──────────
    juce::MidiKeyboardState keyboardState;
    juce::MidiKeyboardComponent keyboard { keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard };

    // ── Plugin scan (DEV-ONLY trigger; removed with this panel in Phase 15+) ──
    juce::TextButton scanButton { "Scan Plugins" };
    juce::TextButton cancelScanButton { "Cancel Scan" };

    juce::Label frequencyLabel { {}, "Tone Hz" };
    juce::Label masterGainLabel { {}, "Gain dB" };

    // ── Readouts (from snapshots/events) ─────────────────────────────────────
    juce::Label deviceLabel;      ///< Active device name / SR / buffer.
    juce::Label statusBanner;     ///< Device-status banner (OK / fell back / dead).
    juce::Label meterLabel;       ///< Peak / RMS in dB.
    juce::Label transportLabel;   ///< PPQ / BPM / play state (snapshot transport fields).
    juce::Label voiceLabel;       ///< Live MIDI-in voice count (snapshot.voiceCount).
    juce::Label blockLabel;       ///< Block counter + STARVED indicator.
    juce::Label eventLabel;       ///< Last drained engine event + dropped-command count.
    juce::Label pluginCountLabel; ///< Known-plugin count (confirms list restored across launches).
    juce::Label scanStatusLabel;  ///< Scan idle / progress % + current file / last result.

    // ── Synth slot (DEV-ONLY; real Sound-column UI is Phase 17) ──────────────
    // Load/swap/remove a hosted instrument + gain trim, driven through SynthSlot.
    juce::ComboBox synthList; ///< Instruments from the known-plugin list.
    juce::TextButton loadSynthButton { "Load Synth" };
    juce::TextButton removeSynthButton { "Remove Synth" };
    juce::Slider synthGainSlider; ///< Synth output-gain trim (dB).
    juce::Label synthGainLabel { {}, "Synth dB" };
    juce::Label synthStatusLabel; ///< Current synth name / latency / error.

    // Backing store for `synthList`: index i ⇒ the i-th instrument item's description.
    juce::Array<juce::PluginDescription> instrumentDescriptions;

    std::uint64_t lastBlockCounter = 0; ///< For starvation detection frame-over-frame.
    std::uint64_t engineEventCount = 0; ///< Total engine events drained since launch.
    juce::String lastEventText { "none" };

    // ── Incremental scan-progress persistence (issue #19 stopgap) ────────────
    // MESSAGE-THREAD ONLY throttle state for the periodic save() the vblank runs
    // WHILE a scan is in flight, so a crash mid-scan (hostile in-process AU)
    // preserves the types accumulated so far across a relaunch. Only ever touched
    // on the message thread (startPluginScan + the vblank refresh), so no atomics.
    int lastSavedTypeCount = 0;       ///< Known-type count at the last periodic save.
    std::uint32_t lastSaveTimeMs = 0; ///< Time::getMillisecondCounter() at the last periodic save.

    // True iff the most recent PluginManager::save() attempt failed (issue #17).
    // PluginManager logs the failure durably; this drives the VISIBLE marker in the
    // scan status line, so a user whose list is not persisting finds out before losing
    // the scan. Message thread only.
    bool lastPersistFailed = false;

    // ── Background plugin scan (DEV-ONLY) ────────────────────────────────────
    // scanAll() BLOCKS, so it runs on this worker thread, NEVER the message thread.
    // Cooperative-cancel via threadShouldExit() (the scan's cancel predicate);
    // stopped + joined in ~DebugPanel before the borrowed PluginManager dies.
    class ScanThread final : public juce::Thread
    {
    public:
        explicit ScanThread (DebugPanel& ownerPanel);
        void run () override;

    private:
        DebugPanel& owner;
    };
    ScanThread scanThread { *this };

    // Cross-thread scan state: the worker WRITES, the vblank (message thread) READS.
    //
    // ORDERING (issue #15): both flags are RELEASE-stored by the worker and
    // ACQUIRE-loaded by the vblank. That pairing is what publishes the worker's
    // KnownPluginList mutations to the message thread — without it, the vblank could
    // observe "finished"/"not running" with no happens-before on the list the worker
    // built, then persist or display it. The remaining fields are relaxed on purpose:
    // they are only ever read after one of the two flag acquires, which orders them.
    std::atomic<bool> scanRunning { false };      ///< Worker is inside scanAll(). Release/acquire.
    std::atomic<bool> scanJustFinished { false }; ///< Worker finished; message thread must save(). Release/acquire.
    std::atomic<float> scanProgress { 0.0f };     ///< [0,1] progress of the current pass.
    std::atomic<int> lastScannedTypeCount { 0 };  ///< Types in the list after the last pass.
    std::atomic<int> lastScanFailedCount { 0 };   ///< Files that failed to load in the last pass.
    juce::CriticalSection scanNameLock;           ///< Guards currentScanName across threads (non-audio).
    juce::String currentScanName;                 ///< Plugin file currently being scanned.

    // Message-thread synth-slot coordinator (owns the async load + swap state
    // machine). Declared BEFORE the vblank so it is fully constructed before the
    // 60 fps tick (which calls synthSlot.poll()) can fire, and destroyed AFTER it.
    SynthSlot synthSlot;

    // 60 fps snapshot read (message thread). Declared LAST so it is destroyed
    // FIRST — the callback must never fire after the members it touches are gone.
    juce::VBlankAttachment vblank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DebugPanel)
};
} // namespace arpbox::app
