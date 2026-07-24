// ─────────────────────────────────────────────────────────────────────────────
// HostingLabSupport — shared helpers for the hosting-lab test files.
//
// PluginInstantiator delivers its result asynchronously via MessageManager::
// callAsync (even the formatUnknown fast-fail), and the JUCE format's async
// creation posts a message to itself. Tests must therefore stand up a
// MessageManager and pump it. We pump in bounded slices up to a hard deadline —
// no wall-clock sleep is used as a synchronization primitive (the deadline is a
// timeout guarding against a hang, not the mechanism that establishes ordering).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>

namespace arpbox::testing
{
/** A unique temporary directory that is created on construction and recursively
    deleted on destruction. Handed to PluginManager as its settingsDirectory so
    save()/restore() round-trip without writing to
    ~/Library/Application Support/ARPBOX. */
struct UniqueTempDir
{
    UniqueTempDir ()
        : dir (juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("arpbox-hosting-lab-" + juce::Uuid ().toDashedString ()))
    {
        dir.createDirectory ();
    }

    ~UniqueTempDir () { dir.deleteRecursively (); }

    juce::File dir;
};

/** RAII MessageManager for a test case: brings up the JUCE message thread so async
    instantiation callbacks can be dispatched, and tears it down at scope exit. */
using MessageScope = juce::ScopedJuceInitialiser_GUI;

/** Pumps the message loop in short slices until `done` is set or `timeoutMs`
    elapses. Returns true if `done` was observed set (false ⇒ timed out). The
    slice length only bounds latency between checks; correctness does not depend
    on it. Must be called on the message thread (the test thread). */
inline bool pumpUntil (const std::atomic<bool>& done, int timeoutMs = 5000)
{
    const auto deadline = juce::Time::getMillisecondCounter () + static_cast<juce::uint32> (timeoutMs);

    while (! done.load (std::memory_order_acquire))
    {
        if (juce::Time::getMillisecondCounter () >= deadline)
            return false;

        juce::MessageManager::getInstance ()->runDispatchLoopUntil (20);
    }

    return true;
}
} // namespace arpbox::testing
