#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace arpbox::app
{
/** The application's single top-level window.

    Phase 1 scaffold: an empty, resizable, native-titlebar DocumentWindow titled
    "ARPBOX". The three-column layout (ARCHITECTURE §10.2) is built out in the UI
    phases (15+). Closing the window quits the app cleanly.

    MESSAGE-THREAD ONLY. */
class MainWindow final : public juce::DocumentWindow
{
public:
    /** Creates the main window. @param name the window title. */
    explicit MainWindow (const juce::String& name);

    /** Requests app shutdown when the user closes the window. */
    void closeButtonPressed () override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};
} // namespace arpbox::app
