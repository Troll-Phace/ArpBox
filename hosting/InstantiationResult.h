#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdint>
#include <memory>

namespace arpbox::hosting
{
/** Typed outcome of a plugin-instantiation attempt (ARCHITECTURE §1.4, §6.2).

    FROZEN CONTRACT. This is the seam between the instantiation service
    (`PluginInstantiator`, Phase 3), the `HostedPluginNode` wrapper + graph
    insertion (Phase 4), and the hosting-lab tests. Do NOT reshape it without
    coordinating those consumers — additive `Status` values are fine; renaming or
    reordering fields is not.

    "Plugins are hostile until proven otherwise": a failed instantiation NEVER
    throws out and NEVER crashes the caller. Every failure mode lands here as a
    `Status` + human-readable `message`, and `instance` is null. On success,
    `instance` is a fully constructed AND prepared (`prepareToPlay`) plugin
    ready for graph insertion; the caller takes ownership by moving it out.

    Move-only (owns a `unique_ptr`). Deliver on the MESSAGE THREAD. */
struct InstantiationResult
{
    enum class Status : std::uint8_t
    {
        ok,             ///< Instance created and prepared; `instance` is non-null.
        formatUnknown,  ///< No registered format matches the description's format.
        creationFailed, ///< The format could not create the instance (JUCE error).
        timedOut,       ///< Reserved: async creation exceeded a deadline (Phase 20
                        ///< subprocess supervision; Phase 3 never produces this).
        cancelled       ///< Reserved: the request was cancelled before completion.
    };

    Status status { Status::creationFailed };
    juce::String message;                                 ///< Empty on success; error text otherwise.
    std::unique_ptr<juce::AudioPluginInstance> instance;  ///< Non-null iff status == ok.

    /** True when a prepared instance is present and ready for the caller to adopt. */
    bool ok () const noexcept { return status == Status::ok && instance != nullptr; }

    /** Builds a success result that transfers ownership of a prepared instance. */
    static InstantiationResult success (std::unique_ptr<juce::AudioPluginInstance> preparedInstance)
    {
        return { Status::ok, {}, std::move (preparedInstance) };
    }

    /** Builds a typed failure result (no instance). */
    static InstantiationResult failure (Status failureStatus, juce::String errorMessage)
    {
        // A failure must never carry an instance — the seam guarantees callers can
        // branch on ok()/status alone without inspecting the pointer.
        return { failureStatus, std::move (errorMessage), nullptr };
    }
};
} // namespace arpbox::hosting
