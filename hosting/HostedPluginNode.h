#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>

namespace arpbox::hosting
{
/** The wrapper every hosted plugin sits inside (ARCHITECTURE §6.3, §1.4).

    A plain `juce::AudioProcessor` so the engine's `AudioProcessorGraph` can hold
    it by base pointer without naming the hosting type (one vtable/ABI — the module
    core compiles `AudioProcessor` exactly once). Constructed on the MESSAGE THREAD
    from an ALREADY-PREPARED `AudioPluginInstance` (see `PluginInstantiator`), which
    it adopts and owns; it re-prepares the inner only when the graph's SR/block
    actually change.

    PHASE 4 SCOPE — SYNTH WRAPPER. Presents a clean instrument layout to the graph:
    MIDI in, stereo audio out, NO audio in. FX dry/wet with a latency-compensated
    dry path is Phase 9; the input-trim setter + input-buffer path are wired as a
    seam here but do nothing for a 0-input instrument.

    Responsibilities implemented:
      - Bus negotiation on the inner instance (prefer stereo out; adapt mono→stereo;
        refuse exotic layouts gracefully to a silent-but-valid state — never crash).
      - Soft-bypass crossfade AND swap fade-in/out driven by one smoothed gain, with
        an audio-thread `isFadeOutComplete()` handshake for the swap coordinator.
      - Input/output gain trims (block-rate smoothed, alloc-free).
      - Unconditional output NaN/Inf scrub + denormal guard (mitigates issue #3: a
        NaN reaching the Master limiter). ALWAYS ON.
      - Latency forwarding: mirrors the inner's reported latency to the graph.

    THREADING: setters below are MESSAGE-THREAD ONLY (they publish to atomics the
    audio thread reads). `processBlock` is RT-SAFE — no alloc/lock/log in this
    wrapper's own code. Destruction is on the message thread (the engine's
    removeNode path guarantees it). */
class HostedPluginNode final
    : public juce::AudioProcessor
    , private juce::AudioProcessorListener
{
public:
    /** Adopts an already-prepared inner instance (ownership transferred). MUST be
        constructed on the MESSAGE THREAD. Negotiates the inner's bus layout and
        seeds latency immediately so the node is graph-ready. Does NOT re-prepare
        the inner here — the graph's `prepareToPlay` does that at the authoritative
        SR/block. */
    explicit HostedPluginNode (std::unique_ptr<juce::AudioPluginInstance> preparedInstance);

    ~HostedPluginNode () override;

    // ── Swap / soft-bypass control (MESSAGE-THREAD ONLY) ──────────────────────

    /** Crossfaded soft-bypass. `true` fades the node to silence; `false` fades it
        back in. Click-free (short smoothed ramp). Independent of the swap fade. */
    void setBypassed (bool shouldBypass) noexcept;
    bool isBypassed () const noexcept { return bypassed.load (std::memory_order_relaxed); }

    /** Swap ramp DOWN to silence. The coordinator calls this on the OUTGOING node,
        then polls `isFadeOutComplete()` before asking the engine to remove it.
        Calling this on a NOT-YET-INSERTED node makes it start silent (its
        `prepareToPlay` seeds the gain at 0), so a later `fadeIn()` ramps up
        click-free — that is the swap-IN recipe. */
    void fadeOut () noexcept;

    /** Swap ramp UP to unity. The coordinator calls this on the INCOMING node after
        it is inserted and prepared, to bring it in without a click. */
    void fadeIn () noexcept;

    /** SWAP HANDSHAKE (audio thread → coordinator). Set true on the audio thread
        once the fade/bypass gain has reached 0 AND stopped smoothing, i.e. the node
        is genuinely silent. The coordinator polls this on its UI tick (NOT a
        sequencing timer) and only then removes the node from the graph — guaranteeing
        no audible tail is chopped. Cleared automatically when the node fades back in. */
    bool isFadeOutComplete () const noexcept { return fadeOutComplete.load (std::memory_order_acquire); }

    // ── Gain trims (MESSAGE-THREAD ONLY; block-rate smoothed) ─────────────────

    /** Output gain trim as a linear multiplier (1.0 = unity). The relevant trim for
        the synth wrapper. */
    void setOutputGain (float linearGain) noexcept;

    /** Input gain trim as a linear multiplier. SEAM: applied only when the inner has
        audio inputs (Phase 9 FX). A no-op for a 0-input instrument. */
    void setInputGain (float linearGain) noexcept;

    // ── Accessors (MESSAGE-THREAD ONLY) ───────────────────────────────────────

    /** The wrapped plugin's display name (captured at construction). */
    const juce::String getName () const override { return pluginName; }

    /** Non-owning passthrough to the inner instance for the editor windows (Phase
        10) and opaque state capture/restore (Phase 11). The wrapper retains
        ownership; never delete through this pointer. */
    juce::AudioPluginInstance* getWrappedInstance () noexcept { return inner.get (); }
    const juce::AudioPluginInstance* getWrappedInstance () const noexcept { return inner.get (); }

    // ── AudioProcessor ────────────────────────────────────────────────────────

    // RT-SAFE: prepares the inner (only on an SR/block change) and (re)arms the
    // smoothers/scratch. MESSAGE-THREAD ONLY (the graph calls this off-audio).
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;

    // MESSAGE-THREAD ONLY: forwards release to the inner instance.
    void releaseResources () override;

    // RT-SAFE: input trim (FX seam) → inner render → output trim → fade/bypass
    // ramp → NaN/Inf scrub. No alloc/lock in this wrapper's own code path.
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    double getTailLengthSeconds () const override;
    bool acceptsMidi () const override { return true; }
    bool producesMidi () const override;
    bool isMidiEffect () const override { return false; }

    // Editors are separate ARPBOX-chromed DocumentWindows (Phase 10) — the wrapper
    // never hosts one inline. §6.4.
    juce::AudioProcessorEditor* createEditor () override { return nullptr; }
    bool hasEditor () const override { return false; }

    // Programs + opaque state forward transparently to the inner instance.
    int getNumPrograms () override;
    int getCurrentProgram () override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // The wrapper presents a fixed instrument layout: no input, stereo output.
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

private:
    // ── AudioProcessorListener ────────────────────────────────────────────────
    // Mirrors the inner's latency onto the graph WHENEVER it changes (§6.3, B5).
    // W3: a HOSTILE inner can call updateHostDisplay() from its OWN processBlock,
    // firing this listener on the AUDIO THREAD. There, setLatencySamples →
    // updateHostDisplay → the graph listener → triggerAsyncUpdate ultimately takes a
    // CriticalSection (and may reallocate the message queue) — a hard-rule
    // violation. So on the audio thread we do the ONE lock-free thing allowed: set an
    // atomic dirty flag. The owner drains it on the message thread via
    // pollPendingLatencyChange(). No timer, no async message, no second audio-thread
    // lock (this project sanctions exactly one — the MIDI collector — and this is not
    // it).
    void audioProcessorChanged (juce::AudioProcessor* processor,
                                const juce::AudioProcessorListener::ChangeDetails& details) override;
    void audioProcessorParameterChanged (juce::AudioProcessor* processor,
                                         int parameterIndex,
                                         float newValue) override;

public:
    /** MESSAGE-THREAD ONLY: services a deferred latency change that was flagged from
        the audio thread (a hostile inner revising its latency inside its own
        processBlock). The owner — the SynthSlot coordinator — calls this every UI
        frame. If the flag was set, propagates the inner's current latency to the
        graph (which then recomputes and raises the UI event). Cheap no-op when
        nothing is pending. */
    void pollPendingLatencyChange () noexcept;

private:

    // Negotiates a workable inner output layout; sets fastStereoPath / renderInner /
    // channel counts. MESSAGE-THREAD ONLY. Never asserts-kills.
    void negotiateBusLayout ();

    // RT-SAFE: renders the inner into `buffer` (stereo), via the fast path when the
    // inner is 0-in/2-out, else through the preallocated scratch buffer.
    void renderInner (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages, int numSamples) noexcept;

    // The instrument bus layout this wrapper presents to the graph.
    static BusesProperties makeWrapperBuses ();

    // Adopted, prepared inner instance. Owned; freed on the message thread.
    std::unique_ptr<juce::AudioPluginInstance> inner;
    juce::String pluginName;

    // Negotiated inner layout (message thread writes; audio thread reads via flags).
    int innerInChannels = 0;
    int innerOutChannels = 0;
    bool fastStereoPath = false;          // inner is 0-in / 2-out → render straight into buffer.
    std::atomic<bool> renderActive { true }; // false ⇒ negotiation failed; emit silence, never crash.

    // Scratch for the non-fast path (preallocated in prepareToPlay; never grown on
    // the audio thread).
    juce::AudioBuffer<float> scratch;
    int scratchChannels = 0;

    // Smoothers live on the audio thread; targets are published via atomics.
    juce::LinearSmoothedValue<float> fadeGain;   // soft-bypass + swap fade.
    juce::LinearSmoothedValue<float> inputTrim;  // FX seam.
    juce::LinearSmoothedValue<float> outputTrim;

    std::atomic<bool> bypassed { false };
    std::atomic<bool> fadedOut { false };
    std::atomic<float> inputGainTarget { 1.0f };
    std::atomic<float> outputGainTarget { 1.0f };

    // Swap handshake (audio thread → coordinator).
    std::atomic<bool> fadeOutComplete { false };

    // Deferred latency change: set (release) on the audio thread by a hostile inner's
    // latency revision; drained (acquire) on the message thread by
    // pollPendingLatencyChange(). Lock-free — the audio-thread side is one atomic store.
    std::atomic<bool> latencyDirty { false };

    // Re-prepare guard (§6.2: adopt the prepared inner; re-prepare only on change).
    double lastPreparedSampleRate = 0.0;
    int lastPreparedBlockSize = 0;

    static constexpr double kFadeSeconds = 0.010; // ~10 ms click-free ramp.
    static constexpr double kTrimSeconds = 0.020;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HostedPluginNode)
};
} // namespace arpbox::hosting
