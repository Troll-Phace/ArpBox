#pragma once

#include "ISynthEngine.h"

#include "engine/graph/DeviceStatus.h"
#include "engine/graph/EngineCommand.h"
#include "engine/graph/EngineEvent.h"
#include "engine/graph/EngineGraph.h"
#include "engine/graph/EngineSnapshotBuffer.h"
#include "engine/graph/NoteEvent.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace arpbox::app
{
/** App-side audio backbone (ARCHITECTURE §3.2, §3.3, §9).

    Owns the `AudioDeviceManager` (CoreAudio), the `AudioProcessorPlayer` that
    drives the root `AudioProcessorGraph`, and the `EngineGraph` that assembles
    that graph plus the three canonical cross-thread channels. This is the ONLY
    place the app touches the device layer; the UI never does (§10.1).

    LIFETIME / DESTRUCTION ORDER (critical — see the member declarations and the
    destructor): the device manager must stop calling into the player, and the
    player must stop referencing the graph, BEFORE any of them is destroyed. The
    destructor does this teardown explicitly, and the member declaration order is
    chosen so the implicit member destruction that follows is also safe.

    DEVICE-DEATH HANDLING (§9): the audio device can die (unplug, driver error)
    from the CoreAudio thread. This class registers a lightweight *secondary*
    `AudioIODeviceCallback` (in addition to the player) purely to observe device
    health. On a device error the callback does the minimum RT-safe work — set an
    atomic and `triggerAsyncUpdate()` — and the actual reopen/fallback runs later
    on the MESSAGE thread in `handleAsyncUpdate()`. Nothing reopens a device from
    the audio callback. The resulting health level is surfaced through
    `EngineSnapshot.deviceStatus` (a level field the audio thread copies into every
    snapshot) — NOT pushed onto `events()`, whose sole producer is the audio thread.

    HARDWARE MIDI INPUT (§9): all available MIDI input devices are enabled and this
    class registers as a `juce::MidiInputCallback`. Each incoming message is
    forwarded (off the MIDI thread) into the graph's shared `MidiMessageCollector`,
    which the MIDI-In graph node drains on the audio thread. QWERTY/pad notes take a
    separate lock-free path: `pushNoteOn`/`pushNoteOff` post PODs onto the graph's
    `NoteEventQueue`, drained by the same MIDI-In node. The shared collector's
    sample rate is configured by the MIDI-In node's `prepareToPlay` (driven by the
    player when the device starts), so this class deliberately never calls
    `collector.reset()` — that would race the audio-thread drain.

    MESSAGE-THREAD ONLY unless a method is explicitly marked otherwise. */
class AudioEngine final : public ISynthEngine,
                          private juce::AudioIODeviceCallback,
                          private juce::AsyncUpdater,
                          private juce::ChangeListener,
                          private juce::MidiInputCallback
{
public:
    // MESSAGE-THREAD ONLY: constructs the graph, loads persisted device settings,
    // opens the device, and starts audio. Never blocks on the audio thread.
    /** Builds the engine graph, restores the persisted device/buffer/SR selection
        (or the system default output if none is saved), hosts the graph in the
        player, and begins the audio callback. */
    AudioEngine ();

    // MESSAGE-THREAD ONLY: stops audio and tears everything down in a
    // use-after-free-safe order (see the .cpp for the exact sequence).
    /** Persists the current device state, detaches callbacks, closes the device,
        and destroys members graph-last. */
    ~AudioEngine () override;

    // ── UI / DebugPanel endpoints (forward to the graph) ─────────────────────

    // MESSAGE-THREAD ONLY (producer side of the UI→engine channel).
    /** UI-writable command queue; drained by the engine every block. */
    engine::EngineCommandQueue& commands () noexcept { return graph.commands (); }

    // MESSAGE-THREAD ONLY (consumer side of the engine→UI state channel).
    /** Snapshot triple buffer; call `.read()` from the UI at frame rate. */
    engine::EngineSnapshotBuffer& snapshots () noexcept { return graph.snapshots (); }

    // MESSAGE-THREAD ONLY (consumer side of the engine→UI event channel).
    /** Discrete engine→UI event queue (audio thread is the only producer). */
    engine::EngineEventQueue& events () noexcept { return graph.events (); }

    // ── Pattern model (§3.4 channel 3; Phase 6) ──────────────────────────────

    // MESSAGE-THREAD ONLY (producer side of the pattern channel). Every committed
    // edit rebuilds an immutable `PatternSnapshot` and publishes it automatically —
    // the graph attached the channel as the document's publish target at build time.
    // Document edits do NOT go through the command queue; only the quantized pattern
    // SWITCH does (`EngineCommandType::queuePatternSwitch`).
    /** The editable pattern model (message-thread authoritative state). */
    engine::PatternDocument& patterns () noexcept { return graph.patterns (); }

    // MESSAGE-THREAD ONLY: must be called on the UI tick — see the note on
    // `EngineGraph::reclaimRetiredPatterns`. Snapshots the audio thread retired are
    // freed HERE, never on the audio thread.
    /** Frees all retired pattern snapshots. */
    void reclaimRetiredPatterns () noexcept { graph.reclaimRetiredPatterns (); }

    // MESSAGE-THREAD ONLY: observation, for the debug readout.
    /** Retired pattern snapshots awaiting reclamation (advisory). */
    int getNumPendingRetirements () const noexcept { return graph.getNumPendingRetirements (); }

    // MESSAGE-THREAD ONLY: observation, for the debug readout. A non-zero value means
    // a LEAKED snapshot — see `EngineGraph::getDroppedRetirementCount`.
    /** Pattern retirements dropped because the retirement queue was full. */
    std::uint64_t getDroppedRetirementCount () const noexcept { return graph.getDroppedRetirementCount (); }

    // ── QWERTY/pad note input (message thread → note FIFO → MIDI-In node) ─────

    // MESSAGE-THREAD ONLY (producer side of the note channel §3.3). Pushes a POD
    // NoteEvent; fully lock-free. Values are clamped to valid MIDI ranges.
    /** Queues a note-on (channel 1..16, note 0..127, velocity 1..127). */
    void pushNoteOn (int channel, int note, int velocity)
    {
        engine::NoteEvent e;
        e.kind = engine::NoteEventKind::noteOn;
        e.channel = static_cast<std::uint8_t> (juce::jlimit (1, 16, channel));
        e.note = static_cast<std::uint8_t> (juce::jlimit (0, 127, note));
        e.velocity = static_cast<std::uint8_t> (juce::jlimit (1, 127, velocity));
        graph.notes ().push (e);
    }

    // MESSAGE-THREAD ONLY (producer side of the note channel §3.3).
    /** Queues a note-off (channel 1..16, note 0..127). */
    void pushNoteOff (int channel, int note)
    {
        engine::NoteEvent e;
        e.kind = engine::NoteEventKind::noteOff;
        e.channel = static_cast<std::uint8_t> (juce::jlimit (1, 16, channel));
        e.note = static_cast<std::uint8_t> (juce::jlimit (0, 127, note));
        e.velocity = 0;
        graph.notes ().push (e);
    }

    // MESSAGE-THREAD ONLY: request an all-notes-off flush on the MIDI-in path.
    /** Flushes held MIDI-in notes (CC123 on all channels); a swap/removal primitive. */
    void allNotesOff () override { graph.allNotesOff (); }

    // MESSAGE-THREAD ONLY: set the MIDI-input channel filter (bit i ⇒ channel i+1).
    /** Sets the 16-bit MIDI-input channel mask (default all-pass). */
    void setMidiChannelMask (std::uint16_t mask) { graph.setMidiChannelMask (mask); }

    // ── Synth slot (seam for the plugin-slot coordinator, Delegation C) ───────

    // MESSAGE-THREAD ONLY: hand a PREPARED synth instance to the engine's single
    // instrument slot (wired MIDI-In → synth → Master). The engine takes ownership;
    // prepare the instance at getCurrentSampleRate()/getCurrentBlockSize() first.
    /** Sets/swaps the hosted synth (base `juce::AudioProcessor`). */
    void setSynth (std::unique_ptr<juce::AudioProcessor> synth) override { graph.setSynth (std::move (synth)); }

    // MESSAGE-THREAD ONLY: removes the current synth.
    /** Removes the hosted synth from the instrument slot. */
    void removeSynth () override { graph.removeSynth (); }

    // MESSAGE-THREAD ONLY: current graph audio config for preparing a synth instance.
    /** Current graph sample rate (Hz); 0 before the device is open. */
    double getCurrentSampleRate () const noexcept override { return graph.getSampleRate (); }
    /** Current graph block size (samples); 0 before the device is open. */
    int getCurrentBlockSize () const noexcept override { return graph.getBlockSize (); }

    // MESSAGE-THREAD ONLY: exposes the device manager for a settings UI (later
    // phases) and tests.
    /** The underlying `AudioDeviceManager`. */
    juce::AudioDeviceManager& getDeviceManager () noexcept { return deviceManager; }

    // MESSAGE-THREAD ONLY: read-only, cached; safe to call every UI frame.
    /** Human-readable "name — SR Hz / buffer smp" description of the active
        device, or a "no device" string when none is open. */
    juce::String getCurrentDeviceDescription () const { return deviceDescription; }

    // MESSAGE-THREAD ONLY (dev/debug): forces the device-death → fallback path so
    // the DebugPanel can exercise it without unplugging hardware.
    /** Simulates loss of the current audio device: schedules an async fallback to
        the default output device on the message thread. */
    void simulateDeviceLoss ();

private:
    // ── AudioIODeviceCallback (secondary, health-monitoring only) ────────────

    // AUDIO-CALLBACK THREAD: contributes SILENCE (the player produces the audio).
    // This secondary callback exists only so device-error notifications reach us.
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;

    // AUDIO-CALLBACK THREAD (may be called from the device thread): RT-safe only.
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;

    // AUDIO-CALLBACK THREAD: RT-safe only.
    void audioDeviceStopped () override;

    // AUDIO-CALLBACK THREAD: a genuine driver error. RT-safe: flag + async wake.
    void audioDeviceError (const juce::String& errorMessage) override;

    // ── AsyncUpdater (message thread) ────────────────────────────────────────

    // MESSAGE-THREAD ONLY: performs the device fallback flagged from the callback.
    void handleAsyncUpdate () override;

    // ── ChangeListener (message thread) ──────────────────────────────────────

    // MESSAGE-THREAD ONLY: the device setup changed (user pick / auto-restart) —
    // re-persist and refresh the cached description.
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;

    // ── MidiInputCallback (MIDI-input thread) ────────────────────────────────

    // MIDI-INPUT THREAD (a per-device high-priority thread, NOT the audio or
    // message thread): forwards each hardware MIDI message into the graph's shared
    // collector (internally synchronized), which the MIDI-In node drains on the
    // audio thread. Does the minimum — no allocation beyond the collector's own.
    void handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& message) override;

    // ── Internal helpers (message thread) ────────────────────────────────────

    // MESSAGE-THREAD ONLY: initial device open from persisted state or default.
    void initialiseDevice ();

    // MESSAGE-THREAD ONLY: reopen the default output device and set the resulting
    // status level (fell-back or dead). Guarded by `fallbackInProgress`.
    void performFallback ();

    // MESSAGE-THREAD ONLY: path to the persisted settings XML (creates the dir).
    juce::File getSettingsFile () const;

    // MESSAGE-THREAD ONLY: write `createStateXml()` to the settings file.
    void saveDeviceState ();

    // MESSAGE-THREAD ONLY: rebuild the cached `deviceDescription` string.
    void refreshDeviceDescription ();

    // MESSAGE-THREAD ONLY: sets the device-status LEVEL. The single writer of the
    // status — updates the message-thread-tracked copy (`currentDeviceStatus`) AND
    // forwards to the graph so the audio thread surfaces it in every snapshot.
    void setDeviceStatus (engine::DeviceStatus status) noexcept;

    // Cross-thread device-death signalling. Set on the audio/device thread, read
    // and cleared on the message thread in handleAsyncUpdate().
    std::atomic<bool> deviceErrored { false };      ///< A genuine driver error fired.
    std::atomic<bool> simulatedLoss { false };      ///< simulateDeviceLoss() requested.
    std::atomic<bool> fallbackInProgress { false }; ///< Debounces re-entrant fallback.

    // MESSAGE-THREAD ONLY: bounded-fallback retry state, touched only inside
    // handleAsyncUpdate(). Caps reopen churn from a persistently-failing default
    // device to kMaxFallbackAttempts attempts per kFallbackWindowMs rolling window
    // (window-elapse is the reset). `fallbackWindowActive` is an EXPLICIT
    // "window open" flag (issue #11) — it replaces the old `fallbackWindowStartMs
    // == 0` sentinel, which could collide with a legitimate zero millisecond counter
    // (boot instant / wrap tick) and permit one extra retry burst.
    int fallbackAttemptsInWindow { 0 };
    bool fallbackWindowActive { false };
    juce::uint32 fallbackWindowStartMs { 0 };

    // MESSAGE-THREAD ONLY: current device-status LEVEL (mirror of what the graph
    // holds), so changeListenerCallback can tell it is upgrading FROM FellBackToDefault.
    engine::DeviceStatus currentDeviceStatus { engine::deviceStatusOk };

    // MESSAGE-THREAD ONLY: name of the device the auto-fallback selected. A later
    // device change to a DIFFERENT healthy device (a genuine user re-selection, not
    // the fallback's own change broadcast) clears the stale FellBackToDefault banner
    // (issue #10). Empty when no fallback has occurred.
    juce::String fallbackDeviceName;

    // Cached, message-thread-only readout of the active device.
    juce::String deviceDescription { "No audio device" };

    // ── Members: declaration order is DESTRUCTION order in reverse ───────────
    // Destroyed bottom-to-top => deviceManager first (stops audio + detaches),
    // then player, then graph LAST (its nodes hold pointers into graph-owned
    // channels; nothing may reference the graph after it is gone). The destructor
    // also performs this teardown explicitly before member destruction runs.
    engine::EngineGraph graph;              ///< Destroyed LAST.
    juce::AudioProcessorPlayer player;      ///< Destroyed after deviceManager.
    juce::AudioDeviceManager deviceManager; ///< Destroyed FIRST.

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
} // namespace arpbox::app
