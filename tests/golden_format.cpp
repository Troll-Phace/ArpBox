// ─────────────────────────────────────────────────────────────────────────────
// determinism suite — golden-MIDI FILE INFRASTRUCTURE tests (Phase 6.4).
//
// These cases test the MECHANISM, not any particular performance: serialization,
// the strict parser, comparison, and the regeneration interlock. They exist BEFORE
// the first engine golden is baked, and they are what makes a later green run
// meaningful — a golden suite whose loader silently accepts a truncated file, or
// whose regeneration path can report success, is a gate that proves nothing.
//
// Everything here is driven from hand-built `MidiRenderResult`s and hand-written
// fixture text: no engine, no transport, no rendering. Filesystem work goes to a
// `UniqueTempDir`, so no case reads or writes tests/golden/ itself.
//
// THE ANTI-VACUITY RULE THIS FILE ENFORCES: a parser that accepts everything
// validates nothing, so EVERY rejection the loader claims to make is asserted
// individually below, with its own expected message and line number.
// ─────────────────────────────────────────────────────────────────────────────

#include "fakes/HostingLabSupport.h"
#include "support/GoldenMidiFile.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <optional>
#include <string>

using namespace arpbox::testing;

namespace
{
/** A small, varied, hand-built render: 1-, 2- and 3-byte messages, ascending
    sample positions. Nothing here depends on the engine. */
MidiRenderResult makeRender ()
{
    MidiRenderResult render;
    render.sampleRate = 48000.0;
    render.blockSize = 128;
    render.numBlocks = 8;
    render.numSamples = 1024;

    render.events.push_back (TimedMidiEvent { 0, juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)) });
    render.events.push_back (TimedMidiEvent { 240, juce::MidiMessage::noteOff (1, 60) });
    render.events.push_back (TimedMidiEvent { 240, juce::MidiMessage::controllerEvent (1, 123, 0) });
    render.events.push_back (TimedMidiEvent { 480, juce::MidiMessage::midiClock () });                  // 1 byte (F8)
    render.events.push_back (TimedMidiEvent { 600, juce::MidiMessage::channelPressureChange (1, 64) }); // 2 bytes
    render.events.push_back (TimedMidiEvent { 960, juce::MidiMessage::noteOn (2, 67, static_cast<juce::uint8> (90)) });
    return render;
}

GoldenHeader makeHeader (const MidiRenderResult& render, const juce::String& name = "fixture")
{
    return headerFor (render, name, 125.0, 0.25);
}

/** A well-formed golden fixture, written out by hand rather than by the serializer,
    so the malformed variants below are edits to a KNOWN-GOOD literal and the parser
    is not being tested only against its own writer. */
const char* const validFixture = "# arpbox-golden 1\n"
                                 "# name: fixture\n"
                                 "# sampleRate: 48000\n"
                                 "# bpm: 125\n"
                                 "# gridPpq: 0.25\n"
                                 "# spanSamples: 1024\n"
                                 "# events: 2\n"
                                 "# rngVersion: 0\n"
                                 "# bakedAtBlockSize: 128\n"
                                 "0 90 3C 64\n"
                                 "240 80 3C 00\n";

/** `validFixture` with `find` replaced by `replacement` (first occurrence). */
juce::String fixtureWith (const juce::String& find, const juce::String& replacement)
{
    return juce::String (validFixture).replaceFirstOccurrenceOf (find, replacement);
}

/** RAII around the regeneration environment variable, so a failing REQUIRE cannot
    leak a set variable into the rest of the run.

    ── IT RESTORES THE PRIOR VALUE. IT DOES NOT UNSET. ─────────────────────────
    THIS IS A CONTRACT, NOT AN IMPLEMENTATION DETAIL — do not "simplify" the
    destructor back to an unconditional `::unsetenv`.

    An unconditional unset makes this guard DESTROY the very state the whole
    regeneration workflow runs on. Catch2 runs every case in ONE process, and
    these cases are declared before the baked-golden cases in
    tests/determinism_goldens.cpp, so the first `~ScopedRegenerateEnv` would
    clear the variable for every golden that follows:

        ARPBOX_REGENERATE_GOLDENS=1 ./build/tests/arpbox_tests "[determinism]"

    …would rewrite NOTHING while reporting an ordinary comparison result. That
    silence is the danger. A developer regenerating a golden sees no diff and
    concludes the engine did not change — the same shape as issue #30, where a
    clang-format hook was a no-op for five phases because its error was
    swallowed. A hard failure would have been strictly better than either.

    (`ctest` is unaffected either way: `catch_discover_tests` gives every case
    its own forked process, which is why the README's documented command always
    worked and why this stayed invisible for so long. The direct-binary form
    above is the one that broke, and it is the one people actually type.)

    `::getenv`'s pointer is invalidated by the `::setenv` below, so the prior
    value is COPIED before anything is written. */
class ScopedRegenerateEnv
{
public:
    explicit ScopedRegenerateEnv (const char* value)
    {
        if (const char* const previous = ::getenv (goldenRegenerateEnvVar))
            saved.emplace (previous); // copy NOW — setenv invalidates the pointer

        if (value != nullptr)
            ::setenv (goldenRegenerateEnvVar, value, 1);
        else
            ::unsetenv (goldenRegenerateEnvVar);
    }

    ~ScopedRegenerateEnv ()
    {
        if (saved.has_value ())
            ::setenv (goldenRegenerateEnvVar, saved->c_str (), 1); // verbatim, overwrite
        else
            ::unsetenv (goldenRegenerateEnvVar); // it genuinely was not set
    }

    ScopedRegenerateEnv (const ScopedRegenerateEnv&) = delete;
    ScopedRegenerateEnv& operator= (const ScopedRegenerateEnv&) = delete;

private:
    /** The value the process had on entry: `nullopt` ⇒ the variable was ABSENT,
        which is a different state from "set to the empty string" and must be
        restored as such (`isTruthyEnvValue` reads "" as OFF, but `getenv`
        returning "" vs nullptr is observable). */
    std::optional<std::string> saved;
};
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Round trip
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/golden: serialize then parse round-trips the event stream", "[unit][determinism]")
{
    const auto render = makeRender ();
    const auto text = serializeGolden (render, makeHeader (render));

    const auto parsed = parseGolden (text);
    INFO (text);
    INFO (parsed.error);
    REQUIRE (parsed.ok);

    // The canonical in-memory form (MidiRenderHarness::toByteStream) must survive the
    // disk trip byte-for-byte — that is what "byte-identical MIDI, forever" means once
    // the reference lives in a file.
    MidiRenderResult reparsed;
    reparsed.events = parsed.events;
    REQUIRE (reparsed.toByteStream () == render.toByteStream ());

    // Derived header fields come from the RENDER, never from the caller's struct.
    CHECK (parsed.header.name == "fixture");
    CHECK (parsed.header.sampleRate == 48000.0);
    CHECK (parsed.header.bpm == 125.0);
    CHECK (parsed.header.gridPpq == 0.25);
    CHECK (parsed.header.spanSamples == 1024);
    CHECK (parsed.header.declaredEvents == static_cast<int> (render.events.size ()));
    CHECK (parsed.header.bakedAtBlockSize == 128);
    CHECK (parsed.header.rngVersion == 0);
}

TEST_CASE ("determinism/golden: the serializer's derived fields ignore a wrong hand-filled header",
           "[unit][determinism]")
{
    const auto render = makeRender ();

    GoldenHeader lying;
    lying.name = "fixture";
    lying.bpm = 125.0;
    lying.gridPpq = 0.25;
    lying.sampleRate = 1.0;      // wrong on purpose
    lying.spanSamples = 7;       // wrong on purpose
    lying.declaredEvents = 999;  // wrong on purpose — would make the file unloadable
    lying.bakedAtBlockSize = 13; // wrong on purpose

    const auto parsed = parseGolden (serializeGolden (render, lying));
    INFO (parsed.error);
    REQUIRE (parsed.ok);
    CHECK (parsed.header.declaredEvents == static_cast<int> (render.events.size ()));
    CHECK (parsed.header.sampleRate == render.sampleRate);
    CHECK (parsed.header.spanSamples == render.numSamples);
    CHECK (parsed.header.bakedAtBlockSize == render.blockSize);
}

// ─────────────────────────────────────────────────────────────────────────────
// Parser rejections — ONE assertion cluster per malformed input
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/golden: the loader accepts a well-formed fixture", "[unit][determinism]")
{
    const auto parsed = parseGolden (validFixture);
    INFO (parsed.error);
    REQUIRE (parsed.ok);
    REQUIRE (parsed.events.size () == 2);
    REQUIRE (parsed.events[0].absoluteSample == 0);
    REQUIRE (parsed.events[1].absoluteSample == 240);
    REQUIRE (parsed.errorLine == 0);
}

TEST_CASE ("determinism/golden: parser rejects every malformed input, individually", "[unit][determinism]")
{
    SECTION ("empty file")
    {
        const auto parsed = parseGolden ("");
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("empty"));
        REQUIRE (parsed.errorLine == 0);
    }

    SECTION ("no trailing newline")
    {
        const auto parsed = parseGolden (juce::String (validFixture).dropLastCharacters (1));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("does not end with a newline"));
        REQUIRE (parsed.errorLine == 11);
    }

    SECTION ("blank line")
    {
        const auto parsed = parseGolden (fixtureWith ("0 90 3C 64\n", "\n0 90 3C 64\n"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("blank line"));
        REQUIRE (parsed.errorLine == 10);
    }

    SECTION ("CRLF line ending")
    {
        const auto parsed = parseGolden (fixtureWith ("0 90 3C 64\n", "0 90 3C 64\r\n"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("carriage return"));
        REQUIRE (parsed.errorLine == 10);
    }

    SECTION ("tab separator")
    {
        const auto parsed = parseGolden (fixtureWith ("0 90 3C 64", "0\t90 3C 64"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("tab"));
        REQUIRE (parsed.errorLine == 10);
    }

    SECTION ("trailing whitespace")
    {
        const auto parsed = parseGolden (fixtureWith ("0 90 3C 64", "0 90 3C 64 "));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("trailing whitespace"));
        REQUIRE (parsed.errorLine == 10);
    }

    SECTION ("double space between fields")
    {
        const auto parsed = parseGolden (fixtureWith ("0 90 3C 64", "0 90  3C 64"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("double space"));
        REQUIRE (parsed.errorLine == 10);
    }

    SECTION ("wrong magic")
    {
        const auto parsed = parseGolden (fixtureWith ("# arpbox-golden 1", "# arpbox-goldenz 1"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("wrong magic"));
        REQUIRE (parsed.errorLine == 1);
    }

    SECTION ("unknown format version")
    {
        const auto parsed = parseGolden (fixtureWith ("# arpbox-golden 1", "# arpbox-golden 2"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("unsupported golden format version 2"));
        REQUIRE (parsed.errorLine == 1);
    }

    SECTION ("non-numeric format version")
    {
        const auto parsed = parseGolden (fixtureWith ("# arpbox-golden 1", "# arpbox-golden one"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("non-numeric format version"));
        REQUIRE (parsed.errorLine == 1);
    }

    SECTION ("malformed header line (no colon)")
    {
        const auto parsed = parseGolden (fixtureWith ("# bpm: 125", "# bpm 125"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("no ':' found"));
        REQUIRE (parsed.errorLine == 4);
    }

    SECTION ("unknown header field")
    {
        const auto parsed = parseGolden (fixtureWith ("# bpm: 125", "# tempo: 125"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("unknown header field 'tempo'"));
        REQUIRE (parsed.errorLine == 4);
    }

    SECTION ("duplicate header field")
    {
        const auto parsed = parseGolden (fixtureWith ("# bpm: 125\n", "# bpm: 125\n# bpm: 130\n"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("duplicate header field 'bpm'"));
        REQUIRE (parsed.errorLine == 5);
    }

    SECTION ("missing required header field")
    {
        const auto parsed = parseGolden (fixtureWith ("# gridPpq: 0.25\n", ""));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("gridPpq"));
        REQUIRE (parsed.error.contains ("header field"));
    }

    SECTION ("header value with no space after the colon")
    {
        const auto parsed = parseGolden (fixtureWith ("# bpm: 125", "# bpm:125"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("must be followed by ': '"));
        REQUIRE (parsed.errorLine == 4);
    }

    SECTION ("header value padded with an extra space")
    {
        const auto parsed = parseGolden (fixtureWith ("# bpm: 125", "# bpm:  125"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("space-padded value"));
        REQUIRE (parsed.errorLine == 4);
    }

    SECTION ("non-numeric header value")
    {
        const auto parsed = parseGolden (fixtureWith ("# spanSamples: 1024", "# spanSamples: lots"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("not a decimal integer"));
        REQUIRE (parsed.errorLine == 6);
    }

    SECTION ("header line after event data")
    {
        const auto parsed = parseGolden (fixtureWith ("240 80 3C 00\n", "# bpm2: 1\n240 80 3C 00\n"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("header line after event data"));
        REQUIRE (parsed.errorLine == 11);
    }

    SECTION ("declared event count does not match the body")
    {
        const auto parsed = parseGolden (fixtureWith ("# events: 2", "# events: 3"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("declares 3 events"));
        REQUIRE (parsed.error.contains ("contains 2"));
    }

    SECTION ("empty event list")
    {
        const auto parsed = parseGolden (
            fixtureWith ("# events: 2", "# events: 0").replace ("0 90 3C 64\n", "").replace ("240 80 3C 00\n", ""));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("no events"));
        REQUIRE (parsed.errorLine == 0);
    }

    SECTION ("event line with zero MIDI bytes")
    {
        const auto parsed = parseGolden (fixtureWith ("0 90 3C 64", "0"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("no MIDI bytes"));
        REQUIRE (parsed.errorLine == 10);
    }

    SECTION ("byte token with one hex digit")
    {
        const auto parsed = parseGolden (fixtureWith ("0 90 3C 64", "0 90 3C 6"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("not two uppercase hex digits"));
        REQUIRE (parsed.errorLine == 10);
    }

    SECTION ("byte token with three hex digits")
    {
        const auto parsed = parseGolden (fixtureWith ("0 90 3C 64", "0 90 3C 640"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("not two uppercase hex digits"));
        REQUIRE (parsed.errorLine == 10);
    }

    SECTION ("byte token that is not hex")
    {
        const auto parsed = parseGolden (fixtureWith ("0 90 3C 64", "0 90 3C GG"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("not two uppercase hex digits"));
        REQUIRE (parsed.errorLine == 10);
    }

    SECTION ("lowercase byte token")
    {
        const auto parsed = parseGolden (fixtureWith ("0 90 3C 64", "0 90 3c 64"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("not two uppercase hex digits"));
        REQUIRE (parsed.errorLine == 10);
    }

    SECTION ("non-numeric sample position")
    {
        const auto parsed = parseGolden (fixtureWith ("0 90 3C 64", "zero 90 3C 64"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("is not a decimal integer"));
        REQUIRE (parsed.errorLine == 10);
    }

    SECTION ("negative sample position")
    {
        const auto parsed = parseGolden (fixtureWith ("0 90 3C 64", "-1 90 3C 64"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("negative sample position"));
        REQUIRE (parsed.errorLine == 10);
    }

    SECTION ("non-monotonic sample positions")
    {
        // §5.5: events are sample-sorted. Encoding that into the format means a
        // golden can never record a stream the engine is forbidden to emit.
        const auto parsed = parseGolden (fixtureWith ("0 90 3C 64", "480 90 3C 64"));
        INFO (parsed.error);
        REQUIRE_FALSE (parsed.ok);
        REQUIRE (parsed.error.contains ("sample-sorted"));
        REQUIRE (parsed.errorLine == 11);
    }

    SECTION ("equal sample positions are ALLOWED (two events on one sample)")
    {
        const auto parsed = parseGolden (fixtureWith ("240 80 3C 00", "0 80 3C 00"));
        INFO (parsed.error);
        REQUIRE (parsed.ok);
    }
}

TEST_CASE ("determinism/golden: a missing golden file is an error, never a silent skip", "[unit][determinism]")
{
    const UniqueTempDir temp;

    const auto missing = loadGolden ("no-such-golden", temp.dir);
    INFO (missing.error);
    REQUIRE_FALSE (missing.ok);
    REQUIRE (missing.error.contains ("not found"));
    REQUIRE (missing.events.empty ());

    // And a comparison against an unreadable golden fails rather than vacuously passing.
    const auto comparison = compareToGolden (makeRender (), missing);
    INFO (comparison.report);
    REQUIRE_FALSE (comparison.matches);
    REQUIRE (comparison.report.contains ("COULD NOT BE READ"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Comparison semantics
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/golden: comparison catches a single changed byte and names the index", "[unit][determinism]")
{
    const auto render = makeRender ();
    const auto golden = parseGolden (serializeGolden (render, makeHeader (render)));
    REQUIRE (golden.ok);

    SECTION ("identical render matches")
    {
        const auto comparison = compareToGolden (render, golden);
        INFO (comparison.report);
        REQUIRE (comparison.matches);
        REQUIRE (comparison.report.contains ("MATCHES"));
    }

    SECTION ("one changed MIDI byte mismatches, and the report names the index")
    {
        auto mutated = render;
        mutated.events[2].message = juce::MidiMessage::controllerEvent (1, 123, 1); // 00 -> 01

        const auto comparison = compareToGolden (mutated, golden);
        INFO (comparison.report);
        REQUIRE_FALSE (comparison.matches);
        REQUIRE (comparison.report.contains ("first difference at index 2"));
        REQUIRE (comparison.report.contains (goldenGovernanceRule));
    }

    SECTION ("one moved sample position mismatches")
    {
        auto mutated = render;
        mutated.events[1].absoluteSample = 241;

        const auto comparison = compareToGolden (mutated, golden);
        INFO (comparison.report);
        REQUIRE_FALSE (comparison.matches);
        REQUIRE (comparison.report.contains ("first difference at index 1"));
    }

    SECTION ("a dropped event mismatches")
    {
        auto mutated = render;
        mutated.events.pop_back ();

        const auto comparison = compareToGolden (mutated, golden);
        INFO (comparison.report);
        REQUIRE_FALSE (comparison.matches);
    }
}

TEST_CASE ("determinism/golden: header-comment edits do not change the verdict", "[unit][determinism]")
{
    const auto render = makeRender ();
    const auto original = serializeGolden (render, makeHeader (render));

    // Every one of these is metadata. None of them describes the performance, so
    // none of them may flip the verdict — comparison runs on the parsed event vector.
    const auto edited = juce::String (original)
                            .replaceFirstOccurrenceOf ("# name: fixture", "# name: fixture-renamed")
                            .replaceFirstOccurrenceOf ("# bpm: 125", "# bpm: 90")
                            .replaceFirstOccurrenceOf ("# gridPpq: 0.25", "# gridPpq: 0.125")
                            .replaceFirstOccurrenceOf ("# bakedAtBlockSize: 128", "# bakedAtBlockSize: 2048");

    REQUIRE (edited != original);

    const auto golden = parseGolden (edited);
    INFO (golden.error);
    REQUIRE (golden.ok);
    REQUIRE (golden.header.bakedAtBlockSize == 2048);

    // bakedAtBlockSize is NON-NORMATIVE: recorded for triage, never compared. A render
    // at 128 must still match a golden baked at 2048 — that IS buffer-size independence.
    const auto comparison = compareToGolden (render, golden);
    INFO (comparison.report);
    REQUIRE (comparison.matches);
}

// ─────────────────────────────────────────────────────────────────────────────
// Directory listing
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/golden: listGoldenFiles handles empty, absent and populated directories", "[unit][determinism]")
{
    const UniqueTempDir temp;

    // Empty directory — legitimate until the first golden is baked.
    REQUIRE (listGoldenFiles (temp.dir).empty ());

    // Absent directory — an error-free empty list, not a crash.
    REQUIRE (listGoldenFiles (temp.dir.getChildFile ("does-not-exist")).empty ());

    const auto render = makeRender ();
    REQUIRE (writeGolden (render, makeHeader (render, "bravo"), temp.dir).wasOk ());
    REQUIRE (writeGolden (render, makeHeader (render, "alpha"), temp.dir).wasOk ());

    // Non-.txt files (tests/golden/README.md) are not goldens.
    REQUIRE (temp.dir.getChildFile ("README.md").replaceWithText ("not a golden"));

    const auto names = listGoldenFiles (temp.dir);
    REQUIRE (names.size () == 2);
    REQUIRE (names[0] == "alpha");
    REQUIRE (names[1] == "bravo");
}

// ─────────────────────────────────────────────────────────────────────────────
// THE REGENERATION INTERLOCK
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/golden: regeneration writes the file AND STILL FAILS", "[unit][determinism]")
{
    const UniqueTempDir temp;
    const auto render = makeRender ();
    const auto header = makeHeader (render, "interlock");
    const auto path = goldenPath ("interlock", temp.dir);

    GoldenCheckOptions regenerating;
    regenerating.directory = temp.dir;
    regenerating.regenerate = true;

    GoldenCheckOptions verifying;
    verifying.directory = temp.dir;
    verifying.regenerate = false;

    SECTION ("a MISSING golden is created and the run is RED")
    {
        REQUIRE_FALSE (path.existsAsFile ());

        const auto check = checkGolden (render, header, verifying);
        INFO (check.report);
        REQUIRE (check.wroteFile);
        REQUIRE (path.existsAsFile ());
        REQUIRE_FALSE (check.passed); // ← the interlock
        REQUIRE (check.report.contains ("GOLDEN CREATED"));
        REQUIRE (check.report.contains ("re-run"));

        // Re-running the same render against the now-reviewed file is green, and
        // writes nothing.
        const auto rerun = checkGolden (render, header, verifying);
        INFO (rerun.report);
        REQUIRE (rerun.passed);
        REQUIRE_FALSE (rerun.wroteFile);
    }

    SECTION ("an EXISTING golden is rewritten and the run is STILL RED")
    {
        // Bake a reviewed golden of a DIFFERENT performance.
        auto baseline = render;
        baseline.events[0].message = juce::MidiMessage::noteOn (1, 61, static_cast<juce::uint8> (100));
        REQUIRE (writeGolden (baseline, makeHeader (baseline, "interlock"), temp.dir).wasOk ());
        REQUIRE (checkGolden (baseline, header, verifying).passed);

        // The engine "changes sound"; the verifying run goes red.
        const auto detected = checkGolden (render, header, verifying);
        INFO (detected.report);
        REQUIRE_FALSE (detected.passed);
        REQUIRE_FALSE (detected.wroteFile);

        // Somebody reaches for regeneration. The file IS rewritten — and the run is
        // still red, with the diff and the governance rule printed.
        const auto regenerated = checkGolden (render, header, regenerating);
        INFO (regenerated.report);
        REQUIRE (regenerated.wroteFile);
        REQUIRE_FALSE (regenerated.passed); // ← the interlock
        REQUIRE (regenerated.report.contains ("REGENERATION REQUESTED"));
        REQUIRE (regenerated.report.contains ("first difference at index 0"));
        REQUIRE (regenerated.report.contains (goldenGovernanceRule));

        // The rewrite really happened: the file on disk now describes the new render.
        const auto reloaded = loadGolden ("interlock", temp.dir);
        REQUIRE (reloaded.ok);
        REQUIRE (compareToGolden (render, reloaded).matches);

        // Only a SEPARATE, non-regenerating run can be green.
        const auto clean = checkGolden (render, header, verifying);
        INFO (clean.report);
        REQUIRE (clean.passed);
        REQUIRE_FALSE (clean.wroteFile);
    }

    SECTION ("INVARIANT: no code path both writes a file and reports pass")
    {
        // Exhaustive over the reachable states: {golden absent, present-matching,
        // present-mismatching} x {verify, regenerate}. `wroteFile && passed` must
        // never hold. This is the property the whole design exists to guarantee.
        const std::array<bool, 2> regenerateStates { false, true };

        for (const bool regenerate : regenerateStates)
        {
            for (int state = 0; state < 3; ++state)
            {
                const UniqueTempDir scratch;
                GoldenCheckOptions options;
                options.directory = scratch.dir;
                options.regenerate = regenerate;

                if (state == 1)
                    REQUIRE (writeGolden (render, makeHeader (render, "interlock"), scratch.dir).wasOk ());

                if (state == 2)
                {
                    auto other = render;
                    other.events.pop_back ();
                    REQUIRE (writeGolden (other, makeHeader (other, "interlock"), scratch.dir).wasOk ());
                }

                const auto check = checkGolden (render, header, options);
                INFO ("regenerate=" << (regenerate ? 1 : 0) << " state=" << state);
                INFO (check.report);
                REQUIRE_FALSE ((check.wroteFile && check.passed)); // extra parens: Catch2 cannot decompose &&

                // The only green combination is: not regenerating, golden present and matching.
                REQUIRE (check.passed == (! regenerate && state == 1));
            }
        }
    }
}

TEST_CASE ("determinism/golden: the environment variable is what gates regeneration", "[unit][determinism]")
{
    // The gate is an ENV VAR, not a CMake option: a configure-time flag persists in
    // build/ across sessions and would leave the release gate permanently green.
    SECTION ("unset ⇒ off")
    {
        const ScopedRegenerateEnv env (nullptr);
        REQUIRE_FALSE (regenerationRequested ());
    }

    SECTION ("=1 ⇒ on")
    {
        const ScopedRegenerateEnv env ("1");
        REQUIRE (regenerationRequested ());
    }

    SECTION ("=0 ⇒ off (reads as the user intends, not as getenv != nullptr)")
    {
        const ScopedRegenerateEnv env ("0");
        REQUIRE_FALSE (regenerationRequested ());
    }

    SECTION ("=false ⇒ off")
    {
        const ScopedRegenerateEnv env ("false");
        REQUIRE_FALSE (regenerationRequested ());
    }

    SECTION ("empty ⇒ off")
    {
        const ScopedRegenerateEnv env ("");
        REQUIRE_FALSE (regenerationRequested ());
    }

    SECTION ("maybeRegenerate honours the variable and writes only when set")
    {
        const UniqueTempDir temp;
        const auto render = makeRender ();
        const auto header = makeHeader (render, "envgated");

        {
            const ScopedRegenerateEnv env (nullptr);
            REQUIRE_FALSE (maybeRegenerate (render, header, temp.dir));
            REQUIRE_FALSE (goldenPath ("envgated", temp.dir).existsAsFile ());
        }

        {
            const ScopedRegenerateEnv env ("1");
            REQUIRE (maybeRegenerate (render, header, temp.dir));
            REQUIRE (goldenPath ("envgated", temp.dir).existsAsFile ());

            // …and the same variable makes checkGolden red even with the file present
            // and matching, because a run that rewrites goldens must never be green.
            GoldenCheckOptions fromEnv;
            fromEnv.directory = temp.dir; // regenerate left as nullopt ⇒ consults the env
            const auto check = checkGolden (render, header, fromEnv);
            INFO (check.report);
            REQUIRE (check.wroteFile);
            REQUIRE_FALSE (check.passed);
        }
    }
}

TEST_CASE ("determinism/golden: the env guard restores the prior value instead of unsetting it", "[unit][determinism]")
{
    // THE REGRESSION TEST FOR THE GUARD ITSELF, and the one that was missing.
    //
    // Every SECTION above scopes a `ScopedRegenerateEnv` and then asserts inside
    // it — so all of them pass whether the destructor RESTORES the prior value or
    // just clears it. The damage from clearing lands on whatever runs NEXT, which
    // is exactly the blind spot: `tests/determinism_goldens.cpp`'s six baked
    // goldens are declared after this file, share the process under a direct
    // binary invocation, and would silently stop regenerating.
    //
    // So this case asserts ACROSS a destruction boundary, which is the only place
    // the difference is observable.

    // Establish an outer "the user asked for regeneration" state, itself scoped so
    // this case cannot leak into the rest of the run.
    const ScopedRegenerateEnv outer ("1");
    REQUIRE (regenerationRequested ());

    {
        // An inner guard turns it off, as the SECTIONs above do…
        const ScopedRegenerateEnv inner (nullptr);
        REQUIRE_FALSE (regenerationRequested ());
    }

    // …and its destruction must hand the outer state back. An unconditional
    // `::unsetenv` here reads as false and is the whole defect.
    REQUIRE (regenerationRequested ());

    {
        const ScopedRegenerateEnv inner ("0");
        REQUIRE_FALSE (regenerationRequested ());
    }
    REQUIRE (regenerationRequested ());

    // The value is restored VERBATIM, not merely to some truthy string — a guard
    // that "restored" by writing "1" would pass the checks above while corrupting
    // a caller who had set something else.
    {
        const ScopedRegenerateEnv verbatim ("yes-please");
        REQUIRE (regenerationRequested ());

        {
            const ScopedRegenerateEnv inner (nullptr);
            REQUIRE_FALSE (regenerationRequested ());
        }

        const char* const restored = ::getenv (goldenRegenerateEnvVar);
        REQUIRE (restored != nullptr);
        REQUIRE (std::string (restored) == "yes-please");
    }

    // ABSENT and "set to empty" are different states, and the guard must not
    // collapse one into the other on the way out.
    {
        const ScopedRegenerateEnv emptyValue ("");
        REQUIRE (::getenv (goldenRegenerateEnvVar) != nullptr);

        {
            const ScopedRegenerateEnv inner ("1");
            REQUIRE (regenerationRequested ());
        }

        const char* const restored = ::getenv (goldenRegenerateEnvVar);
        REQUIRE (restored != nullptr);             // still SET…
        REQUIRE (std::string (restored).empty ()); // …and still empty
    }

    // Unwinding the whole nest leaves the outer state intact.
    REQUIRE (regenerationRequested ());
}

// ─────────────────────────────────────────────────────────────────────────────
// Writer hygiene
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("determinism/golden: the writer produces LF-only text and rejects unusable names", "[unit][determinism]")
{
    const UniqueTempDir temp;
    const auto render = makeRender ();

    REQUIRE (writeGolden (render, makeHeader (render, "lf-check"), temp.dir).wasOk ());

    juce::MemoryBlock raw;
    REQUIRE (goldenPath ("lf-check", temp.dir).loadFileAsData (raw));
    const juce::String bytes (static_cast<const char*> (raw.getData ()), raw.getSize ());
    REQUIRE_FALSE (bytes.containsChar ('\r'));
    REQUIRE (bytes.endsWithChar ('\n'));

    // A name that cannot be a single-line header value or a filename is refused up
    // front rather than emitting a file its own parser would reject.
    for (const auto* bad : { "", "has space", "has/slash", "has\nnewline", ".hidden" })
    {
        INFO (bad);
        REQUIRE (writeGolden (render, makeHeader (render, bad), temp.dir).failed ());
    }
}
