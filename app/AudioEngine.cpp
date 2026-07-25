#include "AudioEngine.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <cmath>

namespace arpbox::app
{
using namespace juce;

// The engine is output-only in Phase 2: 0 input channels, 2 output channels.
static constexpr int kNumInputChannels = 0;
static constexpr int kNumOutputChannels = 2;

// Device-fallback retry policy (MESSAGE THREAD — wall-clock time is fine here).
// Without a cap, a default device that opens and then immediately re-errors can
// drive unbounded full `initialiseWithDefaultDevices` reopen cycles on the
// message thread (UI jank + device churn). We bound the churn: at most
// kMaxFallbackAttempts reopen attempts inside a rolling kFallbackWindowMs window;
// once that many have occurred within the window we stop retrying and declare the
// device dead until the window elapses (the cool-down). A trigger that arrives
// after the window has elapsed opens a fresh window and retries again.
static constexpr int kMaxFallbackAttempts = 3;
static constexpr juce::uint32 kFallbackWindowMs = 10000;

// MESSAGE-THREAD ONLY.
AudioEngine::AudioEngine ()
{
    // 1. Open the audio device (persisted selection, else system default output).
    initialiseDevice ();

    // 2. Host the engine graph in the player. The player prepares and processes
    //    the graph from the device callback — do NOT call graph.prepareToPlay()
    //    here (that headless path would double-prepare the nodes).
    player.setProcessor (&graph.getProcessor ());

    // 3. Register callbacks. ORDER MATTERS: the player is added FIRST so it is
    //    callback index 0 and writes the real device output buffer; the health
    //    monitor (this) is added second and only observes device state.
    deviceManager.addAudioCallback (&player);
    deviceManager.addAudioCallback (this);
    deviceManager.addChangeListener (this);

    // 3b. Hardware MIDI input (§9). Enable every available input device and
    //     register a global callback (empty identifier = all enabled inputs). Each
    //     message is forwarded into the graph's shared collector on the MIDI thread;
    //     the MIDI-In node drains it on the audio thread. Its sample rate is set by
    //     the node's prepareToPlay (driven above by addAudioCallback(&player) →
    //     audioDeviceAboutToStart), so we do NOT reset the collector here.
    for (const auto& input : MidiInput::getAvailableDevices ())
        deviceManager.setMidiInputDeviceEnabled (input.identifier, true);
    deviceManager.addMidiInputDeviceCallback ({}, this);

    // 4. Reflect the opened device into the snapshot + cached readout.
    refreshDeviceDescription ();
    setDeviceStatus (deviceManager.getCurrentAudioDevice () != nullptr ? engine::deviceStatusOk
                                                                       : engine::deviceStatusDead);
}

// MESSAGE-THREAD ONLY.
// Teardown order is the whole point of this destructor: a wrong order is a
// use-after-free on quit. We must (a) stop the async fallback ever running
// again, (b) stop the device calling into the player, (c) stop the player
// referencing the graph, (d) close the device — THEN let members destruct
// (deviceManager first, graph last, per declaration order).
AudioEngine::~AudioEngine ()
{
    // (a) No fallback may fire mid-teardown.
    cancelPendingUpdate ();

    // Persist the current selection while the device is still open.
    saveDeviceState ();

    // (b) Detach observers/callbacks. Remove the MIDI callback first so no MIDI
    //     thread can push into the graph's collector after this; then the monitor
    //     and the player so no device thread can touch either after this point.
    deviceManager.removeMidiInputDeviceCallback ({}, this);
    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (this);
    deviceManager.removeAudioCallback (&player);

    // (c) Player must not reference the graph once callbacks are gone.
    player.setProcessor (nullptr);

    // (d) Close the hardware. (The deviceManager destructor would do this too,
    //     but doing it explicitly here keeps the ordering obvious and testable.)
    deviceManager.closeAudioDevice ();

    // Implicit member destruction now runs bottom-to-top: deviceManager, player,
    // graph — graph last, as required.
}

// ── Device open / persistence ────────────────────────────────────────────────

// MESSAGE-THREAD ONLY.
void AudioEngine::initialiseDevice ()
{
    const auto settingsFile = getSettingsFile ();

    String error;
    if (auto savedState = parseXML (settingsFile))
    {
        // Restore the persisted device/buffer/SR; fall back to the default device
        // automatically if the saved device is no longer available.
        error = deviceManager.initialise (kNumInputChannels,
                                          kNumOutputChannels,
                                          savedState.get (),
                                          /*selectDefaultDeviceOnFailure*/ true);
    }
    else
    {
        error = deviceManager.initialiseWithDefaultDevices (kNumInputChannels, kNumOutputChannels);
    }

    // A non-empty error means even the default device could not be opened; the
    // app stays alive (silent) and the DebugPanel banner reports the dead state.
    jassert (error.isEmpty () || deviceManager.getCurrentAudioDevice () == nullptr);
    ignoreUnused (error);

    // Whatever CoreAudio (or the persisted XML) landed on, it must be inside the
    // supported envelope before we let the graph render at it (issue #56). Runs
    // before the constructor evaluates the device-status level, so a refused device
    // is already closed by the time that level is chosen.
    enforceSampleRateCeiling ();
}

// Identity a clamp is remembered against. `AudioIODevice` exposes no stable UID, so
// type name + device name is the strongest key JUCE offers; two identically-named
// devices of the same type are indistinguishable here (and would need the same clamp
// anyway, so the annotation stays true either way).
static String deviceKeyFor (AudioIODevice& device)
{
    return device.getTypeName () + "/" + device.getName ();
}

// MESSAGE-THREAD ONLY. Is the recorded clamp still a TRUE statement about the device
// as it is running right now? Same device, still at the rate we clamped it to.
bool AudioEngine::clampStillApplies (juce::AudioIODevice& device) const
{
    return clampedFromSampleRate > 0.0 && deviceKeyFor (device) == clampedDeviceKey &&
           std::abs (device.getCurrentSampleRate () - clampedToSampleRate) < 1.0;
}

// MESSAGE-THREAD ONLY. Drop the clamp record, and with it the readout annotation.
// Deliberately does NOT touch `refusedSampleRate` — that annotation has its own
// lifetime (it describes a device we closed, i.e. one that is not open to match).
void AudioEngine::forgetClamp () noexcept
{
    clampedFromSampleRate = 0.0;
    clampedToSampleRate = 0.0;
    clampedDeviceKey.clear ();
}

// MESSAGE-THREAD ONLY. See the header for the contract; the arithmetic behind the
// ceiling itself lives on `engine::kMaxSupportedSampleRate`.
bool AudioEngine::enforceSampleRateCeiling ()
{
    auto* device = deviceManager.getCurrentAudioDevice ();

    if (device == nullptr || ! device->isOpen ())
    {
        // Nothing open: a clamp recorded against a device that is no longer running
        // has stopped being true, so forget it (issue #68). `refusedSampleRate` is
        // preserved — the refuse path below closes the device precisely so that
        // annotation is what the readout shows.
        forgetClamp ();
        return false;
    }

    const double negotiated = device->getCurrentSampleRate ();

    if (negotiated <= engine::kMaxSupportedSampleRate)
    {
        // The common path — and, crucially, the path the clamp's OWN asynchronous
        // change broadcast re-enters a message-queue turn later (issue #68). A
        // compliant rate on its own cannot distinguish "compliant BECAUSE we just
        // clamped it" from "compliant of its own accord", so this must not clear
        // unconditionally: keep the annotation exactly while the recorded clamp still
        // describes THIS device at THIS rate, and drop it otherwise (different
        // device, or a different rate on the same device).
        if (! clampStillApplies (*device))
            forgetClamp ();

        refusedSampleRate = 0.0;
        return false;
    }

    // Highest rate the device offers that we can actually support. `<=` not `<`:
    // exactly 192 kHz is supported (the 0.864-sample worst case is inside the bound).
    double target = 0.0;
    for (const double rate : device->getAvailableSampleRates ())
        if (rate <= engine::kMaxSupportedSampleRate && rate > target)
            target = rate;

    if (target > 0.0)
    {
        // ONE attempt. The driver has the last word, so the result is re-read below
        // rather than assumed — that is what keeps this free of a retry loop when a
        // device refuses the change and re-broadcasts.
        auto setup = deviceManager.getAudioDeviceSetup ();
        setup.sampleRate = target;
        deviceManager.setAudioDeviceSetup (setup, /*treatAsChosenDevice*/ true);
    }

    device = deviceManager.getCurrentAudioDevice ();

    if (device != nullptr && device->isOpen () && device->getCurrentSampleRate () <= engine::kMaxSupportedSampleRate)
    {
        // Record the clamp against the DEVICE and the rate it landed on, not as a
        // bare flag (issue #68): that is what lets the compliant-path branch above
        // tell our own clamp apart from a device that never needed one, so the
        // annotation outlives the change broadcast this very call provoked.
        clampedFromSampleRate = negotiated;
        clampedToSampleRate = device->getCurrentSampleRate ();
        clampedDeviceKey = deviceKeyFor (*device);
        refusedSampleRate = 0.0;

        Logger::writeToLog ("ARPBOX: audio device negotiated " + String (negotiated / 1000.0, 1) +
                            " kHz, above the supported ceiling of " +
                            String (engine::kMaxSupportedSampleRate / 1000.0, 1) + " kHz; clamped to " +
                            String (device->getCurrentSampleRate () / 1000.0, 1) + " kHz.");
        return true;
    }

    // REFUSE. The device offers nothing we support (or would not move). Running at
    // this rate would silently break the sequencer's sub-sample emission bound
    // (issue #37), which is a wrong-timing failure the user cannot see or diagnose —
    // so we close the device instead. Silence with a hard error banner is the honest
    // outcome; the caller's status evaluation sees no device and reports
    // `deviceStatusDead`.
    forgetClamp ();
    refusedSampleRate = negotiated;

    Logger::writeToLog ("ARPBOX: audio device runs at " + String (negotiated / 1000.0, 1) +
                        " kHz and offers no rate at or below the supported ceiling of " +
                        String (engine::kMaxSupportedSampleRate / 1000.0, 1) + " kHz; device closed.");

    deviceManager.closeAudioDevice ();
    return true;
}

// MESSAGE-THREAD ONLY.
File AudioEngine::getSettingsFile () const
{
    // ARCHITECTURE §6.1: persist under ~/Library/Application Support/ARPBOX (issue
    // #13). On macOS `userApplicationDataDirectory` is ~/Library, so we must descend
    // into "Application Support/ARPBOX" explicitly. PluginManager uses the IDENTICAL
    // path so both land in the same directory.
    auto dir = File::getSpecialLocation (File::userApplicationDataDirectory)
                   .getChildFile ("Application Support")
                   .getChildFile ("ARPBOX");
    if (! dir.isDirectory ())
        dir.createDirectory ();

    auto settingsFile = dir.getChildFile ("audio-device-settings.xml");

    // One-time forward migration (issue #13): earlier builds wrote to the wrong
    // ~/Library/ARPBOX. If the new-path file is absent but the legacy one exists,
    // COPY it forward. Leave the legacy file in place as a safety fallback — do NOT
    // move or delete it. Once the new file exists this is a no-op.
    if (! settingsFile.existsAsFile ())
    {
        auto legacyFile = File::getSpecialLocation (File::userApplicationDataDirectory)
                              .getChildFile ("ARPBOX")
                              .getChildFile ("audio-device-settings.xml");
        if (legacyFile.existsAsFile ())
        {
            // Check the copy result (issue #13 robustness): a partial copy can leave
            // a truncated destination that then blocks future retries (existsAsFile
            // becomes true). On failure, delete the partial file so the next launch
            // retries the migration cleanly.
            if (! legacyFile.copyFileTo (settingsFile))
                settingsFile.deleteFile ();
        }
    }

    return settingsFile;
}

// MESSAGE-THREAD ONLY.
void AudioEngine::saveDeviceState ()
{
    if (auto state = deviceManager.createStateXml ()) // null when nothing to save
        state->writeTo (getSettingsFile ());
}

// MESSAGE-THREAD ONLY.
void AudioEngine::refreshDeviceDescription ()
{
    // The ceiling annotations (issue #56) ride the device readout so the clamp is
    // visible wherever the device is: the DebugPanel shows this string every frame,
    // and later phases' settings UI reads the same accessor. A clamp the user cannot
    // see is a clamp they will report as a bug.
    const auto ceilingText = String (engine::kMaxSupportedSampleRate / 1000.0, 1) + " kHz";

    if (auto* device = deviceManager.getCurrentAudioDevice ())
    {
        deviceDescription = device->getName () + " — " + String (device->getCurrentSampleRate () / 1000.0, 1) + " kHz" +
                            " / " + String (device->getCurrentBufferSizeSamples ()) + " smp";

        if (clampedFromSampleRate > 0.0)
        {
            deviceDescription += " (clamped from " + String (clampedFromSampleRate / 1000.0, 1) +
                                 " kHz — ARPBOX supports up to " + ceilingText + ")";
        }
    }
    else if (refusedSampleRate > 0.0)
    {
        deviceDescription = "Unsupported device — " + String (refusedSampleRate / 1000.0, 1) +
                            " kHz exceeds the supported ceiling of " + ceilingText;
    }
    else
    {
        deviceDescription = "No audio device";
    }
}

// MESSAGE-THREAD ONLY.
void AudioEngine::setDeviceStatus (engine::DeviceStatus status) noexcept
{
    currentDeviceStatus = status;
    graph.setDeviceStatus (status);
}

// ── Device-death detection (audio/device thread) ─────────────────────────────

// AUDIO-CALLBACK THREAD. This secondary callback contributes silence: for any
// callback after index 0 the device manager hands us a private scratch output
// buffer and ADDS our result to the real output, so we must clear it (leaving it
// untouched would add stale/garbage samples). No device work happens here.
void AudioEngine::audioDeviceIOCallbackWithContext (const float* const*,
                                                    int,
                                                    float* const* outputChannelData,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (auto* out = outputChannelData[ch])
            FloatVectorOperations::clear (out, numSamples);
}

// AUDIO-CALLBACK THREAD (may be the device thread): RT-safe — atomics only.
void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice*)
{
    // A device is (re)starting cleanly: clear any pending error latch. Status is
    // owned by the message-thread paths (startup / fallback), not set here.
    deviceErrored.store (false, std::memory_order_relaxed);
}

// AUDIO-CALLBACK THREAD: RT-safe. Normal lifecycle event (also fires on clean
// close and on user device switches), so it is deliberately NOT treated as a
// device loss — that would spuriously override a healthy or user-chosen device.
void AudioEngine::audioDeviceStopped () {}

// AUDIO-CALLBACK THREAD: a genuine driver error. Do the minimum RT-safe work:
// latch the error and wake the message thread; the reopen happens there.
void AudioEngine::audioDeviceError (const juce::String&)
{
    deviceErrored.store (true, std::memory_order_relaxed);

    // Accepted RT exception: triggerAsyncUpdate() internally takes a
    // CriticalSection and may allocate (macOS MessageQueue::post), which is
    // normally off-limits on this thread. It is accepted HERE because it only
    // fires when the device is ALREADY failing — typically from a HAL/device
    // notification thread, not the render callback — and it is the canonical JUCE
    // cross-thread signalling pattern (flag + wake the message thread). Documented
    // so a future RT audit does not re-litigate it.
    triggerAsyncUpdate ();
}

// ── Device-death fallback (message thread) ───────────────────────────────────

// MESSAGE-THREAD ONLY.
void AudioEngine::simulateDeviceLoss ()
{
    simulatedLoss.store (true, std::memory_order_relaxed);
    triggerAsyncUpdate ();
}

// MESSAGE-THREAD ONLY. Coalesced wake from a device error or a simulated loss.
void AudioEngine::handleAsyncUpdate ()
{
    const bool simulated = simulatedLoss.exchange (false, std::memory_order_relaxed);
    const bool errored = deviceErrored.exchange (false, std::memory_order_relaxed);
    if (! simulated && ! errored)
        return;

    // Debounce: if a fallback is already running, ignore re-entrant triggers.
    if (fallbackInProgress.exchange (true, std::memory_order_acq_rel))
        return;

    // Bounded-retry state (finding #4) lives in message-thread-only members
    // (fallbackAttemptsInWindow / fallbackWindowActive / fallbackWindowStartMs).
    // handleAsyncUpdate runs only on the message thread (AsyncUpdater) and never
    // concurrently with itself (fallbackInProgress guards re-entrancy), so this
    // cross-call state needs no synchronisation. `fallbackWindowActive` is an
    // explicit "window open" flag (issue #11) — NOT a `fallbackWindowStartMs == 0`
    // sentinel, which could collide with a real zero millisecond counter.
    auto* current = deviceManager.getCurrentAudioDevice ();
    const bool deviceGone = simulated || current == nullptr || ! current->isOpen ();

    if (deviceGone)
    {
        const auto now = juce::Time::getMillisecondCounter ();

        // Open a fresh retry window on the very first attempt (window not active),
        // or once the prior window has fully elapsed (the device stayed healthy long
        // enough that any earlier churn is considered over — this rolling window is
        // the reset).
        if (! fallbackWindowActive || now - fallbackWindowStartMs >= kFallbackWindowMs)
        {
            fallbackWindowActive = true;
            fallbackWindowStartMs = now;
            fallbackAttemptsInWindow = 0;
        }

        if (fallbackAttemptsInWindow >= kMaxFallbackAttempts)
        {
            // Cap reached: a persistently-failing default device. Stop reopening it
            // (no more initialiseWithDefaultDevices) and declare the engine dead
            // until the window elapses and a genuinely new trigger arrives after
            // the cool-down.
            setDeviceStatus (engine::deviceStatusDead);
        }
        else
        {
            ++fallbackAttemptsInWindow;
            performFallback ();
        }
    }
    // else: a transient driver error with the device still open. Deliberately do
    // NOT set deviceStatusOk here (finding #6) — that would silently downgrade an
    // existing FellBackToDefault (or Dead) banner. Only a real successful
    // (re)initialisation clears the status back to Ok; a transient blip preserves
    // whatever health level is already current.

    fallbackInProgress.store (false, std::memory_order_release);
}

// MESSAGE-THREAD ONLY. Reopen the system default output device. Re-initialising
// re-runs audioDeviceAboutToStart on the still-registered player, which
// re-prepares the graph automatically for any new SR/buffer size — we do NOT
// prepare it ourselves.
void AudioEngine::performFallback ()
{
    const auto error = deviceManager.initialiseWithDefaultDevices (kNumInputChannels, kNumOutputChannels);

    // The default device is subject to the same ceiling as any other (issue #56).
    // Enforce BEFORE the health verdict below, and re-read the device afterwards: a
    // refused device has been closed, which the `device != nullptr` test then turns
    // into `deviceStatusDead` — the correct verdict, since we are genuinely silent.
    enforceSampleRateCeiling ();

    auto* device = deviceManager.getCurrentAudioDevice ();

    if (error.isEmpty () && device != nullptr && device->isOpen ())
    {
        // Remember which device the fallback landed on (issue #10). The
        // initialiseWithDefaultDevices call above broadcasts a device change, so
        // changeListenerCallback will fire for THIS same device — it must NOT treat
        // that as a healthy re-selection and clear the banner. It compares the newly
        // active device name against this stored one and only upgrades when a
        // DIFFERENT healthy device is chosen.
        fallbackDeviceName = device->getName ();
        setDeviceStatus (engine::deviceStatusFellBackToDefault);
    }
    else
    {
        setDeviceStatus (engine::deviceStatusDead);
    }

    refreshDeviceDescription ();
    saveDeviceState ();
}

// MESSAGE-THREAD ONLY. Fired when the device setup changes (user selection or an
// internal auto-restart). Persist the new selection and refresh the readout.
void AudioEngine::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // A user picking a device in a settings UI reaches us ONLY here, so the ceiling
    // has to be enforced on this path too (issue #56) — and before the readout is
    // rebuilt and the selection persisted, so we never cache or save an unsupported
    // rate. `AudioDeviceManager` broadcasts asynchronously, so the re-setup this may
    // perform re-enters here later; by then the rate is inside the ceiling (or the
    // device is closed), so the re-entry does NO device work — it only re-checks
    // whether the clamp annotation is still true (issue #68). No loop.
    const bool ceilingActed = enforceSampleRateCeiling ();

    refreshDeviceDescription ();
    saveDeviceState ();

    // A refused device leaves us with no device at all: report the hard error rather
    // than leaving a stale Ok/FellBack banner claiming health we do not have.
    if (ceilingActed && deviceManager.getCurrentAudioDevice () == nullptr)
    {
        setDeviceStatus (engine::deviceStatusDead);
        return;
    }

    // Clear a stale FellBackToDefault banner when the user re-selects a genuinely
    // healthy device (issue #10). Guard against the fallback's OWN change broadcast:
    // upgrade to Ok only if the now-open device is DIFFERENT from the one the
    // fallback selected (a real re-selection), never a downgrade from Dead, and only
    // FROM FellBackToDefault. Re-picking the exact fallback device is the rare case
    // left as-is (the user is on the fallback device anyway).
    if (currentDeviceStatus == engine::deviceStatusFellBackToDefault)
    {
        if (auto* device = deviceManager.getCurrentAudioDevice ();
            device != nullptr && device->isOpen () && device->getName () != fallbackDeviceName)
        {
            setDeviceStatus (engine::deviceStatusOk);
        }
    }
}

// MIDI-INPUT THREAD. Forward the message to the graph's shared collector; the
// MIDI-In node drains it on the audio thread. addMessageToQueue is internally
// synchronized (the collector's own lock), so this cross-thread hand-off is safe.
void AudioEngine::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& message)
{
    graph.midiInputCollector ().addMessageToQueue (message);
}
} // namespace arpbox::app
