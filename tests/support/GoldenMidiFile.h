#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// GoldenMidiFile — the on-disk half of the determinism contract
// (docs/ARCHITECTURE.md §1.2 "same (pattern, seeds, N bars) ⇒ byte-identical
// MIDI, forever … enforced by a golden-MIDI test suite and is a release gate";
// docs/INSTRUCTIONS.md Phase 6.4). support/MidiRenderHarness.h produces the
// in-memory render; THIS header persists it, reloads it, and compares.
//
// ── WHY TEXT AND NOT `.mid` (Phase 6.4 decision — see tests/golden/README.md) ──
// A Standard MIDI File stores TICKS, not samples. Pinning 1 tick = 1 sample needs
// a fabricated 3000-6000 BPM tempo map that differs per sample rate, and
// juce::MidiFile::writeTo's running-status / varlen encoding means a JUCE
// submodule bump could turn every golden red with ZERO engine change. A gate that
// fires on the wrong signal trains people to regenerate, which destroys the gate.
// So the golden format is canonical TEXT: absolute sample position + raw MIDI
// bytes, one event per line, diffable in a code review.
// The SMF writer's real home is Phase 18 (§9 MIDI drag-out), where "exported .mid
// re-imported equals live-rendered golden" makes it a product output tested
// AGAINST these files rather than the reference itself.
//
// ── THE FILE FORMAT (version 1) ───────────────────────────────────────────────
//
//   # arpbox-golden 1
//   # name: baseline-4bar
//   # sampleRate: 48000
//   # bpm: 125
//   # gridPpq: 0.25
//   # spanSamples: 368640
//   # events: 128
//   # rngVersion: 0
//   # bakedAtBlockSize: 128
//   0 90 3C 64
//   2880 80 3C 00
//   5760 90 3E 64
//
// Line 1 is a MAGIC + VERSION line. `MidiRenderResult::toByteStream()` deliberately
// carries neither; on disk you need both, so a future format change is a loud parse
// error instead of a silent mis-read.
//
// Body grammar, STRICT, no tolerance:
//     <decimal int64> SP <2 uppercase hex digits> ( SP <2 uppercase hex digits> )* LF
// No blank lines, no trailing whitespace, no tabs, no CR, LF-only (see
// .gitattributes), and the file must end with LF.
//
// ── WHAT IS AND IS NOT COMPARED ───────────────────────────────────────────────
// Comparison runs on the PARSED EVENT VECTOR, never on raw file bytes. Editing a
// header comment is therefore not a false failure, while any change to the
// performance is a failure.
//
//   • `bakedAtBlockSize` is **NON-NORMATIVE**. It is recorded for triage and is
//     NEVER compared. MidiRenderHarness.h is right that a golden must not encode
//     the buffer size it happened to be rendered at — buffer-size independence is
//     the very property the suite asserts. Recording it costs nothing and tells a
//     future reader which render produced the file. DO NOT start asserting on it.
//   • `sampleRate` / `spanSamples` are reported as advisory lines when they differ
//     but likewise do not decide `matches` — a sample-rate change already moves
//     every absolute sample position, so the event diff is the honest signal.
//   • `rngVersion` is reserved at 0 today so Phase 7.1 / Phase 12's versioned
//     `RngStream` (§5.2 "RNG streams are versioned in the schema") does not force
//     reformatting every existing golden when it lands.
//
// ── THE REGENERATION INTERLOCK ────────────────────────────────────────────────
// Regeneration is gated on the ENVIRONMENT VARIABLE `ARPBOX_REGENERATE_GOLDENS`,
// not a CMake option: a configure-time flag persists in `build/` across sessions
// and nobody re-reads CMakeCache.txt, so "left it on by accident" would leave the
// release gate permanently green and worthless.
//
// **When regeneration is requested, the check writes the new file AND STILL FAILS.**
// There is no invocation of this suite that both rewrites a golden and reports
// green. A missing golden behaves the same way: written, then failed with
// "golden created — review and re-run". The workflow is deliberately:
//
//     ARPBOX_REGENERATE_GOLDENS=1 ctest … -L determinism   → RED
//     git diff tests/golden/                               → read it, decide
//     ctest … -L determinism                               → GREEN (or the diff
//                                                            was a real defect)
//
// `writeGolden` compiles UNCONDITIONALLY (it is not #ifdef'd out) so it cannot
// bit-rot; only its EXECUTION is gated.
//
// A GOLDEN DIFF IS A FINDING, NEVER SOMETHING TO SILENTLY REGENERATE.
//
// Header-only, `namespace arpbox::testing`, matching support/MidiRenderHarness.h.
// ─────────────────────────────────────────────────────────────────────────────

#include "support/MidiRenderHarness.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

// The golden directory is injected as a SOURCE-TREE path by tests/CMakeLists.txt
// (the compiled-target analogue of the `-DENGINE_DIR=` idiom already used there).
// Deliberately NOT file(COPY)'d into the build tree: editing a golden and
// forgetting to rebuild would silently read a stale copy and PASS, which is the
// worst failure class a determinism gate can have.
#ifndef ARPBOX_GOLDEN_DIR
#    error "ARPBOX_GOLDEN_DIR is not defined — add it to target_compile_definitions in tests/CMakeLists.txt"
#endif

namespace arpbox::testing
{
// ─────────────────────────────────────────────────────────────────────────────
// Format constants
// ─────────────────────────────────────────────────────────────────────────────

/** Magic token on line 1. Changing it is a breaking format change. */
inline constexpr const char* goldenMagic = "arpbox-golden";

/** Format version this build reads and writes. Bump ⇒ every older file is a loud
    parse error, which is the point of having a version at all. */
inline constexpr int goldenFormatVersion = 1;

/** Environment variable that enables the write-and-still-fail regeneration path. */
inline constexpr const char* goldenRegenerateEnvVar = "ARPBOX_REGENERATE_GOLDENS";

/** The governance rule, printed verbatim whenever a golden is written, so the
    person who ran the regeneration reads it at the moment it matters. */
inline constexpr const char* goldenGovernanceRule =
    "A golden diff is a FINDING, never something to silently regenerate.\n"
    "  1. Read `git diff tests/golden/` and explain every changed line.\n"
    "  2. If the engine changed sound, that is either a defect or a deliberate,\n"
    "     justified change (ARCHITECTURE §5.2 requires an RNG version bump +\n"
    "     migration note for anything that alters output for existing seeds).\n"
    "  3. Committing a regenerated golden requires an explicit justification line\n"
    "     in the commit body (.claude/rules/git-conventions.md).\n"
    "This run FAILED ON PURPOSE: regeneration and a green report are mutually\n"
    "exclusive by construction. Re-run without ARPBOX_REGENERATE_GOLDENS to verify.";

// ─────────────────────────────────────────────────────────────────────────────
// Header / file / comparison types
// ─────────────────────────────────────────────────────────────────────────────

/** The `# key: value` block at the top of a golden file.

    `name`, `bpm`, `gridPpq` and `rngVersion` describe the SCENARIO and are supplied
    by the test. `sampleRate`, `spanSamples`, `declaredEvents` and `bakedAtBlockSize`
    are DERIVED FACTS about the render: `serializeGolden` always takes them from the
    `MidiRenderResult` and ignores whatever is in the struct, so a hand-filled header
    can never produce a file whose declared event count disagrees with its body. */
struct GoldenHeader
{
    juce::String name;            ///< Scenario name; also the file's base name.
    double sampleRate = 48000.0;  ///< Derived from the render on write.
    double bpm = 120.0;           ///< Scenario tempo (diagnostic).
    double gridPpq = 0.25;        ///< Step grid in quarter notes (diagnostic).
    std::int64_t spanSamples = 0; ///< Derived from the render on write.
    int declaredEvents = 0;       ///< Derived from the render on write; cross-checked on read.
    int bakedAtBlockSize = 128;   ///< NON-NORMATIVE. Derived on write. NEVER compared.
    int rngVersion = 0;           ///< Reserved for Phase 7.1/12's versioned RngStream.
};

/** The result of reading a golden file. `ok == false` ⇒ `error` says what is wrong
    and `errorLine` is the 1-based line (0 for whole-file problems). A failed load is
    ALWAYS a test failure — a missing or unparseable golden must never be treated as
    "nothing to compare against". */
struct GoldenFile
{
    bool ok = false;
    juce::String error;
    int errorLine = 0;
    GoldenHeader header;
    std::vector<TimedMidiEvent> events;
};

/** Outcome of comparing a live render against a loaded golden. This RETURNS a report
    rather than asserting, so tests keep the suite's established shape —
    `INFO (cmp.report); REQUIRE (cmp.matches);` — and Catch2 reports the caller's
    line, not a line inside this header. */
struct GoldenComparison
{
    bool matches = false;
    juce::String report;
};

/** Outcome of the full check (regenerate ⁄ create ⁄ load ⁄ compare).

    INVARIANT, and it is the whole point of this header:
    **`wroteFile == true` implies `passed == false`.** Every branch that touches the
    filesystem sets `passed = false` unconditionally. */
struct GoldenCheck
{
    bool passed = false;
    bool wroteFile = false;
    juce::String report;
};

/** Knobs for `checkGolden`, used by this header's OWN tests to exercise the
    mechanism without a real golden or a mutated process environment. */
struct GoldenCheckOptions
{
    /** `nullopt` ⇒ consult `ARPBOX_REGENERATE_GOLDENS`. Set explicitly to drive the
        regeneration path deterministically from a test. */
    std::optional<bool> regenerate {};

    /** Default-constructed ⇒ `ARPBOX_GOLDEN_DIR`. Point it at a temp dir to test
        the mechanism without writing into the source tree. */
    juce::File directory {};
};

// ─────────────────────────────────────────────────────────────────────────────
// Small formatting / validation helpers
// ─────────────────────────────────────────────────────────────────────────────

/** Canonical, stable text for a header number: integral values print without a
    decimal point (48000, 125), fractional values print trimmed fixed-point (0.25).
    Avoids depending on juce::String(double)'s %g behaviour, which is not something a
    golden format should be pinned to. */
inline juce::String formatGoldenNumber (double value)
{
    if (std::isfinite (value) && value == std::floor (value) && std::abs (value) < 1.0e15)
        return juce::String (static_cast<std::int64_t> (value));

    juce::String text (value, 9);
    if (text.containsChar ('.'))
    {
        text = text.trimCharactersAtEnd ("0");
        if (text.endsWithChar ('.'))
            text << "0";
    }
    return text;
}

/** Two uppercase hex digits for one MIDI byte. */
inline juce::String formatGoldenByte (std::uint8_t byte)
{
    return juce::String::toHexString (static_cast<int> (byte)).paddedLeft ('0', 2).toUpperCase ();
}

/** A golden name must be usable as both a filename and a single-line header value. */
inline bool isValidGoldenName (const juce::String& name)
{
    if (name.isEmpty () || name.length () > 120)
        return false;

    for (auto character : name)
        if (! (juce::CharacterFunctions::isLetterOrDigit (character) || character == '-' || character == '_' ||
               character == '.'))
            return false;

    return ! name.startsWithChar ('.');
}

/** True for a set, non-disabling environment value ("", "0", "false", "no", "off"
    all read as OFF, so `ARPBOX_REGENERATE_GOLDENS=0` behaves the way a reader
    expects rather than the way `getenv != nullptr` would). */
inline bool isTruthyEnvValue (const char* value)
{
    if (value == nullptr)
        return false;

    const auto text = juce::String (value).trim ().toLowerCase ();
    return text.isNotEmpty () && text != "0" && text != "false" && text != "no" && text != "off";
}

/** Whether this process was asked to rewrite goldens. Reading the ENVIRONMENT (not a
    CMake cache entry) is deliberate — see the interlock note at the top. */
inline bool regenerationRequested ()
{
    return isTruthyEnvValue (std::getenv (goldenRegenerateEnvVar));
}

/** The golden directory: `directoryOverride` when non-empty, else the compiled-in
    source-tree path. */
inline juce::File goldenDirectory (const juce::File& directoryOverride = {})
{
    return directoryOverride == juce::File () ? juce::File (ARPBOX_GOLDEN_DIR) : directoryOverride;
}

/** `<dir>/<name>.txt`. `name` may be given with or without the extension. */
inline juce::File goldenPath (const juce::String& name, const juce::File& directory = {})
{
    const auto base = name.endsWithIgnoreCase (".txt") ? name.dropLastCharacters (4) : name;
    return goldenDirectory (directory).getChildFile (base + ".txt");
}

/** Base names (no extension) of every `*.txt` in the golden directory, sorted.
    Returns empty — never an error — for an empty or absent directory, because the
    directory is legitimately empty until the first golden is baked. */
inline std::vector<juce::String> listGoldenFiles (const juce::File& directory = {})
{
    std::vector<juce::String> names;

    const auto dir = goldenDirectory (directory);
    if (! dir.isDirectory ())
        return names;

    for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*.txt", juce::File::findFiles))
        names.push_back (entry.getFile ().getFileNameWithoutExtension ());

    std::sort (names.begin (),
               names.end (),
               [] (const juce::String& a, const juce::String& b) { return a.compare (b) < 0; });
    return names;
}

// ─────────────────────────────────────────────────────────────────────────────
// Serialization
// ─────────────────────────────────────────────────────────────────────────────

/** Renders `result` into the canonical golden text.

    The four DERIVED header fields (`sampleRate`, `spanSamples`, `declaredEvents`,
    `bakedAtBlockSize`) come from `result`, not from `header` — a serializer that let
    the caller declare a wrong event count could emit a file its own parser rejects.
    Everything else (`name`, `bpm`, `gridPpq`, `rngVersion`) comes from `header`. */
inline juce::String serializeGolden (const MidiRenderResult& result, const GoldenHeader& header)
{
    juce::String text;
    text << "# " << goldenMagic << " " << juce::String (goldenFormatVersion) << "\n"
         << "# name: " << header.name << "\n"
         << "# sampleRate: " << formatGoldenNumber (result.sampleRate) << "\n"
         << "# bpm: " << formatGoldenNumber (header.bpm) << "\n"
         << "# gridPpq: " << formatGoldenNumber (header.gridPpq) << "\n"
         << "# spanSamples: " << juce::String (result.numSamples) << "\n"
         << "# events: " << juce::String (static_cast<std::int64_t> (result.events.size ())) << "\n"
         << "# rngVersion: " << juce::String (header.rngVersion) << "\n"
         << "# bakedAtBlockSize: " << juce::String (result.blockSize) << "\n";

    for (const auto& event : result.events)
    {
        text << juce::String (event.absoluteSample);

        for (int i = 0; i < event.numBytes (); ++i)
            text << " " << formatGoldenByte (event.bytes ()[i]);

        text << "\n";
    }

    return text;
}

/** A `GoldenHeader` with the derived fields already filled from `result` — handy when
    a test wants a complete header (e.g. to compare against a parsed one). */
inline GoldenHeader
headerFor (const MidiRenderResult& result, const juce::String& name, double bpm, double gridPpq, int rngVersion = 0)
{
    GoldenHeader header;
    header.name = name;
    header.sampleRate = result.sampleRate;
    header.bpm = bpm;
    header.gridPpq = gridPpq;
    header.spanSamples = result.numSamples;
    header.declaredEvents = static_cast<int> (result.events.size ());
    header.bakedAtBlockSize = result.blockSize;
    header.rngVersion = rngVersion;
    return header;
}

// ─────────────────────────────────────────────────────────────────────────────
// Parsing
// ─────────────────────────────────────────────────────────────────────────────

namespace detail
{
    inline GoldenFile goldenError (int line, const juce::String& message)
    {
        GoldenFile file;
        file.ok = false;
        file.errorLine = line;
        file.error = message;
        return file;
    }

    /** Strict decimal int64: digits with an optional leading '-', fully consumed. */
    inline bool parseGoldenInt (const std::string& token, std::int64_t& out)
    {
        if (token.empty ())
            return false;

        const bool negative = token[0] == '-';
        if (token.size () == (negative ? 1u : 0u))
            return false;

        for (std::size_t i = negative ? 1u : 0u; i < token.size (); ++i)
            if (token[i] < '0' || token[i] > '9')
                return false;

        try
        {
            out = static_cast<std::int64_t> (std::stoll (token));
        }
        catch (...)
        {
            return false;
        }
        return true;
    }

    /** Strict decimal double: fully consumed by strtod, no leading/trailing junk, no
        hex/inf/nan spellings. */
    inline bool parseGoldenDouble (const std::string& token, double& out)
    {
        if (token.empty ())
            return false;

        for (const char character : token)
            if (! ((character >= '0' && character <= '9') || character == '-' || character == '+' || character == '.' ||
                   character == 'e' || character == 'E'))
                return false;

        char* end = nullptr;
        const double value = std::strtod (token.c_str (), &end);
        if (end != token.c_str () + token.size ())
            return false;

        if (! std::isfinite (value))
            return false;

        out = value;
        return true;
    }

    inline bool isUppercaseHexDigit (char character)
    {
        return (character >= '0' && character <= '9') || (character >= 'A' && character <= 'F');
    }

    /** Rebuilds a juce::MidiMessage from raw bytes. Returns false if the bytes do not
        survive the round trip, which keeps "the file says these bytes" and "the compared
        message carries these bytes" the same statement. */
    inline bool makeMidiMessage (const std::vector<std::uint8_t>& bytes, juce::MidiMessage& out)
    {
        if (bytes.empty ())
            return false;

        if (bytes.size () == 1)
            out = juce::MidiMessage (bytes[0]);
        else if (bytes.size () == 2)
            out = juce::MidiMessage (bytes[0], bytes[1]);
        else if (bytes.size () == 3)
            out = juce::MidiMessage (bytes[0], bytes[1], bytes[2]);
        else
        {
            int used = 0;
            out = juce::MidiMessage (bytes.data (), static_cast<int> (bytes.size ()), used, 0);
            if (used != static_cast<int> (bytes.size ()))
                return false;
        }

        return out.getRawDataSize () == static_cast<int> (bytes.size ()) &&
               std::memcmp (out.getRawData (), bytes.data (), bytes.size ()) == 0;
    }
} // namespace detail

/** Parses golden text. Never throws; every rejection is a distinct `error` string
    plus the 1-based `errorLine` (0 for whole-file problems).

    The rejection set is deliberately exhaustive — a parser that accepts everything
    validates nothing, so tests/golden_format.cpp asserts each of these individually:
      missing file (loadGolden) · empty file · no trailing LF · blank line · CR ·
      tab · trailing whitespace · double space · wrong magic · unsupported version ·
      malformed header line · unknown header key · duplicate header key · missing
      required header key · header line after event data · non-numeric sample
      position · negative sample position · non-monotonic sample positions · event
      line with zero MIDI bytes · byte token that is not two uppercase hex digits ·
      declared event count != parsed count · empty event list. */
inline GoldenFile parseGolden (const juce::String& text)
{
    using detail::goldenError;

    const std::string raw = text.toStdString ();
    if (raw.empty ())
        return goldenError (0, "golden file is empty");

    // Split on LF, keeping every line verbatim so whitespace violations are visible.
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < raw.size (); ++i)
    {
        if (raw[i] == '\n')
        {
            lines.push_back (raw.substr (start, i - start));
            start = i + 1;
        }
    }

    if (start != raw.size ())
        return goldenError (static_cast<int> (lines.size ()) + 1,
                            "golden file does not end with a newline (LF); the format requires a final LF");

    GoldenFile file;
    bool sawEventLine = false;
    bool haveKey[8] = {};
    static const char* const requiredKeys[8] = { "name",   "sampleRate", "bpm",         "gridPpq",
                                                 "events", "rngVersion", "spanSamples", "bakedAtBlockSize" };
    std::int64_t previousSample = 0;

    for (std::size_t index = 0; index < lines.size (); ++index)
    {
        const int lineNumber = static_cast<int> (index) + 1;
        const std::string& line = lines[index];

        if (line.empty ())
            return goldenError (lineNumber, "blank line; the golden format allows no blank lines");

        if (line.find ('\r') != std::string::npos)
            return goldenError (lineNumber, "carriage return (CRLF line ending); goldens are LF-only (.gitattributes)");

        if (line.find ('\t') != std::string::npos)
            return goldenError (lineNumber, "tab character; fields are separated by a single space");

        if (line.back () == ' ')
            return goldenError (lineNumber, "trailing whitespace");

        // ── Line 1: magic + version ───────────────────────────────────────────
        if (index == 0)
        {
            const std::string expectedPrefix = std::string ("# ") + goldenMagic + " ";
            if (line.rfind (expectedPrefix, 0) != 0)
                return goldenError (1,
                                    juce::String ("wrong magic line; expected '# ") + goldenMagic + " " +
                                        juce::String (goldenFormatVersion) + "' but found '" + juce::String (line) +
                                        "'");

            std::int64_t version = 0;
            if (! detail::parseGoldenInt (line.substr (expectedPrefix.size ()), version))
                return goldenError (1, "magic line has a non-numeric format version");

            if (version != goldenFormatVersion)
                return goldenError (1,
                                    "unsupported golden format version " + juce::String (version) +
                                        "; this build understands version " + juce::String (goldenFormatVersion));
            continue;
        }

        // ── Header lines ──────────────────────────────────────────────────────
        if (line[0] == '#')
        {
            if (sawEventLine)
                return goldenError (lineNumber, "header line after event data; the header block must come first");

            if (line.rfind ("# ", 0) != 0)
                return goldenError (lineNumber, "malformed header line; expected '# key: value'");

            const std::string body = line.substr (2);
            const auto colon = body.find (':');
            if (colon == std::string::npos)
                return goldenError (lineNumber, "malformed header line; expected '# key: value' (no ':' found)");

            const std::string key = body.substr (0, colon);
            std::string value = body.substr (colon + 1);

            if (key.empty () || key.find (' ') != std::string::npos)
                return goldenError (lineNumber, "malformed header key '" + juce::String (key) + "'");

            if (value.empty () || value[0] != ' ')
                return goldenError (lineNumber,
                                    "header field '" + juce::String (key) + "' must be followed by ': ' and a value");

            value.erase (0, 1);
            if (value.empty () || value[0] == ' ')
                return goldenError (lineNumber,
                                    "header field '" + juce::String (key) + "' has an empty or space-padded value");

            int keyIndex = -1;
            for (int k = 0; k < 8; ++k)
                if (key == requiredKeys[k])
                    keyIndex = k;

            if (keyIndex < 0)
                return goldenError (lineNumber, "unknown header field '" + juce::String (key) + "'");

            if (haveKey[keyIndex])
                return goldenError (lineNumber, "duplicate header field '" + juce::String (key) + "'");

            haveKey[keyIndex] = true;

            std::int64_t integerValue = 0;
            double doubleValue = 0.0;

            if (key == "name")
            {
                file.header.name = juce::String (value);
                if (! isValidGoldenName (file.header.name))
                    return goldenError (lineNumber, "invalid golden name '" + file.header.name + "'");
            }
            else if (key == "sampleRate" || key == "bpm" || key == "gridPpq")
            {
                if (! detail::parseGoldenDouble (value, doubleValue))
                    return goldenError (lineNumber,
                                        "header field '" + juce::String (key) + "' is not a finite decimal number");

                if (key == "sampleRate")
                    file.header.sampleRate = doubleValue;
                else if (key == "bpm")
                    file.header.bpm = doubleValue;
                else
                    file.header.gridPpq = doubleValue;
            }
            else
            {
                if (! detail::parseGoldenInt (value, integerValue))
                    return goldenError (lineNumber,
                                        "header field '" + juce::String (key) + "' is not a decimal integer");

                if (key == "spanSamples")
                    file.header.spanSamples = integerValue;
                else if (key == "events")
                    file.header.declaredEvents = static_cast<int> (integerValue);
                else if (key == "bakedAtBlockSize")
                    file.header.bakedAtBlockSize = static_cast<int> (integerValue);
                else
                    file.header.rngVersion = static_cast<int> (integerValue);
            }

            continue;
        }

        // ── Event lines ───────────────────────────────────────────────────────
        for (int k = 0; k < 8; ++k)
            if (! haveKey[k])
                return goldenError (lineNumber,
                                    juce::String ("event data before the header field '") + requiredKeys[k] + "'");

        sawEventLine = true;

        std::vector<std::string> tokens;
        std::size_t tokenStart = 0;
        for (std::size_t i = 0; i <= line.size (); ++i)
        {
            if (i == line.size () || line[i] == ' ')
            {
                tokens.push_back (line.substr (tokenStart, i - tokenStart));
                tokenStart = i + 1;
            }
        }

        for (const auto& token : tokens)
            if (token.empty ())
                return goldenError (lineNumber, "empty field (double space); fields are separated by a single space");

        std::int64_t sample = 0;
        if (! detail::parseGoldenInt (tokens[0], sample))
            return goldenError (lineNumber,
                                "sample position '" + juce::String (tokens[0]) + "' is not a decimal integer");

        if (sample < 0)
            return goldenError (lineNumber, "negative sample position " + juce::String (sample));

        if (tokens.size () < 2)
            return goldenError (lineNumber, "event line has no MIDI bytes");

        if (! file.events.empty () && sample < previousSample)
            return goldenError (lineNumber,
                                "sample position " + juce::String (sample) + " is before the previous event's " +
                                    juce::String (previousSample) + "; goldens are sample-sorted (ARCHITECTURE §5.5)");

        std::vector<std::uint8_t> bytes;
        bytes.reserve (tokens.size () - 1);
        for (std::size_t t = 1; t < tokens.size (); ++t)
        {
            const auto& token = tokens[t];
            if (token.size () != 2 || ! detail::isUppercaseHexDigit (token[0]) ||
                ! detail::isUppercaseHexDigit (token[1]))
                return goldenError (lineNumber,
                                    "byte token '" + juce::String (token) + "' is not two uppercase hex digits");

            const auto high = static_cast<std::uint8_t> (token[0] <= '9' ? token[0] - '0' : token[0] - 'A' + 10);
            const auto low = static_cast<std::uint8_t> (token[1] <= '9' ? token[1] - '0' : token[1] - 'A' + 10);
            bytes.push_back (static_cast<std::uint8_t> ((high << 4) | low));
        }

        juce::MidiMessage message;
        if (! detail::makeMidiMessage (bytes, message))
            return goldenError (lineNumber, "MIDI bytes do not round-trip through juce::MidiMessage");

        file.events.push_back (TimedMidiEvent { sample, message });
        previousSample = sample;
    }

    for (int k = 0; k < 8; ++k)
        if (! haveKey[k])
            return goldenError (0, juce::String ("missing required header field '") + requiredKeys[k] + "'");

    if (file.events.empty ())
        return goldenError (0, "golden contains no events; an empty golden asserts nothing");

    if (file.header.declaredEvents != static_cast<int> (file.events.size ()))
        return goldenError (0,
                            "header declares " + juce::String (file.header.declaredEvents) + " events but the body " +
                                "contains " + juce::String (static_cast<std::int64_t> (file.events.size ())));

    file.ok = true;
    return file;
}

/** Reads and parses `<goldenDir>/<name>.txt`. A MISSING FILE IS AN ERROR — this never
    silently auto-creates (that is `checkGolden`'s job, and even there it fails). */
inline GoldenFile loadGolden (const juce::String& name, const juce::File& directory = {})
{
    const auto path = goldenPath (name, directory);
    if (! path.existsAsFile ())
        return detail::goldenError (0, "golden file not found: " + path.getFullPathName ());

    return parseGolden (path.loadFileAsString ());
}

// ─────────────────────────────────────────────────────────────────────────────
// Writing (compiled unconditionally; only its EXECUTION is gated)
// ─────────────────────────────────────────────────────────────────────────────

/** Writes `result` to `<goldenDir>/<header.name>.txt`. Callers must treat a
    successful write as a TEST FAILURE (see `checkGolden`). */
inline juce::Result
writeGolden (const MidiRenderResult& result, const GoldenHeader& header, const juce::File& directory = {})
{
    if (! isValidGoldenName (header.name))
        return juce::Result::fail ("invalid golden name '" + header.name + "' (letters, digits, '-', '_', '.' only)");

    const auto dir = goldenDirectory (directory);
    if (! dir.isDirectory ())
    {
        const auto created = dir.createDirectory ();
        if (created.failed ())
            return created;
    }

    const auto path = goldenPath (header.name, directory);
    if (! path.replaceWithText (serializeGolden (result, header), false, false, "\n"))
        return juce::Result::fail ("could not write golden file: " + path.getFullPathName ());

    return juce::Result::ok ();
}

/** The briefed env-gated primitive: writes the golden iff `ARPBOX_REGENERATE_GOLDENS`
    is set, returning whether a file was written. It has NO success path that reports
    "all good" — the caller (always `checkGolden`) must fail when this returns true. */
inline bool
maybeRegenerate (const MidiRenderResult& result, const GoldenHeader& header, const juce::File& directory = {})
{
    if (! regenerationRequested ())
        return false;

    return writeGolden (result, header, directory).wasOk ();
}

// ─────────────────────────────────────────────────────────────────────────────
// Comparison
// ─────────────────────────────────────────────────────────────────────────────

/** Compares a live render against a loaded golden — on the PARSED EVENT VECTOR, so a
    header-comment edit is not a false failure while any change to the performance is.
    `bakedAtBlockSize` is never part of the verdict. */
inline GoldenComparison compareToGolden (const MidiRenderResult& result, const GoldenFile& golden)
{
    GoldenComparison comparison;

    if (! golden.ok)
    {
        comparison.matches = false;
        comparison.report << "GOLDEN COULD NOT BE READ\n"
                          << "  error: " << golden.error << "\n"
                          << "  line : " << juce::String (golden.errorLine) << "\n"
                          << "A missing or unparseable golden is a FAILURE, never a skip.\n";
        return comparison;
    }

    MidiRenderResult expected;
    expected.events = golden.events;
    expected.sampleRate = golden.header.sampleRate;
    expected.numSamples = golden.header.spanSamples;
    expected.blockSize = golden.header.bakedAtBlockSize; // diagnostics only — never compared
    expected.numBlocks = golden.header.bakedAtBlockSize > 0
                             ? static_cast<int> (golden.header.spanSamples / golden.header.bakedAtBlockSize)
                             : 0;

    comparison.matches = (result == expected); // MidiRenderResult::operator== is event-stream only

    comparison.report << "determinism golden '" << golden.header.name << "'"
                      << (comparison.matches ? " — MATCHES" : " — MISMATCH") << "\n"
                      << "  this  = live render\n"
                      << "  other = golden on disk\n"
                      << result.describeDifference (expected);

    if (! comparison.matches)
    {
        if (result.sampleRate != expected.sampleRate)
            comparison.report << "  note: sample rates differ (render " << formatGoldenNumber (result.sampleRate)
                              << " vs golden " << formatGoldenNumber (expected.sampleRate)
                              << ") — every absolute position moves with the rate.\n";

        if (result.numSamples != expected.numSamples)
            comparison.report << "  note: rendered span " << juce::String (result.numSamples)
                              << " samples vs golden spanSamples " << juce::String (expected.numSamples) << ".\n";

        comparison.report << "  note: bakedAtBlockSize (" << juce::String (golden.header.bakedAtBlockSize)
                          << ") is diagnostic only and was NOT compared.\n"
                          << "\n"
                          << goldenGovernanceRule << "\n";
    }

    return comparison;
}

// ─────────────────────────────────────────────────────────────────────────────
// The one call a golden test makes
// ─────────────────────────────────────────────────────────────────────────────

/** Regenerate ⁄ create ⁄ load ⁄ compare, in one call:

        const auto check = checkGolden (render, headerFor (render, "baseline-4bar", 125.0, 0.25));
        INFO (check.report);
        REQUIRE (check.passed);

    THE INTERLOCK. Reading top to bottom, every branch that writes a file assigns
    `passed = false` and then RETURNS — there is no fall-through to the comparison,
    and no `passed = true` anywhere below a write. So:
      • `ARPBOX_REGENERATE_GOLDENS=1` ⇒ file rewritten, run RED, diff printed.
      • golden missing ⇒ file created, run RED ("review and re-run").
      • golden unreadable ⇒ RED.
      • otherwise ⇒ verdict is the event-stream comparison, and nothing was written.
    tests/golden_format.cpp asserts `!(wroteFile && passed)` on every path. */
inline GoldenCheck
checkGolden (const MidiRenderResult& result, const GoldenHeader& header, const GoldenCheckOptions& options = {})
{
    GoldenCheck check;

    const bool regenerate = options.regenerate.value_or (regenerationRequested ());
    const auto path = goldenPath (header.name, options.directory);

    if (regenerate)
    {
        const auto previous = loadGolden (header.name, options.directory);
        const auto written = writeGolden (result, header, options.directory);

        check.passed = false; // UNCONDITIONAL: regeneration and green are exclusive.
        check.wroteFile = written.wasOk ();

        check.report << "GOLDEN REGENERATION REQUESTED (" << goldenRegenerateEnvVar << ")\n"
                     << "  file: " << path.getFullPathName () << "\n"
                     << (written.wasOk () ? juce::String ("  status: rewritten\n")
                                          : "  status: WRITE FAILED — " + written.getErrorMessage () + "\n");

        if (previous.ok)
            check.report << "\nWhat changed against the golden that was on disk:\n"
                         << compareToGolden (result, previous).report;
        else
            check.report << "\nNo readable previous golden to diff against (" << previous.error << ").\n";

        check.report << "\n" << goldenGovernanceRule << "\n";
        return check;
    }

    if (! path.existsAsFile ())
    {
        const auto written = writeGolden (result, header, options.directory);

        check.passed = false; // UNCONDITIONAL: a created golden has been reviewed by nobody.
        check.wroteFile = written.wasOk ();

        check.report << "GOLDEN CREATED — review and re-run\n"
                     << "  file: " << path.getFullPathName () << "\n"
                     << (written.wasOk () ? juce::String ("  status: written\n")
                                          : "  status: WRITE FAILED — " + written.getErrorMessage () + "\n")
                     << "  events: " << juce::String (static_cast<std::int64_t> (result.events.size ())) << "\n"
                     << "\nA golden that has never been read by a human proves nothing. Inspect\n"
                        "`git diff tests/golden/`, confirm the performance is what you intended,\n"
                        "then re-run to turn this green.\n";
        return check;
    }

    const auto golden = loadGolden (header.name, options.directory);
    const auto comparison = compareToGolden (result, golden);

    check.passed = comparison.matches;
    check.wroteFile = false;
    check.report = comparison.report;
    return check;
}
} // namespace arpbox::testing
