#include "hosting/PluginInstantiator.h"

#include <utility>

namespace arpbox::hosting
{
using namespace juce;

// MESSAGE-THREAD ONLY.
PluginInstantiator::PluginInstantiator (AudioPluginFormatManager& formatManager)
    : formats (formatManager)
{
}

// MESSAGE-THREAD ONLY. Non-blocking.
void PluginInstantiator::instantiate (const PluginDescription& description,
                                      double sampleRate,
                                      int blockSize,
                                      Callback callback)
{
    // Pre-check that a format matching the description is actually registered. This
    // lets us return a distinct, typed formatUnknown instead of a generic
    // creationFailed, and it is the guard that keeps a stale/foreign description
    // from reaching JUCE at all.
    bool formatIsRegistered = false;
    for (int i = 0; i < formats.getNumFormats (); ++i)
    {
        auto* format = formats.getFormat (i);
        if (format != nullptr && format->getName () == description.pluginFormatName)
        {
            formatIsRegistered = true;
            break;
        }
    }

    if (! formatIsRegistered)
    {
        // Deliver asynchronously so EVERY outcome (success or failure) reaches the
        // caller the same way — on the message thread, after instantiate() returns.
        MessageManager::callAsync (
            [cb = std::move (callback), formatName = description.pluginFormatName]() mutable {
                cb (InstantiationResult::failure (InstantiationResult::Status::formatUnknown,
                                                  "No registered plugin format named '" + formatName + "'"));
            });
        return;
    }

    // Uniform async path (mandatory for AUv3, used for all formats). JUCE invokes
    // the creation callback on the MESSAGE THREAD.
    formats.createPluginInstanceAsync (
        description,
        sampleRate,
        blockSize,
        [cb = std::move (callback), sampleRate, blockSize] (std::unique_ptr<AudioPluginInstance> instance,
                                                            const String& error) mutable {
            // MESSAGE THREAD.
            if (instance == nullptr)
            {
                // Failure isolation: a null instance is a typed error, never a
                // crash or throw. Preserve JUCE's message when it gave us one.
                cb (InstantiationResult::failure (InstantiationResult::Status::creationFailed,
                                                  error.isNotEmpty () ? error
                                                                      : String ("Plugin creation failed")));
                return;
            }

            // Prepare-before-insertion discipline (§6.2): the instance is prepared
            // at the graph's current SR/block BEFORE it is handed back, so Phase 4
            // can wrap-and-insert a ready node (and Phase 11 can restore a state
            // blob after this prepare, before insertion). No graph wiring here.
            instance->prepareToPlay (sampleRate, blockSize);

            cb (InstantiationResult::success (std::move (instance)));
        });
}
} // namespace arpbox::hosting
