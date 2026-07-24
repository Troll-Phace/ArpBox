#include "hosting/InProcessScanner.h"

namespace arpbox::hosting
{
using namespace juce;

// MESSAGE/WORKER THREAD ONLY.
bool InProcessScanner::findPluginTypesFor (AudioPluginFormat& format,
                                           OwnedArray<PluginDescription>& result,
                                           const String& fileOrIdentifier)
{
    // In-process discovery: the format loads the binary and enumerates the types
    // it exposes. This can execute hostile third-party static/init code, so it
    // MUST stay off the audio thread (it is — scans run on a worker/message
    // thread). We cannot survive a genuine crash here; Phase 20 moves exactly this
    // call into the scanner-helper subprocess and returns false on child death.
    format.findAllTypesForFile (result, fileOrIdentifier);

    // true == "the binary loaded" (whether or not it exposed any types). A false
    // return is the crash marker, reserved for the subprocess scanner.
    return true;
}

// MESSAGE/WORKER THREAD ONLY.
void InProcessScanner::scanFinished ()
{
    // Nothing to release in-process. The Phase 20 subprocess scanner overrides this
    // to shut down the ChildProcessCoordinator / helper process.
}
} // namespace arpbox::hosting
