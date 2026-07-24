#include "EnginePlaceholder.h"

#ifndef ARPBOX_ENGINE_VERSION
#    define ARPBOX_ENGINE_VERSION "0.0.0"
#endif

namespace arpbox::engine
{
juce::String EnginePlaceholder::getEngineVersion ()
{
    return juce::String (ARPBOX_ENGINE_VERSION);
}
} // namespace arpbox::engine
