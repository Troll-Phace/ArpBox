#include "MainWindow.h"

#include "engine/graph/EnginePlaceholder.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace arpbox::app
{
/** ARPBOX application entry point.

    Phase 1 scaffold: constructs the empty main window on launch and tears it
    down cleanly on quit. Audio device / graph wiring (ARCHITECTURE §3.3, §3.4)
    is added in Phase 2. */
class ArpboxApplication final : public juce::JUCEApplication
{
public:
    ArpboxApplication () = default;

    const juce::String getApplicationName () override { return "ARPBOX"; }
    const juce::String getApplicationVersion () override { return ARPBOX_ENGINE_VERSION; }
    bool moreThanOneInstanceAllowed () override { return false; }

    void initialise (const juce::String&) override
    {
        // Prove the app links the (UI-free) engine library.
        DBG ("ARPBOX engine version: " << engine::EnginePlaceholder::getEngineVersion ());
        mainWindow = std::make_unique<MainWindow> (getApplicationName ());
    }

    void shutdown () override { mainWindow = nullptr; }

    void systemRequestedQuit () override { quit (); }

private:
    std::unique_ptr<MainWindow> mainWindow;
};
} // namespace arpbox::app

START_JUCE_APPLICATION (arpbox::app::ArpboxApplication)
