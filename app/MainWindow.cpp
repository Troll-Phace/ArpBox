#include "MainWindow.h"

#include "DebugPanel.h"

namespace arpbox::app
{
MainWindow::MainWindow (const juce::String& name,
                        AudioEngine& engine,
                        hosting::PluginManager& plugins,
                        bool& scanForceKilledSink)
    : juce::DocumentWindow (name, juce::Colours::black, juce::DocumentWindow::allButtons)
{
    setUsingNativeTitleBar (true);

    // Phase 2 temporary content: the debug panel. Real UI (ARCHITECTURE §10)
    // replaces it in Phase 15+. The window owns the panel; the panel holds only
    // non-owning references to the engine + plugin manager, which both outlive
    // this window (owned by the application, destroyed after it). The
    // scanForceKilledSink is app-owned and likewise outlives the panel.
    setContentOwned (new DebugPanel (engine, plugins, scanForceKilledSink), true);

    setResizable (true, true);
    setResizeLimits (1280, 800, 10000, 10000);
    centreWithSize (getWidth (), getHeight ());
    setVisible (true);
}

void MainWindow::closeButtonPressed ()
{
    juce::JUCEApplication::getInstance ()->systemRequestedQuit ();
}
} // namespace arpbox::app
