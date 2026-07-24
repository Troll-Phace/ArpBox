#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace arpbox::app
{
class AudioEngine;

/** The application's single top-level window.

    Phase 2: hosts the temporary `DebugPanel` (test tone, meters, device-status
    banner) as its content component. The three-column layout (ARCHITECTURE §10.2)
    replaces the debug panel in the UI phases (15+). Closing the window quits the
    app cleanly.

    Holds a non-owning reference to the `AudioEngine` (owned by the application,
    constructed before and destroyed after this window).

    MESSAGE-THREAD ONLY. */
class MainWindow final : public juce::DocumentWindow
{
public:
    /** Creates the main window with the debug panel content.
        @param name   the window title.
        @param engine the app audio backbone (non-owning; must outlive this). */
    MainWindow (const juce::String& name, AudioEngine& engine);

    /** Requests app shutdown when the user closes the window. */
    void closeButtonPressed () override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};
} // namespace arpbox::app
