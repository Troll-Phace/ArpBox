#include "AudioEngine.h"
#include "MainWindow.h"

#include "engine/graph/EnginePlaceholder.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace arpbox::app
{
/** ARPBOX application entry point.

    Phase 2: owns the `AudioEngine` (device manager + player + graph, ARCHITECTURE
    §3.2, §3.3) and the main window that hosts the debug panel. The engine is
    constructed BEFORE the window (so it outlives every UI reference into it) and
    destroyed AFTER the window on shutdown. */
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

        // Engine FIRST — it owns the graph/channels the window's debug panel
        // reads from, so it must outlive the window.
        audioEngine = std::make_unique<AudioEngine> ();
        mainWindow = std::make_unique<MainWindow> (getApplicationName (), *audioEngine);
    }

    void shutdown () override
    {
        // Window FIRST (stops UI reads into the engine), THEN the engine.
        mainWindow = nullptr;
        audioEngine = nullptr;
    }

    void systemRequestedQuit () override { quit (); }

private:
    std::unique_ptr<AudioEngine> audioEngine;
    std::unique_ptr<MainWindow> mainWindow;
};
} // namespace arpbox::app

START_JUCE_APPLICATION (arpbox::app::ArpboxApplication)
