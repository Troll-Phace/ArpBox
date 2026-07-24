#include "MainWindow.h"

namespace arpbox::app
{
MainWindow::MainWindow (const juce::String& name)
    : juce::DocumentWindow (name, juce::Colours::black, juce::DocumentWindow::allButtons)
{
    setUsingNativeTitleBar (true);

    // Empty placeholder content. Real UI (ARCHITECTURE §10) arrives in Phase 15+.
    auto content = std::make_unique<juce::Component> ();
    content->setSize (1280, 800); // §10.2 minimum window size.
    setContentOwned (content.release (), true);

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
