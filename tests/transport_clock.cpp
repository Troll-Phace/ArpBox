// ─────────────────────────────────────────────────────────────────────────────
// transport_clock — Phase 5.1 transport tests (ARCHITECTURE §3.3, §4 step 2;
// INSTRUCTIONS Phase 5.1 / 5.3).
//
// Two layers:
//   1. `Transport` in isolation — the PPQ arithmetic, tempo/locate command
//      handling and stop semantics. These are the determinism-contract tests
//      (§1.2): PPQ must be EXACT and buffer-size independent, because every
//      Phase-6+ golden MIDI file is built on top of it. Where a value is
//      mathematically identical to the engine's own expression, the assertion is
//      exact equality on purpose — a float accumulator or per-block drift fails it.
//   2. The assembled `EngineGraph` — that the head node is the sole command
//      consumer, renders FIRST (so a command lands in the same block), that the
//      custom playhead reaches hosted nodes, and that the snapshot carries
//      transport state.
// ─────────────────────────────────────────────────────────────────────────────

#include "support/AllocationSentinel.h"

#include "engine/graph/EngineCommand.h"
#include "engine/graph/EngineGraph.h"
#include "engine/graph/EngineSnapshot.h"
#include "engine/graph/Transport.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_events/juce_events.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>

using arpbox::engine::EngineCommand;
using arpbox::engine::EngineCommandType;
using arpbox::engine::EngineGraph;
using arpbox::engine::EngineSnapshot;
using arpbox::engine::Transport;
using arpbox::test::AllocationSentinel;

namespace
{
constexpr double testSampleRate = 48000.0;
constexpr int testBlockSize = 128;

EngineCommand cmd (EngineCommandType type) noexcept
{
    EngineCommand c {};
    c.type = type;
    return c;
}

EngineCommand cmdDouble (EngineCommandType type, double value) noexcept
{
    EngineCommand c {};
    c.type = type;
    c.value.d = value;
    return c;
}

EngineCommand cmdInt (EngineCommandType type, std::int32_t value) noexcept
{
    EngineCommand c {};
    c.type = type;
    c.value.i = value;
    return c;
}

/** Applies one command to a bare transport, exactly as the head node would: the
    drain runs BEFORE beginBlock, in the same block. */
void applyThenBlock (Transport& transport, const EngineCommand& command, int numSamples)
{
    transport.applyCommand (command);
    transport.beginBlock (numSamples);
}

/** Runs `n` empty blocks (no commands). */
void runBlocks (Transport& transport, int n, int numSamples)
{
    for (int i = 0; i < n; ++i)
        transport.beginBlock (numSamples);
}

/** A stereo-out instrument stand-in that records what the graph-installed
    AudioPlayHead told it, so a test can prove the custom playhead actually reaches
    hosted nodes (§3.3 — synced plugin LFOs/delays depend on this). Emits silence. */
class PlayHeadProbe final : public juce::AudioProcessor
{
public:
    PlayHeadProbe ()
        : juce::AudioProcessor (BusesProperties ().withOutput ("Output", juce::AudioChannelSet::stereo (), true))
    {
    }

    const juce::String getName () const override { return "PlayHeadProbe"; }
    void prepareToPlay (double, int) override {}
    void releaseResources () override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        buffer.clear ();
        ++blocks;

        if (auto* head = getPlayHead ())
        {
            sawPlayHead = true;
            if (const auto pos = head->getPosition ())
            {
                if (const auto bpm = pos->getBpm ())
                    lastBpm = *bpm;
                if (const auto ppq = pos->getPpqPosition ())
                    lastPpq = *ppq;
                if (const auto barStart = pos->getPpqPositionOfLastBarStart ())
                    lastBarStart = *barStart;
                if (const auto samples = pos->getTimeInSamples ())
                    lastTimeInSamples = *samples;
                if (const auto sig = pos->getTimeSignature ())
                    lastNumerator = sig->numerator;
                lastIsPlaying = pos->getIsPlaying ();
                lastIsRecording = pos->getIsRecording ();
            }
        }
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

    int blocks = 0;
    bool sawPlayHead = false;
    double lastBpm = 0.0;
    double lastPpq = -1.0;
    double lastBarStart = -1.0;
    std::int64_t lastTimeInSamples = -1;
    int lastNumerator = 0;
    bool lastIsPlaying = false;
    bool lastIsRecording = true; ///< Starts true so a false assertion proves it was set.
};

/** Pumps the message loop in bounded slices so a queued async graph edit is applied
    before the caller renders. The budget is a HANG GUARD, not a sync primitive. */
void settleGraphEdits (int slices = 20, int msPerSlice = 5)
{
    for (int i = 0; i < slices; ++i)
        juce::MessageManager::getInstance ()->runDispatchLoopUntil (msPerSlice);
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Transport — PPQ arithmetic (the determinism contract starts here)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("transport/clock: starts stopped at PPQ 0 with the default tempo", "[unit]")
{
    Transport transport;
    transport.prepare (testSampleRate);

    REQUIRE (transport.isPlaying () == false);
    REQUIRE (transport.blockStartPpq () == 0.0);
    REQUIRE (transport.bpm () == Transport::defaultBpm);
    REQUIRE (transport.blockStartTimeInSamples () == 0);
    REQUIRE (transport.blockStartTimeInSeconds () == 0.0);
    REQUIRE (transport.ppqPerSample () == Transport::defaultBpm / (60.0 * testSampleRate));
}

TEST_CASE ("transport/clock: a stopped transport does not advance", "[unit]")
{
    Transport transport;
    transport.prepare (testSampleRate);

    runBlocks (transport, 32, testBlockSize);

    REQUIRE (transport.blockStartPpq () == 0.0);
    REQUIRE (transport.blockStartTimeInSamples () == 0);
    // A stopped block spans no musical time, so the sequencer's
    // [blockStartPpq, blockEndPpq) window is empty and it emits nothing.
    REQUIRE (transport.blockEndPpq () == transport.blockStartPpq ());
}

TEST_CASE ("transport/clock: PPQ is exact and free of accumulation drift", "[unit]")
{
    // THE determinism-contract assertion. PPQ is re-derived from an exact int64
    // sample counter every block (Transport.h), so after thousands of blocks it is
    // still bit-identical to the closed-form value. A per-block accumulator (float
    // OR double) drifts and fails this.
    Transport transport;
    transport.prepare (testSampleRate);
    applyThenBlock (transport, cmd (EngineCommandType::transportPlay), testBlockSize);

    constexpr int blocks = 10000;
    runBlocks (transport, blocks, testBlockSize);

    // The transport has now latched the start of block (blocks + 1).
    const auto elapsedSamples = static_cast<std::int64_t> (blocks) * testBlockSize;
    const double expected = static_cast<double> (elapsedSamples) * transport.ppqPerSample ();

    REQUIRE (transport.blockStartTimeInSamples () == elapsedSamples);
    REQUIRE (transport.blockStartPpq () == expected);
    REQUIRE (transport.blockStartTimeInSeconds () == static_cast<double> (elapsedSamples) / testSampleRate);
}

TEST_CASE ("transport/clock: PPQ at a given sample is buffer-size independent", "[determinism]")
{
    // Phase 5.3 acceptance criterion, at the clock level: the same ABSOLUTE sample
    // position must produce the identical PPQ no matter how the blocks were carved
    // up, or step boundaries would land on different samples at different buffer
    // sizes and the goldens would be buffer-size dependent.
    constexpr std::int64_t targetSample = 6144; // divisible by every size below
    double reference = -1.0;

    for (const int blockSize : { 32, 128, 512, 2048 })
    {
        Transport transport;
        transport.prepare (testSampleRate);
        transport.applyCommand (cmd (EngineCommandType::transportPlay));

        const auto blocks = static_cast<int> (targetSample / blockSize);
        runBlocks (transport, blocks + 1, blockSize); // +1 so the latch is AT targetSample

        REQUIRE (transport.blockStartTimeInSamples () == targetSample);

        if (reference < 0.0)
            reference = transport.blockStartPpq ();
        else
            REQUIRE (transport.blockStartPpq () == reference); // bit-identical
    }

    REQUIRE (reference > 0.0);
}

TEST_CASE ("transport/clock: within-block conversion is exact and invertible", "[unit]")
{
    Transport transport;
    transport.prepare (testSampleRate);
    applyThenBlock (transport, cmd (EngineCommandType::transportPlay), testBlockSize);
    runBlocks (transport, 7, testBlockSize);

    REQUIRE (transport.blockLength () == testBlockSize);
    REQUIRE (transport.ppqAtBlockOffset (0) == transport.blockStartPpq ());
    REQUIRE (transport.blockEndPpq () == transport.ppqAtBlockOffset (testBlockSize));
    REQUIRE (transport.blockOffsetForPpq (transport.blockStartPpq ()) == 0.0);

    // Round-trip every offset in the block: offset → ppq → offset.
    for (int offset = 0; offset < testBlockSize; ++offset)
    {
        const double ppq = transport.ppqAtBlockOffset (offset);
        const double back = transport.blockOffsetForPpq (ppq);
        REQUIRE (std::llround (back) == offset);
    }
}

TEST_CASE ("transport/clock: ppqOfLastBarStart snaps to the 4/4 bar grid", "[unit]")
{
    Transport transport;
    transport.prepare (testSampleRate);

    applyThenBlock (transport, cmdDouble (EngineCommandType::transportLocate, 0.0), testBlockSize);
    REQUIRE (transport.ppqOfLastBarStart () == 0.0);

    applyThenBlock (transport, cmdDouble (EngineCommandType::transportLocate, 5.5), testBlockSize);
    REQUIRE (transport.ppqOfLastBarStart () == 4.0);

    applyThenBlock (transport, cmdDouble (EngineCommandType::transportLocate, 8.0), testBlockSize);
    REQUIRE (transport.ppqOfLastBarStart () == 8.0);

    applyThenBlock (transport, cmdDouble (EngineCommandType::transportLocate, 15.999), testBlockSize);
    REQUIRE (transport.ppqOfLastBarStart () == 12.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Transport — tempo
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("transport/tempo: clamped to 20..300 and non-finite values rejected", "[unit]")
{
    Transport transport;
    transport.prepare (testSampleRate);

    applyThenBlock (transport, cmdDouble (EngineCommandType::setTempoBpm, 1000.0), testBlockSize);
    REQUIRE (transport.bpm () == Transport::maxBpm);

    applyThenBlock (transport, cmdDouble (EngineCommandType::setTempoBpm, 0.5), testBlockSize);
    REQUIRE (transport.bpm () == Transport::minBpm);

    applyThenBlock (transport, cmdDouble (EngineCommandType::setTempoBpm, -300.0), testBlockSize);
    REQUIRE (transport.bpm () == Transport::minBpm);

    applyThenBlock (transport, cmdDouble (EngineCommandType::setTempoBpm, 140.0), testBlockSize);
    REQUIRE (transport.bpm () == 140.0);
    REQUIRE (transport.ppqPerSample () == 140.0 / (60.0 * testSampleRate));

    // Non-finite payloads must be DROPPED, keeping the current tempo (issue #3
    // posture). A NaN reaching ppqPerSample would poison every later PPQ value.
    applyThenBlock (transport,
                    cmdDouble (EngineCommandType::setTempoBpm, std::numeric_limits<double>::quiet_NaN ()),
                    testBlockSize);
    REQUIRE (transport.bpm () == 140.0);

    applyThenBlock (transport,
                    cmdDouble (EngineCommandType::setTempoBpm, std::numeric_limits<double>::infinity ()),
                    testBlockSize);
    REQUIRE (transport.bpm () == 140.0);
    REQUIRE (std::isfinite (transport.blockStartPpq ()));
}

TEST_CASE ("transport/tempo: a tempo change lands exactly on a block boundary", "[unit]")
{
    // Within any single block exactly ONE rate is in force: the block's musical span
    // is length * ppqPerSample, with no mid-block rate switch. The tempo command is
    // consumed at the head of the block, so the NEW rate governs the whole block.
    Transport transport;
    transport.prepare (testSampleRate);
    applyThenBlock (transport, cmd (EngineCommandType::transportPlay), testBlockSize);

    constexpr int firstLeg = 10;
    runBlocks (transport, firstLeg, testBlockSize);

    const double rate120 = transport.ppqPerSample ();
    REQUIRE (transport.blockStartPpq () == static_cast<double> (firstLeg * testBlockSize) * rate120);

    // The next command drained will be consumed at the head of the NEXT block, whose
    // start is exactly this block's exclusive end. That is the boundary the tempo
    // change lands on.
    const double ppqAtChange = transport.blockEndPpq ();
    const std::int64_t changeSample = static_cast<std::int64_t> (firstLeg + 1) * testBlockSize;
    REQUIRE (ppqAtChange == static_cast<double> (changeSample) * rate120);

    // Tempo doubles at that block boundary.
    transport.applyCommand (cmdDouble (EngineCommandType::setTempoBpm, 240.0));
    transport.beginBlock (testBlockSize);

    const double rate240 = transport.ppqPerSample ();
    REQUIRE (rate240 == 240.0 / (60.0 * testSampleRate));

    // The block that consumed the change starts where the old leg ended (the change
    // moves the RATE, never the position) and spans one block at the NEW rate.
    REQUIRE (transport.blockStartTimeInSamples () == changeSample);
    REQUIRE (transport.blockStartPpq () == ppqAtChange);
    REQUIRE (transport.blockEndPpq () == ppqAtChange + static_cast<double> (testBlockSize) * rate240);

    constexpr int secondLeg = 10;
    runBlocks (transport, secondLeg, testBlockSize);

    // Position = old leg at the old rate + new leg at the new rate. Exact: one
    // re-anchor per tempo change, no accumulation.
    REQUIRE (transport.blockStartPpq () == ppqAtChange + static_cast<double> (secondLeg * testBlockSize) * rate240);
}

TEST_CASE ("transport/tempo: several tempo commands in one block cost no extra error", "[unit]")
{
    // Re-anchoring repeatedly at the SAME sample must be idempotent for the position
    // (the sample delta is 0), so a slider dragged fast — many setTempoBpm in one
    // block — cannot accumulate rounding error. Last value wins.
    Transport transport;
    transport.prepare (testSampleRate);
    applyThenBlock (transport, cmd (EngineCommandType::transportPlay), testBlockSize);
    runBlocks (transport, 5, testBlockSize);

    // Where the next block — the one that consumes the commands — will start.
    const double ppqAtChange = transport.blockEndPpq ();

    for (const double bpm : { 100.0, 111.0, 137.5, 200.0, 90.0 })
        transport.applyCommand (cmdDouble (EngineCommandType::setTempoBpm, bpm));
    transport.beginBlock (testBlockSize);

    REQUIRE (transport.bpm () == 90.0);
    REQUIRE (transport.blockStartPpq () == ppqAtChange);
}

// ─────────────────────────────────────────────────────────────────────────────
// Transport — stop / locate (the signals Phase 5.2 flushes on)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("transport/stop: stop rewinds, latches a one-block edge, bumps the generation", "[unit]")
{
    Transport transport;
    transport.prepare (testSampleRate);
    applyThenBlock (transport, cmd (EngineCommandType::transportPlay), testBlockSize);
    runBlocks (transport, 20, testBlockSize);

    REQUIRE (transport.isPlaying ());
    REQUIRE (transport.blockStartPpq () > 0.0);
    REQUIRE (transport.stoppedThisBlock () == false);

    const auto generationBefore = transport.stopGeneration ();

    applyThenBlock (transport, cmd (EngineCommandType::transportStop), testBlockSize);

    // The stop is visible in the SAME block it was drained in — this is what lets
    // the sequencer node (which renders after the head node) flush its sounding-note
    // table without a block of latency (§5.5).
    REQUIRE (transport.stoppedThisBlock ());
    REQUIRE (transport.positionJumpedThisBlock ()); // the implicit rewind IS a jump
    REQUIRE (transport.isPlaying () == false);
    REQUIRE (transport.blockStartPpq () == 0.0);
    REQUIRE (transport.blockStartTimeInSamples () == 0);
    REQUIRE (transport.stopGeneration () == generationBefore + 1);

    // ...and for EXACTLY that block. A latched edge that stuck would make the
    // sequencer flush forever.
    transport.beginBlock (testBlockSize);
    REQUIRE (transport.stoppedThisBlock () == false);
    REQUIRE (transport.positionJumpedThisBlock () == false);
    REQUIRE (transport.stopGeneration () == generationBefore + 1);
}

TEST_CASE ("transport/stop: a redundant stop re-requests the flush", "[midi-conformance]")
{
    // Deliberate: a stop while already stopped still raises the edge. A redundant
    // flush costs one CC123 sweep; suppressing it risks a hung note, which is the
    // far worse failure (§5.5).
    Transport transport;
    transport.prepare (testSampleRate);

    applyThenBlock (transport, cmd (EngineCommandType::transportStop), testBlockSize);
    REQUIRE (transport.stoppedThisBlock ());
    const auto generation = transport.stopGeneration ();

    applyThenBlock (transport, cmd (EngineCommandType::transportStop), testBlockSize);
    REQUIRE (transport.stoppedThisBlock ());
    REQUIRE (transport.stopGeneration () == generation + 1);
}

TEST_CASE ("transport/stop: play after stop restarts from the top", "[unit]")
{
    Transport transport;
    transport.prepare (testSampleRate);
    applyThenBlock (transport, cmd (EngineCommandType::transportPlay), testBlockSize);
    runBlocks (transport, 12, testBlockSize);
    applyThenBlock (transport, cmd (EngineCommandType::transportStop), testBlockSize);

    applyThenBlock (transport, cmd (EngineCommandType::transportPlay), testBlockSize);
    REQUIRE (transport.blockStartPpq () == 0.0);

    transport.beginBlock (testBlockSize);
    REQUIRE (transport.blockStartPpq () == static_cast<double> (testBlockSize) * transport.ppqPerSample ());
}

TEST_CASE ("transport/locate: repositions exactly and rejects invalid targets", "[unit]")
{
    Transport transport;
    transport.prepare (testSampleRate);

    applyThenBlock (transport, cmdDouble (EngineCommandType::transportLocate, 8.0), testBlockSize);
    REQUIRE (transport.blockStartPpq () == 8.0); // exact — the anchor IS the target
    REQUIRE (transport.positionJumpedThisBlock ());
    REQUIRE (transport.blockStartTimeInSamples () == std::llround (8.0 / transport.ppqPerSample ()));

    // Invalid targets are dropped and leave the playhead alone.
    for (const double bad :
         { -1.0, std::numeric_limits<double>::quiet_NaN (), -std::numeric_limits<double>::infinity () })
    {
        applyThenBlock (transport, cmdDouble (EngineCommandType::transportLocate, bad), testBlockSize);
        REQUIRE (transport.blockStartPpq () == 8.0);
        REQUIRE (transport.positionJumpedThisBlock () == false);
    }
}

TEST_CASE ("transport/locate: playing on from a located position stays exact", "[unit]")
{
    Transport transport;
    transport.prepare (testSampleRate);
    transport.applyCommand (cmdDouble (EngineCommandType::transportLocate, 16.0));
    applyThenBlock (transport, cmd (EngineCommandType::transportPlay), testBlockSize);

    constexpr int blocks = 64;
    runBlocks (transport, blocks, testBlockSize);

    REQUIRE (transport.blockStartPpq () ==
             16.0 + static_cast<double> (blocks * testBlockSize) * transport.ppqPerSample ());
}

TEST_CASE ("transport/clock: a sample-rate change preserves the musical position", "[unit]")
{
    // A mid-session device switch must not teleport the playhead: PPQ is preserved
    // and the sample counter is re-derived at the new rate.
    Transport transport;
    transport.prepare (testSampleRate);
    applyThenBlock (transport, cmd (EngineCommandType::transportPlay), testBlockSize);
    runBlocks (transport, 100, testBlockSize);

    // prepare() preserves the position the transport has REACHED — i.e. where the
    // next block would have started, which is this block's exclusive end.
    const double ppqBefore = transport.blockEndPpq ();
    const auto samplesBefore = transport.blockStartTimeInSamples ();
    REQUIRE (ppqBefore > 0.0);

    transport.prepare (44100.0);

    REQUIRE (transport.sampleRate () == 44100.0);
    REQUIRE (transport.ppqPerSample () == transport.bpm () / (60.0 * 44100.0));
    // Same musical position (within the one-sample rounding of the re-derivation).
    REQUIRE (std::abs (transport.blockStartPpq () - ppqBefore) <= transport.ppqPerSample ());
    // ...at a different sample count, because a quarter note is now fewer samples.
    REQUIRE (transport.blockStartTimeInSamples () != samplesBefore);
}

TEST_CASE ("transport/commands: unowned command types are ignored", "[unit]")
{
    // ICommandSink fan-out means the transport sees EVERY command. Reacting to one
    // it does not own would corrupt another sink's state.
    Transport transport;
    transport.prepare (testSampleRate);
    applyThenBlock (transport, cmd (EngineCommandType::transportPlay), testBlockSize);
    runBlocks (transport, 4, testBlockSize);

    const double ppq = transport.blockStartPpq ();
    const double bpm = transport.bpm ();

    for (const auto type : { EngineCommandType::none,
                             EngineCommandType::setMasterGainDb,
                             EngineCommandType::setLimiterEnabled,
                             EngineCommandType::setTestToneEnabled,
                             EngineCommandType::setTestToneFrequency })
    {
        EngineCommand c {};
        c.type = type;
        c.value.d = 999.0;
        transport.applyCommand (c);
    }

    REQUIRE (transport.bpm () == bpm);
    REQUIRE (transport.blockStartPpq () == ppq);
    REQUIRE (transport.isPlaying ());
}

// ─────────────────────────────────────────────────────────────────────────────
// EngineGraph integration — head-node ordering, snapshot, playhead
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("graph/transport: a command pushed before a block takes effect IN that block", "[unit]")
{
    // REGRESSION GUARD for the head-node render order (see the insertion-order note
    // in EngineGraph::buildGraph). The transport node must render BEFORE the master:
    // it drains the queue and latches the clock, and the master then snapshots it.
    // If the ordering ever inverted (JUCE's ordering heuristic changing, or the IO
    // nodes being added first again), the very first snapshot would still show
    // "stopped at the default tempo" and this case fails loudly.
    //
    // NOTE: this asserts the NEW Phase-5.1 behaviour. Under the Phase-2 arrangement
    // (master drained the queue itself) a command took effect one block LATE, so a
    // single-block assertion like this could not have passed.
    EngineGraph graph;
    graph.prepareToPlay (testSampleRate, testBlockSize);

    juce::AudioBuffer<float> buffer (2, testBlockSize);
    juce::MidiBuffer midi;

    REQUIRE (graph.commands ().push (cmd (EngineCommandType::transportPlay)));
    REQUIRE (graph.commands ().push (cmdDouble (EngineCommandType::setTempoBpm, 174.0)));

    buffer.clear ();
    graph.getProcessor ().processBlock (buffer, midi);

    const EngineSnapshot& s = graph.snapshots ().read ();
    REQUIRE (s.isPlaying);
    REQUIRE (s.bpm == 174.0);
    REQUIRE (s.ppqPosition == 0.0); // block-START latch: the first block starts at 0
}

TEST_CASE ("graph/transport: the snapshot advances PPQ every block", "[unit]")
{
    EngineGraph graph;
    graph.prepareToPlay (testSampleRate, testBlockSize);

    juce::AudioBuffer<float> buffer (2, testBlockSize);
    juce::MidiBuffer midi;

    REQUIRE (graph.commands ().push (cmd (EngineCommandType::transportPlay)));

    double previous = -1.0;
    for (int i = 0; i < 16; ++i)
    {
        buffer.clear ();
        graph.getProcessor ().processBlock (buffer, midi);

        const EngineSnapshot& s = graph.snapshots ().read ();
        REQUIRE (s.isPlaying);
        REQUIRE (s.bpm == Transport::defaultBpm);
        REQUIRE (s.ppqPosition > previous);
        previous = s.ppqPosition;
    }

    // 16 blocks of 128 at 120 BPM / 48 kHz = 2048 samples = 1 quarter note.
    const double expected =
        static_cast<double> (15 * testBlockSize) * (Transport::defaultBpm / (60.0 * testSampleRate));
    REQUIRE (graph.snapshots ().read ().ppqPosition == expected);
}

TEST_CASE ("graph/transport: stopping rewinds and keeps the snapshot consistent", "[unit]")
{
    EngineGraph graph;
    graph.prepareToPlay (testSampleRate, testBlockSize);

    juce::AudioBuffer<float> buffer (2, testBlockSize);
    juce::MidiBuffer midi;

    REQUIRE (graph.commands ().push (cmd (EngineCommandType::transportPlay)));
    for (int i = 0; i < 8; ++i)
    {
        buffer.clear ();
        graph.getProcessor ().processBlock (buffer, midi);
    }
    REQUIRE (graph.snapshots ().read ().ppqPosition > 0.0);

    REQUIRE (graph.commands ().push (cmd (EngineCommandType::transportStop)));
    buffer.clear ();
    graph.getProcessor ().processBlock (buffer, midi);

    const EngineSnapshot& s = graph.snapshots ().read ();
    REQUIRE (s.isPlaying == false);
    REQUIRE (s.ppqPosition == 0.0);
    REQUIRE (graph.getTransport ().stopGeneration () == 1);
}

TEST_CASE ("graph/transport: master commands still apply after the drain migration", "[unit]")
{
    // The master is now an ICommandSink fed by the head node rather than the queue's
    // consumer. Prove the path still works end-to-end: the limiter can be disabled
    // via a command and the difference is audible on the metered output.
    EngineGraph graph;
    graph.prepareToPlay (testSampleRate, testBlockSize);

    juce::AudioBuffer<float> buffer (2, testBlockSize);
    juce::MidiBuffer midi;

    REQUIRE (graph.commands ().push (cmdInt (EngineCommandType::setTestToneEnabled, 1)));

    EngineCommand hotGain {};
    hotGain.type = EngineCommandType::setMasterGainDb;
    hotGain.value.f = 40.0f;
    REQUIRE (graph.commands ().push (hotGain));

    const auto settle = [&]
    {
        for (int i = 0; i < 80; ++i)
        {
            buffer.clear ();
            graph.getProcessor ().processBlock (buffer, midi);
        }
    };

    settle ();
    const float limited = graph.snapshots ().read ().peakL;
    REQUIRE (limited > 0.0f);
    REQUIRE (limited <= 1.0001f);

    EngineCommand limiterOff {};
    limiterOff.type = EngineCommandType::setLimiterEnabled;
    limiterOff.value.i = 0;
    REQUIRE (graph.commands ().push (limiterOff));

    settle ();
    REQUIRE (graph.snapshots ().read ().peakL > 2.0f);
}

TEST_CASE ("graph/playhead: a hosted node sees the engine transport through the playhead", "[unit]")
{
    // ARCHITECTURE §3.3: the custom AudioPlayHead is what makes hosted plugins'
    // synced LFOs/delays work. `graph.setPlayHead()` is called ONCE at build time and
    // JUCE re-installs it on every node every block — including a node inserted later
    // by an async edit, which is exactly what this exercises.
    EngineGraph graph;
    graph.prepareToPlay (testSampleRate, testBlockSize);

    auto probeOwner = std::make_unique<PlayHeadProbe> ();
    probeOwner->setPlayConfigDetails (0, 2, testSampleRate, testBlockSize);
    probeOwner->prepareToPlay (testSampleRate, testBlockSize);
    auto* probe = probeOwner.get ();

    graph.setSynth (std::move (probeOwner));
    settleGraphEdits ();

    juce::AudioBuffer<float> buffer (2, testBlockSize);
    juce::MidiBuffer midi;

    REQUIRE (graph.commands ().push (cmd (EngineCommandType::transportPlay)));
    REQUIRE (graph.commands ().push (cmdDouble (EngineCommandType::setTempoBpm, 96.0)));
    REQUIRE (graph.commands ().push (cmdDouble (EngineCommandType::transportLocate, 4.0)));

    for (int i = 0; i < 8; ++i)
    {
        buffer.clear ();
        graph.getProcessor ().processBlock (buffer, midi);
    }

    REQUIRE (probe->blocks > 0);
    REQUIRE (probe->sawPlayHead);
    REQUIRE (probe->lastBpm == 96.0);
    REQUIRE (probe->lastIsPlaying);
    REQUIRE (probe->lastIsRecording == false);
    REQUIRE (probe->lastNumerator == 4);
    REQUIRE (probe->lastBarStart == 4.0);

    // The probe renders AFTER the head node (MidiIn → synth is downstream of the
    // ordering edge), so the position it saw is THIS block's, already advanced past
    // the locate target.
    REQUIRE (probe->lastPpq > 4.0);
    REQUIRE (probe->lastTimeInSamples > 0);

    graph.removeSynth ();
    settleGraphEdits ();
}

TEST_CASE ("graph/transport: transport churn allocates nothing in steady state", "[perf-budget]")
{
    // Extends the Phase-2 allocation guard to the new head-node path: the drain +
    // fan-out to two sinks, the transport's own arithmetic, and the playhead read the
    // hosted node performs every block must all be allocation-free.
    EngineGraph graph;
    graph.prepareToPlay (testSampleRate, testBlockSize);

    auto probeOwner = std::make_unique<PlayHeadProbe> ();
    probeOwner->setPlayConfigDetails (0, 2, testSampleRate, testBlockSize);
    probeOwner->prepareToPlay (testSampleRate, testBlockSize);
    graph.setSynth (std::move (probeOwner));
    settleGraphEdits ();

    juce::AudioBuffer<float> buffer (2, testBlockSize);
    juce::MidiBuffer midi;

    graph.commands ().push (cmd (EngineCommandType::transportPlay));

    // Warmup: JUCE allocates render-sequence buffers lazily on the first blocks.
    for (int i = 0; i < 64; ++i)
    {
        buffer.clear ();
        graph.getProcessor ().processBlock (buffer, midi);
    }

    constexpr int measuredBlocks = 256;
    std::uint64_t allocations = 0;
    {
        AllocationSentinel sentinel;
        for (int i = 0; i < measuredBlocks; ++i)
        {
            // Every block: a tempo change, and periodically a stop/play/locate cycle
            // so the rewind + edge-latching paths are inside the measured region.
            graph.commands ().push (cmdDouble (EngineCommandType::setTempoBpm, 100.0 + static_cast<double> (i % 50)));
            if (i % 37 == 0)
            {
                graph.commands ().push (cmd (EngineCommandType::transportStop));
                graph.commands ().push (cmdDouble (EngineCommandType::transportLocate, 8.0));
                graph.commands ().push (cmd (EngineCommandType::transportPlay));
            }

            graph.getProcessor ().processBlock (buffer, midi);
        }
        allocations = sentinel.allocations ();
    }

    INFO ("steady-state allocations across " << measuredBlocks << " transport blocks");
    REQUIRE (allocations == 0);

    graph.removeSynth ();
    settleGraphEdits ();
}
