// TEMPORARY DEBUG UI — replaced by the real UI in Phase 15+.
#include "DebugPanel.h"

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
DebugPanel::DebugPanel (AudioEngine& engine)
    : audioEngine (engine),
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

    // ── Readout labels ───────────────────────────────────────────────────────
    for (auto* label : { &deviceLabel, &statusBanner, &meterLabel, &blockLabel, &eventLabel })
        addAndMakeVisible (*label);

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
