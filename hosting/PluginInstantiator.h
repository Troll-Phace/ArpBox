#pragma once

#include "hosting/InstantiationResult.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>

namespace arpbox::hosting
{
/** Async, failure-isolated plugin instantiation (ARCHITECTURE §6.2, §1.4).

    Given a `PluginDescription` (chosen from `PluginManager`'s known list), creates
    the plugin instance and prepares it, delivering a typed `InstantiationResult`
    on the MESSAGE THREAD. Phase 3 stops there — no graph insertion, no
    `HostedPluginNode` wrapper (Phase 4 consumes the prepared-instance seam).

    WHY ASYNC UNIFORMLY: AUv3 MUST be instantiated asynchronously; using
    `createPluginInstanceAsync` for every format keeps one code path and never
    blocks the message thread (code-style.md prohibits blocking instantiation).

    PREPARE-BEFORE-INSERTION: on success the instance has already had
    `prepareToPlay(sampleRate, blockSize)` called before it reaches the callback,
    so Phase 4 can wrap-and-insert a ready node. State-blob restore (Phase 11)
    slots in AFTER prepare and BEFORE insertion.

    FAILURE ISOLATION: a bad description, an unknown format, or a creation failure
    NEVER throws out and NEVER crashes the caller — it lands as a typed
    `InstantiationResult` with a null instance. (A genuine in-process crash during
    construction is out of scope until Phase 20's subprocess work.)

    Borrows the same `AudioPluginFormatManager` the `PluginManager` uses (the DI
    seam); it MUST outlive this object. MESSAGE-THREAD ONLY. */
class PluginInstantiator final
{
public:
    /** Delivers the outcome on the MESSAGE THREAD. Takes the result by value
        (move-only); the caller adopts `instance` on success. */
    using Callback = std::function<void (InstantiationResult)>;

    /** Borrows a caller-owned format manager (the DI seam). Not owned; MUST
        outlive this instantiator. */
    explicit PluginInstantiator (juce::AudioPluginFormatManager& formats);

    /** Asynchronously creates and prepares the described plugin, then invokes
        `callback` on the message thread with a typed result. Non-blocking; returns
        immediately. If no registered format matches the description, `callback` is
        still invoked (with `Status::formatUnknown`) — asynchronously, so callers
        see uniform delivery semantics.

        @param description  What to instantiate (format + uid + name).
        @param sampleRate   Sample rate to create + prepare at (graph's current SR).
        @param blockSize    Max block size to prepare at (graph's current block).
        @param callback     Message-thread completion; never called on the audio
                            thread and never called re-entrantly before return. */
    void instantiate (const juce::PluginDescription& description,
                      double sampleRate,
                      int blockSize,
                      Callback callback);

private:
    // Borrowed, not owned (the DI seam). Must outlive this object.
    juce::AudioPluginFormatManager& formats;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginInstantiator)
};
} // namespace arpbox::hosting
