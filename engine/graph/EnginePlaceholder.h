#pragma once

#include "../EngineGuiGuard.h"

#include <juce_core/juce_core.h>

namespace arpbox::engine
{
/** Placeholder engine entity that exists only to give the arpbox_engine static
    library a real, compilable translation unit for the Phase 1 scaffold.

    It carries no audio logic. Real graph/transport/master code (ARCHITECTURE
    §3.3, §3.4) lands in Phase 2 and replaces this. The one behaviour it has —
    reporting the engine's semantic version — lets the app prove it links the
    engine library at startup.

    MESSAGE-THREAD ONLY: nothing here runs on the audio thread yet. */
class EnginePlaceholder
{
public:
    EnginePlaceholder () = default;

    /** Returns the engine library's semantic version string ("MAJOR.MINOR.PATCH").
        Pulled from the CMake project version via a compile definition. */
    static juce::String getEngineVersion ();
};
} // namespace arpbox::engine
