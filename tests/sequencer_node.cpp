// ─────────────────────────────────────────────────────────────────────────────
// sequencer_node — Phase 5.3e: the sequencer / stop-flush test matrix
// (ARCHITECTURE §5.5 "MIDI correctness invariants", §4 step 4; INSTRUCTIONS Phase
// 5.2/5.3 and the Phase 5 success criterion "Stop flushes all notes (table empty
// assertion)").
//
// FOUR LAYERS, deliberately separated:
//   1. The SCAFFOLD PATTERN as a performance — pitches, velocity, channel, gate
//      length, on/off pairing, and "a stopped transport emits nothing". Phase 6
//      deletes the scaffold, and these assertions with it; the note-lifecycle
//      assertions below survive.
//   2. FLUSH POINTS (§5.5) — transport stop and locate, asserted both against the
//      bare node and through the assembled `EngineGraph` (which is what
//      `EngineGraph::getSequencer()` exists for). The invariant is not "some
//      note-offs were emitted" but "a note-off for EVERY sounding note, and the
//      sounding-note table is EMPTY afterwards".
//   3. NOTE LIFECYCLE over churn — scripted, then a seeded mini-fuzzer over
//      play/stop/locate/tempo. Both assert through `NoteLifecycleTracker`
//      (support/NoteLifecycleCheck.h), the reusable balance checker that Phase 8.3's
//      hanging-note fuzzer inherits.
//   4. `SoundingNoteTable` in isolation — capacity/overflow asymmetry, `find`, the
//      half-open due window, the same-pitch retrigger primitives, and the
//      channel-restricted CC123 sweep.
//
// Buffer-size independence lives in transport_timing.cpp (5.3c) and is not repeated
// here; transport-level PPQ/tempo/stop semantics live in transport_clock.cpp (5.1).
// ─────────────────────────────────────────────────────────────────────────────

#include "support/AllocationSentinel.h"
#include "support/MidiRenderHarness.h"
#include "support/NoteLifecycleCheck.h"
#include "support/SequencerRenderRig.h"

#include "engine/graph/EngineCommand.h"
#include "engine/graph/EngineGraph.h"
#include "engine/midi/SoundingNoteTable.h"
#include "engine/sequencer/SequencerProcessor.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <juce_events/juce_events.h>

#include <cstdint>
#include <memory>
#include <random>
#include <vector>

using arpbox::engine::EngineCommand;
using arpbox::engine::EngineCommandType;
using arpbox::engine::EngineGraph;
using arpbox::engine::SequencerProcessor;
using arpbox::engine::SoundingNoteTable;
using arpbox::test::AllocationSentinel;
using arpbox::testing::engineCommand;
using arpbox::testing::MidiCaptureNode;
using arpbox::testing::MidiRenderConfig;
using arpbox::testing::MidiRenderResult;
using arpbox::testing::NoteLifecycleTracker;
using arpbox::testing::renderSequencer;
using arpbox::testing::samplesPerScaffoldStep;
using arpbox::testing::scaffoldGateSamples;
using arpbox::testing::ScheduledCommand;
using arpbox::testing::SequencerRig;
using arpbox::testing::TimedMidiEvent;

namespace
{
// 120 BPM @ 48 kHz / 128-sample blocks: a 16th note is exactly 6000 samples and the
// 50% gate exactly 3000, so every position below is an exact integer and can be
// asserted literally rather than approximately.
constexpr double testSampleRate = 48000.0;
constexpr int testBlockSize = 128;
constexpr std::int64_t stepSamples = 6000;
constexpr std::int64_t gateSamples = 3000;

/** The scaffold's one-octave-ascending pitch sequence (C major from middle C), as the
    UI/ear would describe it — written out independently of the node's own table so a
    change to that table has to be justified here too. */
constexpr int scaffoldPitches[SequencerProcessor::scaffoldNumSteps] = { 60, 62, 64, 65, 67, 69, 71, 72,
                                                                        60, 62, 64, 65, 67, 69, 71, 72 };

/** Pumps the message loop in bounded slices so a queued async graph edit is applied
    before the caller renders. The budget is a HANG GUARD, not a sync primitive (same
    helper and rationale as transport_clock.cpp / HostedSynthGraphSupport.h). */
void settleGraphEdits (int slices = 20, int msPerSlice = 5)
{
    for (int i = 0; i < slices; ++i)
        juce::MessageManager::getInstance ()->runDispatchLoopUntil (msPerSlice);
}

/** A render config for the bare-node tests: `blocks` blocks of 128 at 48 kHz. */
MidiRenderConfig nodeBlocks (int blocks)
{
    auto config = MidiRenderConfig::blocks (blocks, testSampleRate, testBlockSize);
    config.numChannels = 1;
    return config;
}

std::vector<TimedMidiEvent> noteOnsOf (const MidiRenderResult& render)
{
    return render.select ([] (const TimedMidiEvent& event) { return event.message.isNoteOn (); });
}

std::vector<TimedMidiEvent> noteOffsOf (const MidiRenderResult& render)
{
    return render.select ([] (const TimedMidiEvent& event) { return event.message.isNoteOff (); });
}

std::vector<TimedMidiEvent> allNotesOffOf (const MidiRenderResult& render)
{
    return render.select ([] (const TimedMidiEvent& event) { return event.message.isControllerOfType (123); });
}

/** Counts events of a kind in a `juce::MidiBuffer` (for the table's own unit tests). */
int countIf (const juce::MidiBuffer& midi, bool (*predicate) (const juce::MidiMessage&))
{
    int count = 0;
    for (const auto meta : midi)
        if (predicate (meta.getMessage ()))
            ++count;
    return count;
}

bool isNoteOffMessage (const juce::MidiMessage& message)
{
    return message.isNoteOff ();
}
bool isAllNotesOffMessage (const juce::MidiMessage& message)
{
    return message.isControllerOfType (123);
}

/** Adds a raw 3-byte note-on, the way the sequencer does (no `juce::MidiMessage`
    construction), so the retrigger-ordering test composes exactly the same primitives
    `SequencerProcessor::emitStep` composes. */
void addRawNoteOn (juce::MidiBuffer& midi, int channel, int note, int velocity, int offset)
{
    const juce::uint8 bytes[3] = { static_cast<juce::uint8> (0x90 | ((channel - 1) & 0x0F)),
                                   static_cast<juce::uint8> (note & 0x7F),
                                   static_cast<juce::uint8> (velocity & 0x7F) };
    midi.addEvent (bytes, 3, offset);
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1. The scaffold pattern as a performance
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/scaffold: one bar is 16 ascending 16ths at velocity 100 with 50% gates", "[unit]")
{
    // One bar at 120 BPM / 48 kHz = 96000 samples = 750 blocks of 128, covering steps
    // 0..15 exactly (step 16 would sit on the bar line, which the half-open interval
    // excludes).
    REQUIRE (samplesPerScaffoldStep (120.0, testSampleRate) == static_cast<double> (stepSamples));
    REQUIRE (scaffoldGateSamples (120.0, testSampleRate) == gateSamples);

    SequencerRig rig { testSampleRate, testBlockSize };
    const auto render = renderSequencer (rig,
                                         nodeBlocks (750),
                                         { ScheduledCommand { 0, engineCommand (EngineCommandType::transportPlay) } });

    INFO (render.describe (40));
    REQUIRE (render.numSamples == 96000);
    REQUIRE (render.isSampleSorted ());

    const auto ons = noteOnsOf (render);
    const auto offs = noteOffsOf (render);
    REQUIRE (ons.size () == static_cast<std::size_t> (SequencerProcessor::scaffoldNumSteps));
    REQUIRE (offs.size () == ons.size ());
    REQUIRE (allNotesOffOf (render).empty ()); // no flush point in this render
    REQUIRE (render.size () == ons.size () + offs.size ());

    for (int step = 0; step < SequencerProcessor::scaffoldNumSteps; ++step)
    {
        const auto& on = ons[static_cast<std::size_t> (step)];
        const auto& off = offs[static_cast<std::size_t> (step)];
        INFO ("step " << step << "\n  on : " << on.describe () << "\n  off: " << off.describe ());

        // Position: step n at exactly n * 6000; gate ends 3000 samples later (50% of
        // the step, per scaffoldGateFraction — the LEN-lane-shaped contract of §12.1).
        const std::int64_t expectedOn = static_cast<std::int64_t> (step) * stepSamples;
        REQUIRE (on.absoluteSample == expectedOn);
        REQUIRE (off.absoluteSample == expectedOn + gateSamples);

        // Pitch, velocity, channel.
        REQUIRE (on.message.getNoteNumber () == scaffoldPitches[step]);
        REQUIRE (on.message.getVelocity () == SequencerProcessor::scaffoldVelocity);
        REQUIRE (on.message.getChannel () == SequencerProcessor::scaffoldChannel);

        // Pairing: the off is for the SAME pitch and channel as the on it closes.
        REQUIRE (off.message.getNoteNumber () == on.message.getNoteNumber ());
        REQUIRE (off.message.getChannel () == on.message.getChannel ());
    }

    // The pattern really ascends an octave twice (the audible property the scaffold
    // exists for), and the second half repeats the first.
    REQUIRE (ons[0].message.getNoteNumber () == 60);
    REQUIRE (ons[7].message.getNoteNumber () == 72);
    REQUIRE (ons[8].message.getNoteNumber () == 60);

    NoteLifecycleTracker tracker;
    tracker.observeAll (render);
    INFO (tracker.describe ());
    REQUIRE (tracker.noteOnsSeen () == SequencerProcessor::scaffoldNumSteps);
    REQUIRE (tracker.balanced ());
}

TEST_CASE ("sequencer/scaffold: a stopped transport emits nothing at all", "[unit]")
{
    // Guards every "the streams matched" assertion in the sweep from passing
    // vacuously: silence is what a stopped transport produces, so a render that
    // produced silence while PLAYING would be a bug, not a coincidence.
    SequencerRig rig { testSampleRate, testBlockSize };
    const auto render = renderSequencer (rig, nodeBlocks (200)); // no play command

    INFO (render.describe ());
    REQUIRE (render.empty ());
    REQUIRE (rig.sequencer.soundingNotes ().isEmpty ());
    REQUIRE (rig.transport.isPlaying () == false);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Flush points (§5.5) — the Phase 5 success criterion
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/flush: transport stop empties the sounding-note table", "[unit][midi-conformance]")
{
    SequencerRig rig { testSampleRate, testBlockSize };

    // Render 48 blocks (6144 samples) from the top: steps 0 (note 60 @ 0, released at
    // 3000) and 1 (note 62 @ 6000, gate open until 9000) have fired, so when this
    // render ends EXACTLY one note is sounding, mid-gate. That is the state a stop has
    // to clean up.
    const auto upToTheStop =
        renderSequencer (rig,
                         nodeBlocks (48),
                         { ScheduledCommand { 0, engineCommand (EngineCommandType::transportPlay) } });

    INFO (upToTheStop.describe ());
    REQUIRE (noteOnsOf (upToTheStop).size () == 2u);
    REQUIRE (noteOffsOf (upToTheStop).size () == 1u);
    REQUIRE (rig.sequencer.soundingNotes ().size () == 1);
    REQUIRE (rig.sequencer.soundingNotes ().find (SequencerProcessor::scaffoldChannel, 62) >= 0);
    REQUIRE (rig.sequencer.soundingNotes ().isEmpty () == false);

    // The stop is consumed at the head of this render's first block, and the sequencer
    // sees `stoppedThisBlock()` in the SAME block because the transport head node
    // renders first (§5.5, Transport.h stop semantics).
    const auto afterTheStop =
        renderSequencer (rig,
                         nodeBlocks (2),
                         { ScheduledCommand { 0, engineCommand (EngineCommandType::transportStop) } });

    INFO (afterTheStop.describe ());

    // (a) A note-off for every note that was sounding...
    const auto offs = noteOffsOf (afterTheStop);
    REQUIRE (offs.size () == 1u);
    REQUIRE (offs[0].absoluteSample == 0); // at the block head, where the discontinuity is
    REQUIRE (offs[0].message.getNoteNumber () == 62);
    REQUIRE (offs[0].message.getChannel () == SequencerProcessor::scaffoldChannel);

    // ...plus the CC123 belt-and-braces sweep, on the one channel we sounded on.
    const auto sweeps = allNotesOffOf (afterTheStop);
    REQUIRE (sweeps.size () == 1u);
    REQUIRE (sweeps[0].message.getChannel () == SequencerProcessor::scaffoldChannel);
    REQUIRE (sweeps[0].absoluteSample == 0);

    // The per-note off precedes the sweep: `juce::MidiBuffer` preserves insertion order
    // among equal timestamps, and §5.5 makes the per-note off the real mechanism.
    REQUIRE (afterTheStop.size () == 2u);
    REQUIRE (afterTheStop[0].message.isNoteOff ());
    REQUIRE (afterTheStop[1].message.isControllerOfType (123));

    // (b) THE success criterion.
    REQUIRE (rig.sequencer.soundingNotes ().isEmpty ());
    REQUIRE (rig.transport.isPlaying () == false);

    // Nothing leaks across the two renders taken together.
    NoteLifecycleTracker tracker;
    tracker.observeAll (upToTheStop);
    tracker.observeAll (afterTheStop);
    INFO (tracker.describe ());
    REQUIRE (tracker.balanced ());
}

TEST_CASE ("sequencer/flush: a locate empties the sounding-note table", "[unit][midi-conformance]")
{
    // A locate breaks the ABSOLUTE SAMPLE timeline the pending note-offs are scheduled
    // on (SoundingNoteTable.h: the timeline is not monotonic across a locate), which
    // would orphan them. So a position jump is a flush point in its own right, and —
    // unlike a stop — the transport keeps PLAYING through it.
    SequencerRig rig { testSampleRate, testBlockSize };

    const auto upToTheLocate =
        renderSequencer (rig,
                         nodeBlocks (48),
                         { ScheduledCommand { 0, engineCommand (EngineCommandType::transportPlay) } });
    REQUIRE (rig.sequencer.soundingNotes ().size () == 1);

    // Target PPQ 8.1 — deliberately BETWEEN step boundaries (steps sit on multiples of
    // 0.25), so this render contains the flush and nothing else. The next step boundary
    // (8.25) is 3600 samples away, well past the two blocks rendered here.
    const auto afterTheLocate =
        renderSequencer (rig,
                         nodeBlocks (2),
                         { ScheduledCommand { 0, engineCommand (EngineCommandType::transportLocate, 8.1) } });

    INFO (afterTheLocate.describe ());
    REQUIRE (rig.transport.isPlaying ()); // a locate does not stop the transport
    REQUIRE (rig.sequencer.soundingNotes ().isEmpty ());

    const auto offs = noteOffsOf (afterTheLocate);
    REQUIRE (offs.size () == 1u);
    REQUIRE (offs[0].absoluteSample == 0);
    REQUIRE (offs[0].message.getNoteNumber () == 62);
    REQUIRE (allNotesOffOf (afterTheLocate).size () == 1u);
    REQUIRE (noteOnsOf (afterTheLocate).empty ());

    NoteLifecycleTracker tracker;
    tracker.observeAll (upToTheLocate);
    tracker.observeAll (afterTheLocate);
    INFO (tracker.describe ());
    REQUIRE (tracker.balanced ());
}

TEST_CASE ("sequencer/flush: a stop the node never observed still flushes", "[unit][midi-conformance]")
{
    // THE MISSED-BLOCK SAFETY NET (SequencerProcessor.h, third flush point). This node
    // is spliced into the graph by an `UpdateKind::async` edit, so it can begin
    // rendering having NEVER seen the block whose latch carried the stop's edge — and
    // the edges are one-shot, so by the time it renders they are gone. The monotonic
    // `Transport::stopGeneration()` counter is what catches that; the edges alone would
    // not, and the note would hang for the rest of the session.
    SequencerRig rig { testSampleRate, testBlockSize };

    // Play until one note is sounding — and, importantly, until the node has seeded its
    // generation watermark (it does so on its first block, so that joining an
    // already-stopped session does not fire a spurious flush).
    const auto playing = renderSequencer (rig,
                                          nodeBlocks (48),
                                          { ScheduledCommand { 0, engineCommand (EngineCommandType::transportPlay) } });
    REQUIRE (noteOnsOf (playing).size () == 2u);
    REQUIRE (rig.sequencer.soundingNotes ().size () == 1);

    const auto generationBefore = rig.transport.stopGeneration ();

    // The block the node MISSES: the transport consumes the stop and latches its edges,
    // but the sequencer does not render, so it observes neither `stoppedThisBlock()` nor
    // `positionJumpedThisBlock()` for it.
    rig.transport.applyCommand (engineCommand (EngineCommandType::transportStop));
    rig.transport.beginBlock (testBlockSize);
    REQUIRE (rig.transport.stopGeneration () == generationBefore + 1);
    REQUIRE (rig.transport.stoppedThisBlock ());
    REQUIRE (rig.sequencer.soundingNotes ().size () == 1); // still holding the note

    // The next block it DOES render. The one-shot edges were consumed by the missed
    // block's latch, so they are false here — the flush can only come from the
    // generation comparison.
    const auto recovered = renderSequencer (rig, nodeBlocks (1));

    INFO (recovered.describe ());
    REQUIRE (rig.transport.stoppedThisBlock () == false);
    REQUIRE (rig.transport.positionJumpedThisBlock () == false);

    REQUIRE (rig.sequencer.soundingNotes ().isEmpty ());
    const auto offs = noteOffsOf (recovered);
    REQUIRE (offs.size () == 1u);
    REQUIRE (offs[0].message.getNoteNumber () == 62);
    REQUIRE (offs[0].absoluteSample == 0);
    REQUIRE (allNotesOffOf (recovered).size () == 1u);

    NoteLifecycleTracker tracker;
    tracker.observeAll (playing);
    tracker.observeAll (recovered);
    INFO (tracker.describe ());
    REQUIRE (tracker.balanced ());
}

TEST_CASE ("graph/flush: stopping through the assembled graph empties the sequencer table", "[unit][midi-conformance]")
{
    // The same criterion on the REAL path: commands through the engine command queue,
    // the transport head node draining them, the sequencer node spliced into
    // `Transport → MidiIn → Seq → Synth`, and the emitted MIDI captured where the synth
    // would receive it. `EngineGraph::getSequencer()` exists precisely so a headless
    // test can assert the §5.5 table invariant here.
    EngineGraph graph;
    graph.prepareToPlay (testSampleRate, testBlockSize);

    auto captureOwner = std::make_unique<MidiCaptureNode> ();
    captureOwner->setPlayConfigDetails (0, 2, testSampleRate, testBlockSize);
    captureOwner->prepareToPlay (testSampleRate, testBlockSize);
    auto* capture = captureOwner.get ();

    graph.setSynth (std::move (captureOwner));
    settleGraphEdits ();

    juce::AudioBuffer<float> buffer (2, testBlockSize);
    juce::MidiBuffer midi;

    // Warm up with the transport STOPPED so the async insertion is live and the clock
    // has not moved, then rebase the capture: absolute capture sample 0 now coincides
    // with transport sample 0.
    for (int i = 0; i < 4; ++i)
    {
        buffer.clear ();
        graph.getProcessor ().processBlock (buffer, midi);
    }
    REQUIRE (capture->blocksRendered () == 4);
    REQUIRE (capture->result ().empty ()); // stopped ⇒ silence
    capture->resetCapture ();

    REQUIRE (graph.commands ().push (engineCommand (EngineCommandType::transportPlay)));
    for (int i = 0; i < 48; ++i)
    {
        buffer.clear ();
        graph.getProcessor ().processBlock (buffer, midi);
    }

    INFO (capture->result ().describe ());
    REQUIRE (capture->droppedEvents () == 0);
    REQUIRE (graph.getSequencer ().soundingNotes ().size () == 1);
    REQUIRE (graph.getSequencer ().soundingNotes ().find (1, 62) >= 0);

    // The captured stream is the sequencer's output as the synth sees it, at absolute
    // sample positions matching the bare-node render above.
    const auto played = noteOnsOf (capture->result ());
    REQUIRE (played.size () == 2u);
    REQUIRE (played[0].absoluteSample == 0);
    REQUIRE (played[0].message.getNoteNumber () == 60);
    REQUIRE (played[1].absoluteSample == stepSamples);
    REQUIRE (played[1].message.getNoteNumber () == 62);

    REQUIRE (graph.commands ().push (engineCommand (EngineCommandType::transportStop)));
    buffer.clear ();
    graph.getProcessor ().processBlock (buffer, midi);

    // THE success criterion, on the assembled graph.
    REQUIRE (graph.getSequencer ().soundingNotes ().isEmpty ());

    const auto flushSample = static_cast<std::int64_t> (48) * testBlockSize;
    const auto offs = noteOffsOf (capture->result ());
    REQUIRE (offs.size () == 2u); // step 0's own release, then the flushed step 1
    REQUIRE (offs[1].absoluteSample == flushSample);
    REQUIRE (offs[1].message.getNoteNumber () == 62);

    const auto sweeps = allNotesOffOf (capture->result ());
    REQUIRE (sweeps.size () == 1u);
    REQUIRE (sweeps[0].absoluteSample == flushSample);
    REQUIRE (sweeps[0].message.getChannel () == 1);

    NoteLifecycleTracker tracker;
    tracker.observeAll (capture->result ());
    INFO (tracker.describe ());
    REQUIRE (tracker.noteOnsSeen () == 2);
    REQUIRE (tracker.balanced ());

    graph.removeSynth ();
    settleGraphEdits ();
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Note lifecycle over churn
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("support/note-lifecycle: the tracker detects orphans and unmatched note-ons", "[unit]")
{
    // The checker is the assertion every case below leans on, so pin that it FAILS on
    // the two failures it exists to detect — otherwise "balanced()" means nothing.
    NoteLifecycleTracker tracker;

    tracker.observe (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)));
    REQUIRE (tracker.outstanding () == 1);
    REQUIRE (tracker.balanced () == false); // an unreleased note-on is not balanced

    tracker.observe (juce::MidiMessage::noteOff (1, 60));
    REQUIRE (tracker.balanced ());
    REQUIRE (tracker.noteOnsSeen () == 1);
    REQUIRE (tracker.noteOffsSeen () == 1);

    // A note-off for a pitch that was not sounding is an orphan, and stays recorded.
    tracker.observe (juce::MidiMessage::noteOff (1, 64));
    REQUIRE (tracker.orphanNoteOffs () == 1);
    REQUIRE (tracker.outstanding () == 0);
    REQUIRE (tracker.balanced () == false);

    // A velocity-0 note-on releases, the way every synth treats it.
    NoteLifecycleTracker velocityZero;
    velocityZero.observe (juce::MidiMessage::noteOn (2, 48, static_cast<juce::uint8> (90)));
    velocityZero.observe (juce::MidiMessage::noteOn (2, 48, static_cast<juce::uint8> (0)));
    REQUIRE (velocityZero.balanced ());

    // CC123 is COUNTED but never applied: a flush that emitted only the sweep and no
    // per-note off must still read as unbalanced (§5.5 — the per-note offs are the
    // mechanism, the sweep is belt and braces).
    NoteLifecycleTracker sweepOnly;
    sweepOnly.observe (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)));
    sweepOnly.observe (juce::MidiMessage::allNotesOff (1));
    REQUIRE (sweepOnly.allNotesOffSeen () == 1);
    REQUIRE (sweepOnly.outstanding () == 1);
    REQUIRE (sweepOnly.balanced () == false);

    // Channel-aware: same note number on two channels are two different voices.
    NoteLifecycleTracker perChannel;
    perChannel.observe (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)));
    perChannel.observe (juce::MidiMessage::noteOff (5, 60));
    REQUIRE (perChannel.outstandingFor (1, 60) == 1);
    REQUIRE (perChannel.orphanNoteOffs () == 1);

    perChannel.reset ();
    REQUIRE (perChannel.balanced ());
    REQUIRE (perChannel.noteOnsSeen () == 0);
}

TEST_CASE ("sequencer/lifecycle: a scripted transport script leaves zero orphan note-ons", "[unit][midi-conformance]")
{
    // A deterministic (no RNG) churn script over one render: tempo extremes, a locate
    // mid-gate, stop/restart cycles. Every command sample is a multiple of 128 so it
    // lands on a block head. The script ENDS with a stop, which is the point at which
    // the §5.5 balance must close.
    SequencerRig rig { testSampleRate, testBlockSize };

    const std::vector<ScheduledCommand> script {
        ScheduledCommand { 0, engineCommand (EngineCommandType::transportPlay) },
        ScheduledCommand { 12800, engineCommand (EngineCommandType::setTempoBpm, 300.0) },
        ScheduledCommand { 25600, engineCommand (EngineCommandType::transportLocate, 4.7) },
        ScheduledCommand { 38400, engineCommand (EngineCommandType::transportStop) },
        ScheduledCommand { 39424, engineCommand (EngineCommandType::transportPlay) },
        ScheduledCommand { 44800, engineCommand (EngineCommandType::setTempoBpm, 20.0) },
        ScheduledCommand { 56320, engineCommand (EngineCommandType::transportStop) }
    };

    const auto render = renderSequencer (rig, nodeBlocks (480), script);

    INFO (render.summary ());
    REQUIRE (render.isSampleSorted ());

    NoteLifecycleTracker tracker;
    tracker.observeAll (render);
    INFO (tracker.describe ());

    // Non-vacuous: the legs are 120 BPM (6000-sample steps), then 300 BPM (2400), then
    // 20 BPM (36000), so the script plays roughly 17 steps in total. The bound is what
    // matters — a script that fell silent could not reach it.
    REQUIRE (tracker.noteOnsSeen () > 12);
    REQUIRE (tracker.allNotesOffSeen () >= 2); // one sweep per flush point reached

    // THE invariant: every note-on matched, no note-off without its note-on.
    REQUIRE (tracker.orphanNoteOffs () == 0);
    REQUIRE (tracker.outstanding () == 0);
    REQUIRE (tracker.balanced ());

    // ...corroborated by the engine's own authority, and the table never overflowed.
    REQUIRE (rig.sequencer.soundingNotes ().isEmpty ());
    REQUIRE (rig.sequencer.soundingNotes ().droppedNoteOnCount () == 0);
}

TEST_CASE ("sequencer/lifecycle: seeded transport churn leaves no stuck note", "[unit][midi-conformance]")
{
    // The Phase-5 ancestor of Phase 8.3's hanging-note fuzzer: random play / stop /
    // locate / tempo churn, rendered block by block (never slept on), asserting the
    // §5.5 invariants at the end. Every run takes an explicit, printed seed so a
    // failure reproduces from the seed alone (.claude/rules/testing.md).
    //
    // Bounded on purpose (≈4k blocks per seed) so it stays a per-commit test; Phase 8
    // scales it to the 10k-event runs its own success criterion asks for.
    const unsigned seed = GENERATE (1u, 1337u, 424242u);
    INFO ("seed = " << seed);

    std::mt19937 rng (seed);
    SequencerRig rig { testSampleRate, testBlockSize };
    NoteLifecycleTracker tracker;

    juce::AudioBuffer<float> audio (1, testBlockSize);
    juce::MidiBuffer midi;
    midi.ensureSize (8192);

    std::uniform_int_distribution<int> action (0, 9);
    std::uniform_int_distribution<int> blocksToRun (1, 40);
    std::uniform_real_distribution<double> locateTarget (0.0, 32.0);
    std::uniform_real_distribution<double> tempoBpm (20.0, 300.0);

    constexpr int iterations = 200;
    int maxTableSize = 0;
    int unsortedBlocks = 0;

    for (int it = 0; it < iterations; ++it)
    {
        const int a = action (rng);

        if (a <= 3) // play (40%) — keeps the transport running most of the time
            rig.transport.applyCommand (engineCommand (EngineCommandType::transportPlay));
        else if (a <= 5) // stop (20%) — the primary flush point
            rig.transport.applyCommand (engineCommand (EngineCommandType::transportStop));
        else if (a <= 7) // locate (20%) — the position-jump flush point
            rig.transport.applyCommand (engineCommand (EngineCommandType::transportLocate, locateTarget (rng)));
        else // tempo change (20%) — re-anchors the clock under sounding notes
            rig.transport.applyCommand (engineCommand (EngineCommandType::setTempoBpm, tempoBpm (rng)));

        const int blocks = blocksToRun (rng);
        for (int block = 0; block < blocks; ++block)
        {
            midi.clear ();
            rig.renderBlock (audio, midi);
            tracker.observeBuffer (midi);

            // Cheap per-block invariants only (no Catch2 macros in the hot loop):
            // events must be sample-sorted within the block, and the table must never
            // overflow.
            int previous = -1;
            for (const auto meta : midi)
            {
                if (meta.samplePosition < previous)
                    ++unsortedBlocks;
                previous = meta.samplePosition;
            }

            maxTableSize = juce::jmax (maxTableSize, rig.sequencer.soundingNotes ().size ());
        }
    }

    // Finish on a flush point: after a stop, the table MUST be empty and every note
    // the run started MUST have been released.
    rig.transport.applyCommand (engineCommand (EngineCommandType::transportStop));
    midi.clear ();
    rig.renderBlock (audio, midi);
    tracker.observeBuffer (midi);

    INFO (tracker.describe ());
    INFO ("max simultaneous sounding notes = " << maxTableSize);

    REQUIRE (unsortedBlocks == 0);
    REQUIRE (tracker.noteOnsSeen () > 50); // non-vacuous
    REQUIRE (tracker.orphanNoteOffs () == 0);
    REQUIRE (tracker.outstanding () == 0);
    REQUIRE (rig.sequencer.soundingNotes ().isEmpty ());
    REQUIRE (rig.sequencer.soundingNotes ().droppedNoteOnCount () == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. SoundingNoteTable in isolation
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("midi/sounding-table: find locates a sounding pitch per channel", "[unit]")
{
    SoundingNoteTable table;
    juce::MidiBuffer midi;

    REQUIRE (table.isEmpty ());
    REQUIRE (table.find (1, 60) == -1);

    REQUIRE (table.add (1, 60, 1000));
    REQUIRE (table.add (1, 64, 2000));
    REQUIRE (table.add (5, 60, 3000));

    REQUIRE (table.size () == 3);
    REQUIRE (table.find (1, 60) >= 0);
    REQUIRE (table.find (1, 64) >= 0);
    REQUIRE (table.find (5, 60) >= 0);

    // Channel and pitch both participate in identity — a table that ignored the
    // channel would retire the wrong voice under the retrigger policy.
    REQUIRE (table.find (2, 60) == -1);
    REQUIRE (table.find (1, 61) == -1);

    // Retiring removes the entry (and only that entry).
    table.retireAt (table.find (1, 60), midi, 4);
    REQUIRE (table.find (1, 60) == -1);
    REQUIRE (table.find (1, 64) >= 0);
    REQUIRE (table.find (5, 60) >= 0);
    REQUIRE (table.size () == 2);
    REQUIRE (countIf (midi, isNoteOffMessage) == 1);

    // Out-of-range indices are ignored rather than corrupting the table.
    table.retireAt (-1, midi, 0);
    table.retireAt (99, midi, 0);
    REQUIRE (table.size () == 2);
}

TEST_CASE ("midi/sounding-table: overflow drops the note-on and never a note-off", "[unit]")
{
    // THE overflow asymmetry (SoundingNoteTable.h): dropping a note-ON costs one
    // missing note; dropping a note-OFF costs a hung note for the rest of the session.
    // So `add` refuses at capacity and `flush` still releases everything it holds.
    SoundingNoteTable table;
    juce::MidiBuffer midi;

    for (int i = 0; i < SoundingNoteTable::capacity; ++i)
    {
        const int channel = 1 + (i / 128); // channels 1 and 2, notes 0..127 each
        const int note = i % 128;
        REQUIRE (table.add (channel, note, 1000 + i));
    }

    REQUIRE (table.size () == SoundingNoteTable::capacity);
    REQUIRE (table.droppedNoteOnCount () == 0);

    // One past capacity: refused, counted, and NOT registered.
    REQUIRE (table.add (3, 60, 5000) == false);
    REQUIRE (table.droppedNoteOnCount () == 1);
    REQUIRE (table.size () == SoundingNoteTable::capacity);
    REQUIRE (table.find (3, 60) == -1);

    REQUIRE (table.add (4, 61, 5000) == false);
    REQUIRE (table.droppedNoteOnCount () == 2);

    // Every held note is released — a full table is exactly when a dropped note-off
    // would be catastrophic.
    table.flush (midi, 0);
    REQUIRE (countIf (midi, isNoteOffMessage) == SoundingNoteTable::capacity);
    REQUIRE (countIf (midi, isAllNotesOffMessage) == 2); // only channels 1 and 2 sounded
    REQUIRE (table.isEmpty ());

    // Capacity is available again afterwards.
    REQUIRE (table.add (3, 60, 6000));
}

TEST_CASE ("midi/sounding-table: the due window is half-open on the exact sample timeline", "[unit]")
{
    // Integer arithmetic end to end (SoundingNoteTable.h): the emitted offset is
    // `due - blockStart`, so it is IDENTICAL at every buffer size, and an off due
    // exactly at the block's exclusive end belongs to the NEXT block.
    SoundingNoteTable table;
    juce::MidiBuffer midi;

    REQUIRE (table.add (1, 60, 1000)); // due exactly at the end of block [0, 1000)
    table.emitDueNoteOffs (midi, 0, 1000);
    REQUIRE (midi.isEmpty ());
    REQUIRE (table.size () == 1);

    // Next block: due at its very first sample ⇒ offset 0.
    table.emitDueNoteOffs (midi, 1000, 128);
    REQUIRE (countIf (midi, isNoteOffMessage) == 1);
    REQUIRE (table.isEmpty ());
    {
        auto iterator = midi.begin ();
        REQUIRE ((*iterator).samplePosition == 0);
    }

    // An interior due sample lands at its exact offset.
    midi.clear ();
    REQUIRE (table.add (1, 62, 1050));
    table.emitDueNoteOffs (midi, 1000, 128);
    REQUIRE (countIf (midi, isNoteOffMessage) == 1);
    {
        auto iterator = midi.begin ();
        REQUIRE ((*iterator).samplePosition == 50);
    }

    // An off already in the PAST (only reachable after an unflushed discontinuity) is
    // emitted at offset 0 rather than dropped: a late note-off is a blemish, a missing
    // one is a hung note.
    midi.clear ();
    REQUIRE (table.add (1, 64, 10));
    table.emitDueNoteOffs (midi, 5000, 128);
    REQUIRE (countIf (midi, isNoteOffMessage) == 1);
    REQUIRE (table.isEmpty ());
    {
        auto iterator = midi.begin ();
        REQUIRE ((*iterator).samplePosition == 0);
    }

    // Several notes due in one block are all released.
    midi.clear ();
    REQUIRE (table.add (1, 60, 100));
    REQUIRE (table.add (1, 62, 101));
    REQUIRE (table.add (1, 64, 102));
    table.emitDueNoteOffs (midi, 0, 128);
    REQUIRE (countIf (midi, isNoteOffMessage) == 3);
    REQUIRE (table.isEmpty ());

    // A zero-length block releases nothing (and must not read past the buffer).
    REQUIRE (table.add (1, 60, 0));
    table.emitDueNoteOffs (midi, 0, 0);
    REQUIRE (table.size () == 1);
}

TEST_CASE ("midi/sounding-table: the same-pitch retrigger policy emits off-then-on with a 1-sample gap",
           "[unit][midi-conformance]")
{
    // §5.5 overlap policy: "same-pitch retrigger ⇒ note-off then note-on with a
    // 1-sample gap". `SequencerProcessor::emitStep` implements it as
    // `retireAt (find (ch, note), midi, jmax (0, offset - 1))` followed by the note-on
    // at `offset` — but the SCAFFOLD PATTERN cannot reach it (no two consecutive steps
    // share a pitch, and a pitch only recurs 8 steps later, far beyond the 50% gate).
    // Phase 6's PITCH lane exercises it immediately; until then this case pins the two
    // primitives the policy is composed from, including the platform assumption at
    // offset 0.
    SECTION ("mid-block: the off lands one sample before the on")
    {
        SoundingNoteTable table;
        juce::MidiBuffer midi;

        REQUIRE (table.add (1, 60, 5000)); // note 60 already sounding

        constexpr int offset = 64;
        const int existing = table.find (1, 60);
        REQUIRE (existing >= 0);
        table.retireAt (existing, midi, juce::jmax (0, offset - 1));
        addRawNoteOn (midi, 1, 60, 100, offset);

        REQUIRE (table.find (1, 60) == -1); // the old voice is no longer tracked

        std::vector<juce::MidiMessage> messages;
        std::vector<int> positions;
        for (const auto meta : midi)
        {
            messages.push_back (meta.getMessage ());
            positions.push_back (meta.samplePosition);
        }

        REQUIRE (messages.size () == 2u);
        REQUIRE (messages[0].isNoteOff ());
        REQUIRE (messages[1].isNoteOn ());
        REQUIRE (positions[0] == offset - 1); // THE 1-sample gap
        REQUIRE (positions[1] == offset);
        REQUIRE (messages[0].getNoteNumber () == 60);
        REQUIRE (messages[1].getNoteNumber () == 60);
    }

    SECTION ("offset 0: both land on the same sample, off first by insertion order")
    {
        // There is no earlier sample in the block, so the gap cannot exist. Correctness
        // then rests on `juce::MidiBuffer` preserving INSERTION ORDER among events with
        // equal timestamps — if it ever sorted them differently, the note-on would be
        // swallowed by its own note-off. That platform assumption is asserted here
        // rather than assumed in a comment.
        SoundingNoteTable table;
        juce::MidiBuffer midi;

        REQUIRE (table.add (1, 60, 5000));
        const int existing = table.find (1, 60);
        REQUIRE (existing >= 0);
        table.retireAt (existing, midi, juce::jmax (0, 0 - 1));
        addRawNoteOn (midi, 1, 60, 100, 0);

        std::vector<juce::MidiMessage> messages;
        for (const auto meta : midi)
        {
            REQUIRE (meta.samplePosition == 0);
            messages.push_back (meta.getMessage ());
        }

        REQUIRE (messages.size () == 2u);
        REQUIRE (messages[0].isNoteOff ()); // the off still precedes the on
        REQUIRE (messages[1].isNoteOn ());
    }
}

TEST_CASE ("midi/sounding-table: flush sweeps CC123 only on channels it sounded on", "[unit][midi-conformance]")
{
    // WHY THE RESTRICTION MATTERS (SoundingNoteTable.h): this table lives in a node
    // between the MIDI-in node and the synth, so live THRU notes (QWERTY, hardware)
    // flow through the same buffer. An unconditional 16-channel CC123 sweep would kill
    // notes the user is physically holding — a real, audible bug. So the sweep must
    // cover every channel the table sounded on and NOT ONE MORE.
    SoundingNoteTable table;
    juce::MidiBuffer midi;

    REQUIRE (table.add (1, 60, 1000));
    REQUIRE (table.add (5, 72, 1000));
    REQUIRE (table.add (5, 79, 1000));

    constexpr int flushOffset = 8;
    table.flush (midi, flushOffset);

    REQUIRE (table.isEmpty ());
    REQUIRE (countIf (midi, isNoteOffMessage) == 3); // one per sounding note
    REQUIRE (countIf (midi, isAllNotesOffMessage) == 2);

    bool sweptChannel1 = false;
    bool sweptChannel5 = false;
    for (const auto meta : midi)
    {
        const auto message = meta.getMessage ();
        REQUIRE (meta.samplePosition == flushOffset); // the whole flush sits at the offset

        if (! message.isControllerOfType (123))
            continue;

        const int channel = message.getChannel ();
        // The load-bearing negative: nothing on a channel we never sounded on.
        REQUIRE ((channel == 1 || channel == 5));
        sweptChannel1 = sweptChannel1 || channel == 1;
        sweptChannel5 = sweptChannel5 || channel == 5;
    }
    REQUIRE (sweptChannel1);
    REQUIRE (sweptChannel5);

    // An EMPTY table's flush emits NOTHING AT ALL, which is what makes the sequencer's
    // defensive/redundant flushes (a stop while already stopped, the missed-block
    // safety net) harmless to live THRU notes.
    juce::MidiBuffer quiet;
    SoundingNoteTable emptyTable;
    emptyTable.flush (quiet, 0);
    REQUIRE (quiet.isEmpty ());

    // `reset()` is the teardown path: forgets everything and emits nothing.
    juce::MidiBuffer silent;
    SoundingNoteTable discarded;
    REQUIRE (discarded.add (1, 60, 1000));
    discarded.reset ();
    REQUIRE (discarded.isEmpty ());
    REQUIRE (silent.isEmpty ());
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. RT-safety — [perf-budget], excluded from the sanitizer `-L unit` runs
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/alloc-guard: zero allocations across the transport and sequencer path", "[perf-budget]")
{
    // ARCHITECTURE §11: "Audio-thread allocations in steady state = 0". The measured
    // region deliberately CONTAINS FLUSHES: a flush is the one place this node emits a
    // burst (every sounding note plus the CC123 sweep) into the outgoing MidiBuffer,
    // which is exactly the shape of the Phase-4 W2 bug — a buffer that grows on the
    // audio thread because its capacity was only ever sized for the common case.
    //
    // AllocationSentinel discipline (see support/AllocationSentinel.h): everything the
    // region touches is allocated BEFORE arming, the delta is read INSIDE the region
    // into a plain integer, and no REQUIRE / juce::String appears while armed.
    SequencerRig rig { testSampleRate, testBlockSize };

    juce::AudioBuffer<float> audio (1, testBlockSize);
    juce::MidiBuffer midi;
    midi.ensureSize (16384); // above the node's own 4096 warm-up ⇒ ensureSize never grows

    const auto play = engineCommand (EngineCommandType::transportPlay);
    const auto stop = engineCommand (EngineCommandType::transportStop);
    const auto locate = engineCommand (EngineCommandType::transportLocate, 6.5);
    const auto tempo = engineCommand (EngineCommandType::setTempoBpm, 174.0);

    // Warmup with the FULL path active — play, steps, note-offs and flushes — so any
    // one-time lazy allocation happens before the sentinel is armed.
    constexpr int warmupBlocks = 128;
    for (int i = 0; i < warmupBlocks; ++i)
    {
        if (i % 32 == 0)
            rig.transport.applyCommand (stop);
        else if (i % 32 == 1)
            rig.transport.applyCommand (play);
        else if (i % 32 == 17)
            rig.transport.applyCommand (locate);

        midi.clear ();
        rig.renderBlock (audio, midi);
    }

    constexpr int measuredBlocks = 512;
    std::uint64_t allocations = 0;
    {
        AllocationSentinel sentinel;
        for (int i = 0; i < measuredBlocks; ++i)
        {
            // A stop every 32 blocks puts a real flush (note-offs + CC123) inside the
            // armed region, with a locate and a tempo change for the other paths.
            if (i % 32 == 0)
                rig.transport.applyCommand (stop);
            else if (i % 32 == 1)
                rig.transport.applyCommand (play);
            else if (i % 32 == 17)
                rig.transport.applyCommand (locate);
            else if (i % 8 == 3)
                rig.transport.applyCommand (tempo);

            midi.clear ();
            rig.renderBlock (audio, midi);
        }
        allocations = sentinel.allocations ();
    }

    INFO ("steady-state allocations across " << measuredBlocks << " transport+sequencer blocks");
    REQUIRE (allocations == 0);
}
