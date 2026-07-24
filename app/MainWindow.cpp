#include "MainWindow.h"

#include "DebugPanel.h"

namespace arpbox::app
{
MainWindow::MainWindow (const juce::String& name, AudioEngine& engine)
    : juce::DocumentWindow (name, juce::Colours::black, juce::DocumentWindow::allButtons)
{
    setUsingNativeTitleBar (true);

    // Phase 2 temporary content: the debug panel. Real UI (ARCHITECTURE §10)
    // replaces it in Phase 15+. The window owns the panel; the panel holds only a
    // non-owning reference to the engine, which outlives this window.
    setContentOwned (new DebugPanel (engine), true);

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
