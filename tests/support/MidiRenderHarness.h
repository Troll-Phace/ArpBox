#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// MidiRenderHarness — "render N blocks headless and collect every emitted MIDI
// event at its ABSOLUTE sample position" (docs/INSTRUCTIONS.md Phase 5.3;
// ARCHITECTURE §1.2 the determinism contract, §5.5 event ordering).
//
// WHY THIS EXISTS. Two idioms already in the suite each lose the one thing the
// determinism contract is about:
//   • fakes/HostedSynthGraphSupport.h `renderBlocks` throws the MidiBuffer away and
//     returns a meter value — a signal PROXY, not the signal.
//   • midi_input_node.cpp's local `messagesOf` keeps the messages but drops
//     `samplePosition`, so it cannot tell 128-sample blocks from 512-sample ones.
// Neither can express "the same absolute sample carries the same event at every
// buffer size", which is Phase 5.3's acceptance criterion and the foundation of
// every Phase-6+ golden MIDI file. Absolute position — `blockBase +
// meta.samplePosition` — is the whole point of this header.
//
// DESIGNED FOR THE PHASE-6 GOLDEN SUITE, not just for this phase:
//   • `TimedMidiEvent` compares BYTE-FOR-BYTE (position + raw MIDI bytes), so two
//     renders are equal iff they are the same performance.
//   • `MidiRenderResult::toByteStream()` is a canonical, lossless, self-describing
//     serialization — the byte-identical comparison target for `tests/golden/`
//     (a `juce::MidiFile` writer can be layered on top when 6.4 needs `.mid` on
//     disk; absolute sample positions survive either way).
//   • `firstDifference()` / `describeDifference()` exist because a bare
//     `REQUIRE (a == b)` on 200-event vectors produces an undiagnosable wall of
//     text. Always `INFO (a.describeDifference (b))` before the REQUIRE.
//
// TWO RENDER IDIOMS, ONE FUNCTION. `renderProcessor` takes a `juce::AudioProcessor&`,
// which covers both shapes the suite already uses:
//   • a SINGLE node driven directly:  renderProcessor (sequencerNode, config)
//   • a WHOLE EngineGraph:            renderProcessor (graph.getProcessor(), config)
// The graph case collects what the graph leaves in the CALLER's MidiBuffer (its
// MIDI-out path). To tap MIDI generated INSIDE the graph — e.g. what the sequencer
// node hands the synth — insert a `MidiCaptureNode` in the synth slot instead; it
// does the same absolute-sample accounting from within the render sequence.
//
// ALLOCATION (this is a test helper, so allocation is allowed — but be precise
// about WHERE, because callers wrap `AllocationSentinel` around render loops):
//   • `renderProcessor` allocates its AudioBuffer/MidiBuffer and RESERVES the event
//     vector BEFORE the first block. Inside the loop it allocates only if the event
//     count exceeds `MidiRenderConfig::eventReserve` (raise it) or a message is
//     longer than 3 bytes (juce::MidiMessage stores ≤3 bytes inline; sysex heaps).
//     It is therefore safe to sentinel a render of note data with a sized reserve.
//   • `MidiCaptureNode::processBlock` runs on the AUDIO path inside a real graph, so
//     it NEVER grows: it records up to its reserved capacity and counts
//     `droppedEvents()` beyond it. Assert that count is 0.
//
// Header-only, `namespace arpbox::testing`, matching fakes/HostedSynthGraphSupport.h.
// ─────────────────────────────────────────────────────────────────────────────

#include <juce_audio_processors/juce_audio_processors.h>

#include <catch2/catch_tostring.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <vector>

namespace arpbox::testing
{
// ─────────────────────────────────────────────────────────────────────────────
// One event, at an absolute position on the render timeline
// ─────────────────────────────────────────────────────────────────────────────

/** A MIDI message tagged with its ABSOLUTE sample position on the render timeline
    (sample 0 = the first sample of the first rendered block), not the within-block
    offset a `MidiBuffer` carries.

    Equality is BYTE-FOR-BYTE — position plus raw MIDI bytes — which is exactly the
    determinism-contract predicate (§1.2). Ordering is (position, then bytes), a
    strict weak order, so a render can be sorted into a canonical form when a test
    deliberately does not care about within-sample ordering. */
struct TimedMidiEvent
{
    std::int64_t absoluteSample = 0; ///< Samples since the first rendered sample.
    juce::MidiMessage message;       ///< The message as emitted (raw bytes preserved).

    /** Raw byte count of the message. */
    int numBytes () const noexcept { return message.getRawDataSize (); }

    /** Pointer to the message's raw bytes. */
    const std::uint8_t* bytes () const noexcept { return message.getRawData (); }

    /** True if both messages carry identical raw bytes (ignoring position). */
    bool sameBytes (const TimedMidiEvent& other) const noexcept
    {
        return numBytes () == other.numBytes ()
            && std::memcmp (bytes (), other.bytes (), static_cast<std::size_t> (numBytes ())) == 0;
    }

    /** Byte-for-byte equality including the absolute position. */
    bool operator== (const TimedMidiEvent& other) const noexcept
    {
        return absoluteSample == other.absoluteSample && sameBytes (other);
    }

    bool operator!= (const TimedMidiEvent& other) const noexcept { return ! (*this == other); }

    /** (position, then raw bytes) ordering — a strict weak order for canonical sorts. */
    bool operator< (const TimedMidiEvent& other) const noexcept
    {
        if (absoluteSample != other.absoluteSample)
            return absoluteSample < other.absoluteSample;

        const int shared = std::min (numBytes (), other.numBytes ());
        if (shared > 0)
        {
            const int cmp = std::memcmp (bytes (), other.bytes (), static_cast<std::size_t> (shared));
            if (cmp != 0)
                return cmp < 0;
        }
        return numBytes () < other.numBytes ();
    }

    /** Compact, human-diagnosable one-liner: `@2048 90 3C 64 (Note on C3 ...)`. The
        hex bytes come first because they are what equality actually compares. */
    juce::String describe () const
    {
        juce::String text;
        text << "@" << juce::String (absoluteSample);

        for (int i = 0; i < numBytes (); ++i)
            text << " " << juce::String::toHexString (static_cast<int> (bytes ()[i])).paddedLeft ('0', 2).toUpperCase ();

        text << "  (" << message.getDescription () << ")";
        return text;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Render configuration
// ─────────────────────────────────────────────────────────────────────────────

/** How many blocks to render, at what rate/size, and how much to pre-reserve.

    Use the named factories rather than filling fields by hand — `bars()` in
    particular encodes the samples-per-bar arithmetic once so N-bar renders at
    different buffer sizes agree on the sample count they cover. */
struct MidiRenderConfig
{
    double sampleRate = 48000.0; ///< Render sample rate (Hz).
    int blockSize = 128;         ///< Samples per block; every block is this length.
    int numBlocks = 0;           ///< Blocks to render.
    int numChannels = 2;         ///< Audio channels in the scratch buffer (≥1).

    /** Events reserved up front. Exceeding it reallocates mid-render (see the
        allocation note at the top of this header). */
    std::size_t eventReserve = 4096;

    /** Bytes reserved in the working `MidiBuffer` (`ensureSize`). */
    int midiReserveBytes = 8192;

    /** Absolute samples this configuration covers. */
    std::int64_t totalSamples () const noexcept
    {
        return static_cast<std::int64_t> (numBlocks) * static_cast<std::int64_t> (blockSize);
    }

    /** Whole blocks of `blockSize` needed to cover at least `samples` (rounds UP: a
        render always ends on a block boundary, so it may cover slightly more musical
        time than asked for. Compare renders by ABSOLUTE SAMPLE, never by index). */
    static int numBlocksForSamples (std::int64_t samples, int blockSize) noexcept
    {
        if (blockSize <= 0 || samples <= 0)
            return 0;

        return static_cast<int> ((samples + blockSize - 1) / blockSize);
    }

    /** Samples spanned by `numBars` bars at `bpm` (4/4 unless told otherwise) — the
        Phase-6 "render N bars" arithmetic, in one place. */
    static std::int64_t samplesForBars (double numBars,
                                        double bpm,
                                        double sampleRate,
                                        double quartersPerBar = 4.0) noexcept
    {
        const double quarters = numBars * quartersPerBar;
        const double seconds = quarters * (60.0 / bpm);
        return static_cast<std::int64_t> (std::llround (seconds * sampleRate));
    }

    /** `n` blocks at the given rate/size. */
    static MidiRenderConfig blocks (int n, double sampleRate = 48000.0, int blockSize = 128)
    {
        MidiRenderConfig config;
        config.sampleRate = sampleRate;
        config.blockSize = blockSize;
        config.numBlocks = std::max (0, n);
        return config;
    }

    /** Enough whole blocks to cover `samples`. */
    static MidiRenderConfig samples (std::int64_t n, double sampleRate = 48000.0, int blockSize = 128)
    {
        return blocks (numBlocksForSamples (n, blockSize), sampleRate, blockSize);
    }

    /** Enough whole blocks to cover `numBars` bars at `bpm`. */
    static MidiRenderConfig bars (double numBars,
                                  double bpm,
                                  double sampleRate = 48000.0,
                                  int blockSize = 128,
                                  double quartersPerBar = 4.0)
    {
        return samples (samplesForBars (numBars, bpm, sampleRate, quartersPerBar), sampleRate, blockSize);
    }
};

/** What a per-block hook is handed, BEFORE the block is processed. The buffers are
    already cleared: push commands, and/or inject INPUT MIDI into `midi` (whatever
    the processor leaves in it afterwards is what gets collected). */
struct RenderBlockContext
{
    int blockIndex = 0;          ///< 0-based block counter.
    std::int64_t blockBase = 0;  ///< Absolute sample of this block's FIRST sample.
    int numSamples = 0;          ///< This block's length.
    juce::AudioBuffer<float>* audio = nullptr; ///< Cleared scratch audio (never null).
    juce::MidiBuffer* midi = nullptr;          ///< Cleared MIDI buffer (never null).
};

/** Called before each block. Empty by default. */
using RenderBlockHook = std::function<void (const RenderBlockContext&)>;

// ─────────────────────────────────────────────────────────────────────────────
// Render result
// ─────────────────────────────────────────────────────────────────────────────

/** Everything one headless render emitted, plus the comparison/diagnostic surface
    the golden suite needs.

    `operator==` compares the EVENT STREAM ONLY (byte-for-byte) and deliberately
    ignores the configuration metadata — that is what makes a 32-sample render
    directly comparable with a 2048-sample one, which is the buffer-size
    independence property. */
struct MidiRenderResult
{
    std::vector<TimedMidiEvent> events; ///< In emission order (block order, then within-block order).
    double sampleRate = 0.0;            ///< Rate this render ran at.
    int blockSize = 0;                  ///< Block size this render ran at.
    int numBlocks = 0;                  ///< Blocks actually rendered.
    std::int64_t numSamples = 0;        ///< Absolute samples covered.

    std::size_t size () const noexcept { return events.size (); }
    bool empty () const noexcept { return events.empty (); }
    const TimedMidiEvent& operator[] (std::size_t index) const { return events[index]; }
    auto begin () const noexcept { return events.begin (); }
    auto end () const noexcept { return events.end (); }

    /** Byte-identical event streams (configuration metadata ignored). */
    bool operator== (const MidiRenderResult& other) const noexcept
    {
        return events.size () == other.events.size ()
            && std::equal (events.begin (), events.end (), other.events.begin ());
    }

    bool operator!= (const MidiRenderResult& other) const noexcept { return ! (*this == other); }

    /** Index of the first differing event, or (when the streams share a prefix but
        differ in length) the length of the shorter one. `nullopt` ⇒ identical. */
    std::optional<std::size_t> firstDifference (const MidiRenderResult& other) const
    {
        const std::size_t shared = std::min (events.size (), other.events.size ());
        for (std::size_t i = 0; i < shared; ++i)
            if (events[i] != other.events[i])
                return i;

        if (events.size () != other.events.size ())
            return shared;

        return std::nullopt;
    }

    /** The message to hand `INFO(...)` before a `REQUIRE (a == b)`: stream sizes, the
        index of the first divergence, and `context` events either side of it from
        BOTH renders. Bounded output — never dumps the whole stream. */
    juce::String describeDifference (const MidiRenderResult& other, int context = 3) const
    {
        juce::String text;
        text << "MIDI render comparison:\n"
             << "  this : " << summary () << "\n"
             << "  other: " << other.summary () << "\n";

        const auto diff = firstDifference (other);
        if (! diff.has_value ())
        {
            text << "  streams are BYTE-IDENTICAL\n";
            return text;
        }

        const auto index = *diff;
        text << "  first difference at index " << juce::String (static_cast<std::int64_t> (index)) << ":\n";

        const auto low = index > static_cast<std::size_t> (context)
                             ? index - static_cast<std::size_t> (context)
                             : static_cast<std::size_t> (0);
        const auto high = index + static_cast<std::size_t> (context) + 1;

        for (std::size_t i = low; i < high; ++i)
        {
            const bool marked = (i == index);
            text << (marked ? "  >> [" : "     [") << juce::String (static_cast<std::int64_t> (i)) << "]\n";
            text << "        this : " << (i < events.size () ? events[i].describe () : juce::String ("<end of stream>")) << "\n";
            text << "        other: " << (i < other.events.size () ? other.events[i].describe () : juce::String ("<end of stream>")) << "\n";
        }
        return text;
    }

    /** One-line shape summary (no events). */
    juce::String summary () const
    {
        juce::String text;
        text << juce::String (static_cast<std::int64_t> (events.size ())) << " events over "
             << juce::String (numBlocks) << " blocks (" << juce::String (blockSize) << " samples @ "
             << juce::String (sampleRate, 1) << " Hz, " << juce::String (numSamples) << " samples total)";
        return text;
    }

    /** Summary plus the first `maxEvents` events — for `INFO` on a single-render
        assertion. Bounded by construction. */
    juce::String describe (std::size_t maxEvents = 24) const
    {
        juce::String text;
        text << summary () << "\n";

        const auto shown = std::min (maxEvents, events.size ());
        for (std::size_t i = 0; i < shown; ++i)
            text << "  [" << juce::String (static_cast<std::int64_t> (i)) << "] " << events[i].describe () << "\n";

        if (shown < events.size ())
            text << "  ... " << juce::String (static_cast<std::int64_t> (events.size () - shown)) << " more\n";

        return text;
    }

    /** True if absolute positions are non-decreasing — ARCHITECTURE §5.5 "within a
        block, events are strictly sample-sorted", checked across the whole render. */
    bool isSampleSorted () const
    {
        return std::is_sorted (events.begin (),
                               events.end (),
                               [] (const TimedMidiEvent& a, const TimedMidiEvent& b)
                               { return a.absoluteSample < b.absoluteSample; });
    }

    /** Events matching `predicate` (e.g. `[] (auto& e) { return e.message.isNoteOn (); }`). */
    template <typename Predicate>
    std::vector<TimedMidiEvent> select (Predicate predicate) const
    {
        std::vector<TimedMidiEvent> picked;
        for (const auto& event : events)
            if (predicate (event))
                picked.push_back (event);
        return picked;
    }

    /** A canonical, lossless serialization of the event stream — the golden-file
        comparison target. Per event, little-endian: int64 absolute sample, uint16
        byte count, then the raw MIDI bytes. Self-describing and position-exact, so
        two byte streams are equal iff the two performances are identical. Contains
        NO configuration metadata, by design: a golden must not encode the buffer
        size it happened to be rendered at. */
    std::vector<std::uint8_t> toByteStream () const
    {
        std::vector<std::uint8_t> bytes;
        std::size_t total = 0;
        for (const auto& event : events)
            total += 10u + static_cast<std::size_t> (std::max (0, event.numBytes ()));
        bytes.reserve (total);

        for (const auto& event : events)
        {
            const auto sample = static_cast<std::uint64_t> (event.absoluteSample);
            for (int shift = 0; shift < 64; shift += 8)
                bytes.push_back (static_cast<std::uint8_t> ((sample >> shift) & 0xffu));

            const auto count = static_cast<std::uint16_t> (std::max (0, event.numBytes ()));
            bytes.push_back (static_cast<std::uint8_t> (count & 0xffu));
            bytes.push_back (static_cast<std::uint8_t> ((count >> 8) & 0xffu));

            for (int i = 0; i < event.numBytes (); ++i)
                bytes.push_back (event.bytes ()[i]);
        }
        return bytes;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// The render loop
// ─────────────────────────────────────────────────────────────────────────────

/** Renders `config.numBlocks` blocks through `processor`, collecting every emitted
    MIDI event at its ABSOLUTE sample position (`blockBase + meta.samplePosition`).

    Works for BOTH suite idioms — a single node (`renderProcessor (node, …)`) and a
    whole engine graph (`renderProcessor (graph.getProcessor(), …)`). The caller is
    responsible for having prepared the processor at `config.sampleRate` /
    `config.blockSize`; the harness deliberately does not call `prepareToPlay`,
    because graph preparation is the graph owner's job and re-preparing mid-suite
    would mask ordering bugs.

    `beforeBlock` runs before each block with the cleared buffers, for pushing
    commands and/or injecting input MIDI. MESSAGE-THREAD/test-thread only — there is
    no audio device here, everything runs on the calling thread. */
inline MidiRenderResult renderProcessor (juce::AudioProcessor& processor,
                                         const MidiRenderConfig& config,
                                         const RenderBlockHook& beforeBlock = {})
{
    MidiRenderResult result;
    result.sampleRate = config.sampleRate;
    result.blockSize = config.blockSize;

    const int blockSize = std::max (1, config.blockSize);
    const int channels = std::max (1, config.numChannels);

    // Everything the loop touches is allocated HERE, before the first block.
    juce::AudioBuffer<float> audio (channels, blockSize);
    juce::MidiBuffer midi;
    midi.ensureSize (std::max (0, config.midiReserveBytes));
    result.events.reserve (config.eventReserve);

    std::int64_t base = 0;
    for (int block = 0; block < config.numBlocks; ++block)
    {
        audio.clear ();
        midi.clear ();

        if (beforeBlock)
            beforeBlock (RenderBlockContext { block, base, blockSize, &audio, &midi });

        processor.processBlock (audio, midi);

        // THE arithmetic this harness exists for: within-block offset → absolute.
        for (const auto meta : midi)
            result.events.push_back (TimedMidiEvent { base + static_cast<std::int64_t> (meta.samplePosition),
                                                      meta.getMessage () });

        base += blockSize;
        ++result.numBlocks;
    }

    result.numSamples = base;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Graph-internal tap
// ─────────────────────────────────────────────────────────────────────────────

/** An instrument-shaped node (MIDI in, stereo silent out) that records the MIDI it
    receives, at ABSOLUTE sample positions, from INSIDE a real `AudioProcessorGraph`.

    This is how you capture MIDI the graph generates internally — e.g. install it in
    the synth slot (`graph.setSynth (…)`) and it records exactly what the sequencer
    node handed the synth, sample-accurately, which the caller's `MidiBuffer` never
    sees. Emits silence, so a metered render stays quiet.

    RT DISCIPLINE: `processBlock` never grows its storage. It records up to the
    capacity reserved at construction and counts anything beyond as
    `droppedEvents()` — assert that is 0 rather than trusting a short stream. */
class MidiCaptureNode final : public juce::AudioProcessor
{
public:
    /** Reserves room for `reserveEvents` events up front (message thread). */
    explicit MidiCaptureNode (std::size_t reserveEvents = 8192)
        : juce::AudioProcessor (BusesProperties ().withOutput ("Output",
                                                              juce::AudioChannelSet::stereo (),
                                                              true))
    {
        captured.events.reserve (reserveEvents);
    }

    /** Everything captured so far, with the same comparison/diagnostic surface a
        `renderProcessor` result has. */
    const MidiRenderResult& result () const noexcept { return captured; }

    /** Events that did not fit the reserved capacity. MUST be 0 for a trustworthy
        stream. */
    std::int64_t droppedEvents () const noexcept { return dropped; }

    /** Blocks this node has rendered. */
    int blocksRendered () const noexcept { return captured.numBlocks; }

    /** Clears the capture and rewinds the absolute-sample origin to 0 (keeps the
        reserved capacity). Call after warmup blocks so index 0 is the first block you
        actually care about. MESSAGE-THREAD ONLY — call it with rendering stopped. */
    void resetCapture () noexcept
    {
        captured.events.clear ();
        captured.numBlocks = 0;
        captured.numSamples = 0;
        blockBase = 0;
        dropped = 0;
    }

    // ── AudioProcessor ────────────────────────────────────────────────────────
    const juce::String getName () const override { return "MidiCaptureNode"; }

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override
    {
        captured.sampleRate = sampleRate;
        captured.blockSize = maximumExpectedSamplesPerBlock;
    }

    void releaseResources () override {}

    // RT-SAFE: records into pre-reserved storage; never grows (see droppedEvents()).
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::ScopedNoDenormals noDenormals;
        buffer.clear ();

        for (const auto meta : midiMessages)
        {
            if (captured.events.size () < captured.events.capacity ())
                captured.events.push_back (TimedMidiEvent { blockBase + static_cast<std::int64_t> (meta.samplePosition),
                                                            meta.getMessage () });
            else
                ++dropped;
        }

        blockBase += static_cast<std::int64_t> (buffer.getNumSamples ());
        ++captured.numBlocks;
        captured.numSamples = blockBase;
    }

    void processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) override { buffer.clear (); }

    juce::AudioProcessorEditor* createEditor () override { return nullptr; }
    bool hasEditor () const override { return false; }
    bool acceptsMidi () const override { return true; }
    bool producesMidi () const override { return false; }
    double getTailLengthSeconds () const override { return 0.0; }
    int getNumPrograms () override { return 1; }
    int getCurrentProgram () override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    MidiRenderResult captured;
    std::int64_t blockBase = 0;
    std::int64_t dropped = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiCaptureNode)
};
} // namespace arpbox::testing

// ─────────────────────────────────────────────────────────────────────────────
// Catch2 stringification — bounded on purpose. A failed vector comparison must not
// dump 200 events; for a real diff use `INFO (a.describeDifference (b))`.
// ─────────────────────────────────────────────────────────────────────────────
namespace Catch
{
template <>
struct StringMaker<arpbox::testing::TimedMidiEvent>
{
    static std::string convert (const arpbox::testing::TimedMidiEvent& event)
    {
        return event.describe ().toStdString ();
    }
};

template <>
struct StringMaker<arpbox::testing::MidiRenderResult>
{
    static std::string convert (const arpbox::testing::MidiRenderResult& result)
    {
        return result.describe (8).toStdString ();
    }
};
} // namespace Catch
