// ─────────────────────────────────────────────────────────────────────────────
// hosting-lab — PRODUCTION FORMAT REGISTRATION (docs/INSTRUCTIONS.md Phase 3.3;
// ARCHITECTURE §6.1).
//
// Regression guard for the Phase 3 bug where `addProductionFormats` registered
// ZERO formats because JUCE_PLUGINHOST_VST3 / JUCE_PLUGINHOST_AU defaulted to 0.
// The production scan path was completely non-functional ("0 types, 0 failed"
// instantly on a real machine), yet the rest of the hosting-lab never caught it
// because those tests inject a FakePluginFormat directly and bypass
// `addProductionFormats` entirely.
//
// This test calls the REAL production registration helper and asserts the
// formats are actually registered. It is CI-safe: registration only — it does
// NOT scan directories, touch ~/Library, or instantiate any real plugin. Had it
// existed, it would have caught the regression at commit time.
// ─────────────────────────────────────────────────────────────────────────────

#include "fakes/HostingLabSupport.h"

#include "hosting/PluginManager.h"

#include <catch2/catch_test_macros.hpp>

using namespace arpbox::hosting;
using namespace arpbox::testing;

TEST_CASE ("hosting/formats: addProductionFormats registers VST3 + AudioUnit", "[hosting-lab][unit]")
{
    MessageScope juceInit;

    juce::AudioPluginFormatManager formats;
    addProductionFormats (formats);

    // Collect the registered format names (owned by the manager).
    juce::StringArray names;
    for (int i = 0; i < formats.getNumFormats (); ++i)
        names.add (formats.getFormat (i)->getName ());

    SECTION ("both production formats register by name")
    {
        // The exact assertion that would have caught the regression: with the
        // host flags off, neither name is present (the list is empty).
        REQUIRE (names.contains ("VST3"));
        REQUIRE (names.contains ("AudioUnit"));
    }

    SECTION ("exactly the two macOS production formats register")
    {
        // JUCE_PLUGINHOST_VST3 + JUCE_PLUGINHOST_AU enable exactly these two on
        // macOS. Guarded on JUCE_MAC since AudioUnit is macOS-only.
#if JUCE_MAC
        REQUIRE (formats.getNumFormats () == 2);
#endif
    }
}
