// ─────────────────────────────────────────────────────────────────────────────
// midi_input_node — MidiInputProcessor unit + MIDI-conformance tests
// (docs/INSTRUCTIONS.md Phase 4.2/4.4; ARCHITECTURE §3.3 the "[MIDI In Node]",
// §5.5 flush semantics). Drives the node DIRECTLY (no graph): construct →
// setSharedState → prepareToPlay → processBlock, reading the emitted MidiBuffer and
// the published MidiInputControl.
//
// Behaviors covered:
//   • Note-FIFO round-trip: QWERTY/pad NoteEvents surface as MIDI in FIFO order.
//   • Channel filter: the 16-bit mask drops filtered channels, passes allowed ones.
//   • Voice count: note-on/off track; an all-notes-off flush zeroes it; never negative.
//   • Zero-allocation steady-state processBlock (the documented collector lock is NOT
//     an allocation) — tagged [perf-budget] to stay out of the sanitizer `-L unit` run.
// ─────────────────────────────────────────────────────────────────────────────

#include "support/AllocationSentinel.h"

#include "engine/graph/MidiInputControl.h"
#include "engine/graph/MidiInputProcessor.h"
#include "engine/graph/NoteEvent.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using arpbox::engine::MidiInputControl;
using arpbox::engine::MidiInputProcessor;
using arpbox::engine::NoteEvent;
using arpbox::engine::NoteEventKind;
using arpbox::engine::NoteEventQueue;
using arpbox::test::AllocationSentinel;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 128;

// A wired, prepared node plus the shared state it reads. Bundled so each test owns
// an independent, self-consistent set (the node holds non-owning pointers into it).
struct NodeRig
{
    NoteEventQueue notes;
    juce::MidiMessageCollector collector;
    MidiInputControl control;
    MidiInputProcessor node;

    NodeRig ()
    {
        node.setSharedState (&notes, &collector, &control);
        node.prepareToPlay (kSampleRate, kBlockSize); // resets the collector to SR
    }

    // Runs one block and returns the MIDI the node emitted. The audio buffer is a
    // throwaway (MIDI-only node) sized so getNumSamples() == kBlockSize.
    juce::MidiBuffer process ()
    {
        juce::AudioBuffer<float> audio (1, kBlockSize);
        juce::MidiBuffer midi;
        node.processBlock (audio, midi);
        return midi;
    }

    void pushOn (int note, int velocity, int channel)
    {
        notes.push (NoteEvent { NoteEventKind::noteOn,
                                static_cast<std::uint8_t> (note),
                                static_cast<std::uint8_t> (velocity),
                                static_cast<std::uint8_t> (channel) });
    }

    void pushOff (int note, int channel)
    {
        notes.push (NoteEvent { NoteEventKind::noteOff,
                                static_cast<std::uint8_t> (note),
                                0,
                                static_cast<std::uint8_t> (channel) });
    }
};

// Flattens a MidiBuffer to an ordered vector of messages for positional assertions.
std::vector<juce::MidiMessage> messagesOf (const juce::MidiBuffer& midi)
{
    std::vector<juce::MidiMessage> out;
    for (const auto meta : midi)
        out.push_back (meta.getMessage ());
    return out;
}
} // namespace

//==============================================================================
TEST_CASE ("midi-in/note-fifo: QWERTY note events surface as MIDI in FIFO order", "[midi-conformance]")
{
    NodeRig rig;

    rig.pushOn (60, 100, 1);
    rig.pushOff (60, 1);

    const auto messages = messagesOf (rig.process ());

    REQUIRE (messages.size () == 2);

    REQUIRE (messages[0].isNoteOn ());
    REQUIRE (messages[0].getNoteNumber () == 60);
    REQUIRE (messages[0].getVelocity () == 100);
    REQUIRE (messages[0].getChannel () == 1);

    REQUIRE (messages[1].isNoteOff ());
    REQUIRE (messages[1].getNoteNumber () == 60);
    REQUIRE (messages[1].getChannel () == 1);
}

TEST_CASE ("midi-in/channel-filter: mask drops filtered channels, passes allowed ones", "[midi-conformance]")
{
    NodeRig rig;

    // Allow ONLY channel 1 (bit 0). Channel 2 (bit 1) is filtered out.
    rig.control.channelMask.store (0x0001, std::memory_order_relaxed);

    rig.pushOn (60, 100, 1); // passes
    rig.pushOn (64, 100, 2); // dropped
    rig.pushOn (67, 100, 1); // passes

    const auto messages = messagesOf (rig.process ());

    REQUIRE (messages.size () == 2);
    for (const auto& m : messages)
        REQUIRE (m.getChannel () == 1);

    // The dropped channel-2 note never incremented the voice count either.
    REQUIRE (rig.control.voiceCount.load () == 2);
}

TEST_CASE ("midi-in/voice-count: tracks note-on/off, flushes clean, never goes negative", "[unit]")
{
    NodeRig rig;

    SECTION ("note-on increments, note-off decrements")
    {
        rig.pushOn (60, 100, 1);
        rig.pushOn (64, 100, 1);
        rig.process ();
        REQUIRE (rig.control.voiceCount.load () == 2);

        rig.pushOff (60, 1);
        rig.process ();
        REQUIRE (rig.control.voiceCount.load () == 1);
    }

    SECTION ("surplus note-offs never drive the count below zero")
    {
        rig.pushOn (60, 100, 1);
        rig.process ();
        REQUIRE (rig.control.voiceCount.load () == 1);

        // Three offs against one live voice: the count clamps at 0, not negative.
        rig.pushOff (60, 1);
        rig.pushOff (61, 1);
        rig.pushOff (62, 1);
        rig.process ();
        REQUIRE (rig.control.voiceCount.load () == 0);
    }

    SECTION ("all-notes-off flush emits CC123 on every channel and zeroes the count")
    {
        rig.pushOn (60, 100, 1);
        rig.pushOn (64, 100, 1);
        rig.pushOn (67, 100, 1);
        rig.process ();
        REQUIRE (rig.control.voiceCount.load () == 3);

        // Request the flush; next block must clear everything.
        rig.control.allNotesOffRequested.store (true, std::memory_order_release);
        const auto messages = messagesOf (rig.process ());

        REQUIRE (rig.control.voiceCount.load () == 0);

        int cc123 = 0;
        for (const auto& m : messages)
            if (m.isControllerOfType (123))
                ++cc123;
        REQUIRE (cc123 == 16); // one CC123 per MIDI channel
    }
}

//==============================================================================
TEST_CASE ("midi-in/channel-filter: a masked-off note-OFF still passes (no hanging note)", "[midi-conformance]")
{
    // §5.5 invariant / review finding W1: the channel mask gates ONLY note-ons. A
    // note let through under a permissive mask must still receive its note-off after
    // the mask changes, or it hangs on the synth and the voice count drifts forever.
    NodeRig rig;

    // Note-on under a permissive mask (all channels allowed).
    rig.control.channelMask.store (0xFFFF, std::memory_order_relaxed);
    rig.pushOn (60, 100, 1);
    {
        const auto messages = messagesOf (rig.process ());
        REQUIRE (messages.size () == 1);
        REQUIRE (messages[0].isNoteOn ());
        REQUIRE (rig.control.voiceCount.load () == 1);
    }

    // Mask OUT every channel, then release the held note.
    rig.control.channelMask.store (0x0000, std::memory_order_relaxed);
    rig.pushOff (60, 1);
    {
        const auto messages = messagesOf (rig.process ());

        // The note-off is emitted despite channel 1 being masked, and the voice
        // count returns to zero — no stuck note, no drift.
        REQUIRE (messages.size () == 1);
        REQUIRE (messages[0].isNoteOff ());
        REQUIRE (messages[0].getNoteNumber () == 60);
        REQUIRE (messages[0].getChannel () == 1);
        REQUIRE (rig.control.voiceCount.load () == 0);
    }

    // A FRESH note-on on the masked channel is still correctly dropped (the mask
    // gates note-ons — it just never swallows a release).
    rig.pushOn (67, 100, 1);
    REQUIRE (messagesOf (rig.process ()).empty ());
    REQUIRE (rig.control.voiceCount.load () == 0);
}

//==============================================================================
// [perf-budget] — excluded from the sanitizer `-L unit` runs.
TEST_CASE ("midi-in/alloc-guard: the all-notes-off flush does not allocate after warm-up", "[perf-budget]")
{
    // Review finding W2: the 16-event CC123 flush must not grow the outgoing
    // MidiBuffer on the audio thread. The node capacity-warms the buffer on its
    // first block, so a later flush block is allocation-free. We deliberately do NOT
    // pre-ensureSize the buffer here — the node's OWN warm-up must eliminate it.
    NodeRig rig;

    juce::AudioBuffer<float> audio (1, kBlockSize);
    juce::MidiBuffer midi; // fresh, zero-capacity — reused across blocks so the
                           // node's first-block warm-up persists (as in the graph).

    // Warm-up: the first block grows the buffer (the sanctioned one-time alloc);
    // a few held notes give it representative traffic.
    for (int i = 0; i < 4; ++i)
    {
        rig.pushOn (60 + i, 100, 1);
        midi.clear ();
        rig.node.processBlock (audio, midi);
    }
    REQUIRE (rig.control.voiceCount.load () == 4);

    // Measured flush block: request the flush, clear (capacity retained), then
    // process under the sentinel. The 16 CC123 events must fit reserved capacity.
    rig.control.allNotesOffRequested.store (true, std::memory_order_release);
    midi.clear ();

    std::uint64_t allocations = 0;
    {
        AllocationSentinel sentinel;
        rig.node.processBlock (audio, midi);
        allocations = sentinel.allocations ();
    }

    INFO ("allocations during the CC123 all-notes-off flush block");
    REQUIRE (allocations == 0);

    // The flush actually happened (16 CC123 emitted, voice count zeroed).
    int cc123 = 0;
    for (const auto meta : midi)
        if (meta.getMessage ().isControllerOfType (123))
            ++cc123;
    REQUIRE (cc123 == 16);
    REQUIRE (rig.control.voiceCount.load () == 0);
}

//==============================================================================
// [perf-budget] — excluded from the sanitizer `-L unit` runs.
TEST_CASE ("midi-in/alloc-guard: zero allocations in steady-state processBlock", "[perf-budget]")
{
    NodeRig rig;

    // Pre-allocate the audio + MIDI buffers the measured loop reuses; pre-size the
    // output MidiBuffer so addEvent stays within capacity (never grows on the path).
    juce::AudioBuffer<float> audio (1, kBlockSize);
    juce::MidiBuffer midi;
    midi.ensureSize (4096);

    // Warmup: absorb any one-time lazy allocation with the full drain path active.
    constexpr int warmupBlocks = 64;
    for (int i = 0; i < warmupBlocks; ++i)
    {
        rig.pushOn (60, 100, 1);
        rig.pushOff (60, 1);
        midi.clear ();
        rig.node.processBlock (audio, midi);
    }

    constexpr int measuredBlocks = 256;
    std::uint64_t allocations = 0;
    {
        AllocationSentinel sentinel;
        for (int i = 0; i < measuredBlocks; ++i)
        {
            rig.notes.push (NoteEvent { NoteEventKind::noteOn, 60, 100, 1 });
            rig.notes.push (NoteEvent { NoteEventKind::noteOff, 60, 0, 1 });
            midi.clear ();
            rig.node.processBlock (audio, midi);
        }
        allocations = sentinel.allocations ();
    }

    INFO ("steady-state MIDI-in allocations across " << measuredBlocks << " processBlock calls");
    REQUIRE (allocations == 0);
}
