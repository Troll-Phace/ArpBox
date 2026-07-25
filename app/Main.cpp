#include "AudioEngine.h"
#include "MainWindow.h"

#include "engine/graph/EnginePlaceholder.h"
#include "hosting/PluginManager.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace arpbox::app
{
/** ARPBOX application entry point.

    Phase 2: owns the `AudioEngine` (device manager + player + graph, ARCHITECTURE
    §3.2, §3.3) and the main window that hosts the debug panel. The engine is
    constructed BEFORE the window (so it outlives every UI reference into it) and
    destroyed AFTER the window on shutdown.

    Phase 3 (§6.1): also owns the plugin-hosting layer — a production
    `AudioPluginFormatManager` (VST3 + AU) and the `hosting::PluginManager` that
    borrows it. ORDER IS LOAD-BEARING: the format manager is declared BEFORE the
    PluginManager (which holds a reference to it), and both are declared AFTER the
    engine and BEFORE the window. On shutdown the window is destroyed first — its
    debug panel stops + joins the background scan thread before any hosting object
    it references is torn down — then the PluginManager, then (implicitly) the
    format manager, then the engine. */
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

        // Hosting layer (§6.1). Register the production formats (VST3 + AU) on the
        // app-owned format manager — addProductionFormats wraps JUCE 8's
        // addDefaultFormatsToManager (the deleted addDefaultFormats replacement) —
        // then let the PluginManager borrow it. Default settings dir → the real
        // ~/Library/Application Support/ARPBOX/, so plugin-list.xml persists.
        hosting::addProductionFormats (pluginFormats);
        pluginManager = std::make_unique<hosting::PluginManager> (pluginFormats);

        // Restore a previously-scanned list (+ dead-man's-pedal blacklist) so it is
        // available at launch WITHOUT rescanning. A fresh install simply starts empty.
        pluginManager->restore ();

        mainWindow = std::make_unique<MainWindow> (getApplicationName (),
                                                   *audioEngine,
                                                   *pluginManager,
                                                   scanWorkerForceKilled);
    }

    void shutdown () override
    {
        // Window FIRST: destroying the debug panel stops + joins the background
        // scan thread, so nothing touches the PluginManager after this line. If the
        // worker had to be force-killed, the panel sets scanWorkerForceKilled here.
        mainWindow = nullptr;

        // Belt-and-suspenders persist (the authoritative save is on scan
        // completion). SKIP it if the scan worker was force-killed (issue #14): a
        // force-kill can leave the KnownPluginList locked/inconsistent mid-mutation,
        // and save() reads it via createXml() — reading would deadlock or serialize
        // corrupt state. A cleanly-finished scan already saved on completion, so no
        // data is lost by skipping. Normal (never-scanned / clean-scan) shutdowns
        // still save.
        //
        // save() logs its own failure durably (issue #17). There is no UI left to
        // surface it to here — the window is already gone — so we only add the shutdown
        // consequence, and NEVER throw or block: a failed persist must not wedge
        // shutdown. writeTo() is atomic, so a previously-saved list survives intact.
        if (pluginManager != nullptr && ! scanWorkerForceKilled)
        {
            const auto saved = pluginManager->save ();

            if (! saved.wasOk ())
            {
                juce::Logger::writeToLog ("ARPBOX: shutdown was the last chance to persist the "
                                          "plugin list — this session's scan results are lost.");
            }
        }

        // PluginManager BEFORE its borrowed format manager (a value member destroyed
        // after this scope), then the engine LAST.
        pluginManager = nullptr;
        audioEngine = nullptr;
    }

    void systemRequestedQuit () override { quit (); }

private:
    // App-owned teardown flag written by the debug panel's destructor (issue #14):
    // true iff the background scan worker had to be force-killed. Declared FIRST so
    // it outlives mainWindow (whose destruction writes it) and is still valid when
    // shutdown() reads it. A plain bool — only ever touched on the message thread.
    bool scanWorkerForceKilled = false;

    // Declaration order == reverse destruction order. Destroyed bottom-to-top:
    // mainWindow (joins the scan thread) → pluginManager → pluginFormats → engine.
    std::unique_ptr<AudioEngine> audioEngine;         ///< Destroyed LAST.
    juce::AudioPluginFormatManager pluginFormats;      ///< Borrowed by pluginManager; outlives it.
    std::unique_ptr<hosting::PluginManager> pluginManager;
    std::unique_ptr<MainWindow> mainWindow;            ///< Destroyed FIRST.
};
} // namespace arpbox::app

START_JUCE_APPLICATION (arpbox::app::ArpboxApplication)
