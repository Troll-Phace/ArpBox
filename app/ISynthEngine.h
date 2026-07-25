#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace arpbox::app
{
/** Minimal, GUI-FREE abstraction of the engine surface the synth-slot coordinator
    (`SynthSlot`) drives (issue #22 testability seam).

    WHY THIS EXISTS: `SynthSlot` needs to call a handful of message-thread methods on
    the engine, but the concrete `AudioEngine` transitively pulls in
    `juce_audio_utils` / `juce_audio_devices` (AudioDeviceManager / AudioProcessorPlayer).
    Depending on that whole device stack made `SynthSlot` impossible to compile in the
    GUI-free test target (`arpbox_tests`, which links only `arpbox_hosting`). This
    interface declares ONLY the five calls `SynthSlot` makes, and includes ONLY
    `juce_audio_processors` (needed for `std::unique_ptr<juce::AudioProcessor>`) — no
    utils, no devices, no GUI. `AudioEngine` implements it in production; a lightweight
    fake implements it in tests.

    THREADING: every method here is MESSAGE-THREAD ONLY, matching `AudioEngine`. */
class ISynthEngine
{
public:
    virtual ~ISynthEngine () = default;

    // MESSAGE-THREAD ONLY: hand a PREPARED synth instance to the engine's single
    // instrument slot (ownership transferred). `nullptr` is equivalent to removeSynth().
    virtual void setSynth (std::unique_ptr<juce::AudioProcessor> synth) = 0;

    // MESSAGE-THREAD ONLY: remove the current synth from the instrument slot.
    virtual void removeSynth () = 0;

    // MESSAGE-THREAD ONLY: flush held MIDI-in notes (stuck-note guard around swaps).
    virtual void allNotesOff () = 0;

    // MESSAGE-THREAD ONLY: current graph sample rate (Hz); 0 before the device opens.
    virtual double getCurrentSampleRate () const noexcept = 0;

    // MESSAGE-THREAD ONLY: current graph block size (samples); 0 before the device opens.
    virtual int getCurrentBlockSize () const noexcept = 0;
};
} // namespace arpbox::app
