// ─────────────────────────────────────────────────────────────────────────────
// pattern_alloc_guard — ARCHITECTURE §11 "Audio-thread allocations in steady
// state = 0" for the PHASE 6 paths specifically: snapshot ADOPTION and the
// quantized pattern-switch FLUSH.
//
// ── WHY A SECOND ALLOCATION GUARD ───────────────────────────────────────────
// sequencer_node.cpp already guards the transport+sequencer steady state, but
// its measured region contains neither of the two things Phase 6 added to
// `processBlock`. An allocation guard is only worth its runtime if the thing it
// is guarding actually happens inside the armed region, so this file's real
// content is the ANTI-VACUITY BOOKKEEPING: adoptions and flushes are counted
// while armed (with raw byte inspection, never a Catch2 macro or a
// `juce::MidiMessage`) and asserted to be non-zero afterwards. Without those
// counts, "zero allocations across 512 blocks" could describe 512 blocks in which
// nothing interesting occurred.
//
// ── THE MidiBuffer IS DELIBERATELY NOT PRE-SIZED ────────────────────────────
// Every other render loop in the suite calls `midi.ensureSize (16384)` so the
// harness buffer can never be the thing that grows. Here that would DEFEAT the
// test. `SequencerProcessor` warms the outgoing buffer to `outgoingWarmupBytes`
// (8192, doubled from 4096 in Phase 6 precisely because two flushes are now
// reachable in one block), and the only way to prove that constant is big enough
// is to let it be the sole sizing and watch for a growth allocation. So the buffer
// is left at whatever the node's own warm-up made it during the warmup blocks —
// `MidiBuffer::clear()` keeps capacity, so it does not shrink back.
//
// ── THE CONFIGURATION IS CHOSEN TO PUT TWO FLUSHES IN ONE BLOCK ─────────────
// 300 BPM on a 1/32 grid at 48 kHz makes a step 1200 samples, and a 4096-sample
// block therefore spans ~3.4 steps — enough for the walk to emit notes BEFORE the
// switch's adopt step is reached in the same block. Pair that with a locate at the
// block head (whose discontinuity flush is the first) and a beat-quantized switch
// resolving to a step further into the same block (whose flush is the second), and
// the "two flushes in one block" worst case the 8192 bound was sized for is
// actually exercised rather than merely reasoned about.
//
// PRESET: default only. The allocation counter replaces global operator
// new/delete and does not compose with ASan's own replacement — hence
// [perf-budget], which the sanitizer `-L unit` runs exclude.
// ─────────────────────────────────────────────────────────────────────────────

#include "support/AllocationSentinel.h"
#include "support/MidiRenderHarness.h"
#include "support/SequencerRenderRig.h"

#include "engine/graph/EngineCommand.h"
#include "engine/sequencer/PatternChannel.h"
#include "engine/sequencer/PatternDocument.h"
#include "engine/sequencer/PatternSnapshot.h"
#include "engine/sequencer/PatternTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

using arpbox::engine::buildPatternSnapshot;
using arpbox::engine::EngineCommandType;
using arpbox::engine::LaneId;
using arpbox::engine::maxSteps;
using arpbox::engine::PatternDocument;
using arpbox::engine::PatternSnapshot;
using arpbox::engine::QuantizeMode;
using arpbox::test::AllocationSentinel;
using arpbox::testing::engineCommand;
using arpbox::testing::patternSwitchCommand;
using arpbox::testing::SequencerRig;

namespace
{
constexpr double testSampleRate = 48000.0;
constexpr int testBlockSize = 4096;
constexpr double testBpm = 300.0;
constexpr double testGridPpq = 0.125; ///< 1/32 — 1200 samples per step at 300 BPM.

/** Locate target for the double-flush block: PPQ 3.70 is deliberately on neither a
    step boundary (3.70 / 0.125 = 29.6) nor a beat, so the block that follows it
    contains steps 30 and 31 BEFORE the beat-quantized switch lands on step 32. */
constexpr double doubleFlushLocatePpq = 3.70;

/** LEN as a percentage of the step. Above 100 guarantees a note is sounding at
    every step boundary, which is the only way a flush emits anything at all. */
constexpr int longGate = 150;

/** True if this MIDI event is a CC123 all-notes-off — i.e. the tail of a flush.
    Raw-byte inspection on purpose: building a `juce::MidiMessage` inside the armed
    region would be counted as an allocation for messages longer than three bytes,
    and the whole point is to touch nothing that can allocate. */
bool isFlushSweep (const juce::MidiMessageMetadata& meta) noexcept
{
    return meta.numBytes >= 2 && (meta.data[0] & 0xF0) == 0xB0 && meta.data[1] == 123;
}

/** Both patterns audible, distinguishable, and holding their notes across every
    step boundary — the configuration a flush has real work in. */
void configurePatterns (PatternDocument& document)
{
    document.beginTransaction ();
    document.setGrid (testGridPpq);

    for (int step = 0; step < maxSteps; ++step)
    {
        document.setLaneValue (0, LaneId::gate, step, 1);
        document.setLaneValue (1, LaneId::gate, step, 1);
        document.setLaneValue (0, LaneId::len, step, longGate);
        document.setLaneValue (1, LaneId::len, step, longGate);
        document.setLaneValue (1, LaneId::vel, step, 111);
    }

    document.endTransaction ();
}
} // namespace

TEST_CASE ("sequencer/alloc-guard: adoption and the pattern-switch flush allocate nothing", "[perf-budget]")
{
    SequencerRig rig { testSampleRate, testBlockSize };
    configurePatterns (rig.patternDocument);

    // From here on the document must NOT republish automatically: `publishTo`
    // reclaims (which deletes) and builds (which allocates), and neither may happen
    // inside the armed region. Publishing is done by hand from a pool of snapshots
    // built up front.
    rig.patternDocument.setPublishTarget (nullptr);

    constexpr int prebuiltCount = 8;
    std::vector<std::unique_ptr<const PatternSnapshot>> prebuilt;
    prebuilt.reserve (prebuiltCount);
    for (int i = 0; i < prebuiltCount; ++i)
        prebuilt.push_back (buildPatternSnapshot (rig.patternDocument.state (), 1000 + static_cast<std::uint64_t> (i)));

    // Everything the measured loop touches, allocated before arming. The MidiBuffer
    // is deliberately left unsized — see the header note.
    juce::AudioBuffer<float> audio (1, testBlockSize);
    juce::MidiBuffer midi;

    const auto play = engineCommand (EngineCommandType::transportPlay);
    const auto stop = engineCommand (EngineCommandType::transportStop);
    const auto locate = engineCommand (EngineCommandType::transportLocate, doubleFlushLocatePpq);
    const auto tempo = engineCommand (EngineCommandType::setTempoBpm, testBpm);

    rig.applyCommand (tempo);
    rig.applyCommand (play);

    // ── WARMUP ───────────────────────────────────────────────────────────────
    // The full path, including an adoption and both flush kinds, so every one-time
    // lazy allocation (JUCE scratch, the node's own outgoing-buffer warm-up, the
    // sounding-note table's first use) has already happened.
    constexpr int warmupBlocks = 96;
    for (int i = 0; i < warmupBlocks; ++i)
    {
        if (i == 8)
            rig.patternChannel.publish (buildPatternSnapshot (rig.patternDocument.state (), 1));
        if (i == 16)
        {
            rig.applyCommand (locate);
            rig.applyCommand (patternSwitchCommand (1, QuantizeMode::beat));
        }
        if (i == 40)
            rig.applyCommand (stop);
        if (i == 41)
            rig.applyCommand (play);
        if (i == 60)
            rig.applyCommand (patternSwitchCommand (0, QuantizeMode::instant));

        midi.clear ();
        rig.renderBlock (audio, midi);
    }

    rig.patternChannel.reclaim (); // deletes — must happen BEFORE arming
    REQUIRE (rig.sequencer.adoptedSnapshot () != nullptr);

    // ── THE MEASURED REGION ──────────────────────────────────────────────────
    constexpr int measuredBlocks = 512;
    constexpr int publishEvery = 64;

    std::uint64_t allocations = 0;
    int adoptsInside = 0;
    int flushSweepsInside = 0;
    int doubleFlushBlocks = 0;
    int notesInside = 0;
    int nextPrebuilt = 0;

    {
        AllocationSentinel sentinel;

        for (int i = 0; i < measuredBlocks; ++i)
        {
            if (i % publishEvery == 0 && nextPrebuilt < prebuiltCount)
                rig.patternChannel.publish (std::move (prebuilt[static_cast<std::size_t> (nextPrebuilt++)]));

            // The double-flush block: a locate (block-head discontinuity flush) plus
            // a beat-quantized switch that lands mid-block, after the walk has
            // already emitted notes into the same buffer.
            if (i % 128 == 40)
            {
                rig.applyCommand (locate);
                rig.applyCommand (
                    patternSwitchCommand (rig.sequencer.activePattern () == 0 ? 1 : 0, QuantizeMode::beat));
            }
            else if (i % 128 == 90)
            {
                // A plain quantized switch with no discontinuity around it.
                rig.applyCommand (
                    patternSwitchCommand (rig.sequencer.activePattern () == 0 ? 1 : 0, QuantizeMode::instant));
            }
            else if (i % 128 == 110)
            {
                rig.applyCommand (stop);
            }
            else if (i % 128 == 111)
            {
                rig.applyCommand (play);
            }

            const PatternSnapshot* const before = rig.sequencer.adoptedSnapshot ();

            midi.clear ();
            rig.renderBlock (audio, midi);

            if (rig.sequencer.adoptedSnapshot () != before)
                ++adoptsInside;

            int sweepsThisBlock = 0;
            for (const auto meta : midi)
            {
                if (isFlushSweep (meta))
                    ++sweepsThisBlock;
                else if (meta.numBytes == 3 && (meta.data[0] & 0xF0) == 0x90)
                    ++notesInside;
            }

            flushSweepsInside += sweepsThisBlock;
            if (sweepsThisBlock >= 2)
                ++doubleFlushBlocks;
        }

        allocations = sentinel.allocations ();
    }

    // Deleting is a message-thread job and happens only now the region is closed.
    rig.patternChannel.reclaim ();

    INFO ("adopts " << adoptsInside << ", flush sweeps " << flushSweepsInside << ", blocks with >= 2 sweeps "
                    << doubleFlushBlocks << ", note-ons " << notesInside);

    // ── ANTI-VACUITY: the region really contained what it claims to guard ────
    REQUIRE (adoptsInside == prebuiltCount);
    REQUIRE (flushSweepsInside > 0);
    REQUIRE (notesInside > 0);
    REQUIRE (doubleFlushBlocks > 0);                               // the 8192-byte warm-up's worst case, exercised
    REQUIRE (rig.patternChannel.getNumPendingRetirements () == 0); // reclaimed above
    REQUIRE (rig.patternChannel.getDroppedRetirementCount () == 0);
    REQUIRE (rig.sequencer.soundingNotes ().droppedNoteOnCount () == 0);

    // ── THE LOAD-BEARING ASSERTION ───────────────────────────────────────────
    INFO ("steady-state allocations across " << measuredBlocks << " blocks with adoption and switch flushes");
    REQUIRE (allocations == 0);
}
