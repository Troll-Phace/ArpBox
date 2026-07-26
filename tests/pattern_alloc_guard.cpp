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
// ── THREE CASES, AND WHAT EACH ONE CLOSES ───────────────────────────────────
// The FIRST is Phase 6's: adoption plus the two-flush block, described above.
//
// The SECOND is Phase 7.2's, and it exists because ratchets multiplied this node's
// per-block event count by up to eight while `outgoingWarmupBytes` was deliberately
// left at 8192 on an ARGUED margin (see the note on the constant, which names this
// file as the thing that turns the argument into a proof). It runs the same
// double-flush shape with RATCHET 8 on every step, displaced by MICRO and swing, and
// records the note-ons in the WORST single block plus the buffer's peak byte usage —
// so the margin is measured rather than reasoned about.
//
// The THIRD arms nothing. It pins `StepEmission`'s size and triviality at RUNTIME,
// which is the same contract one layer up: a `std::vector<StepNote>` for the note
// list would be an audio-thread allocation, and the header's `static_assert`s —
// while they are the real enforcement — never appear in test output, so nobody
// reading a green run learns what the number is.
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
#include "engine/sequencer/SequencerProcessor.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

using arpbox::engine::buildPatternSnapshot;
using arpbox::engine::EngineCommandType;
using arpbox::engine::LaneId;
using arpbox::engine::maxRatchetChildren;
using arpbox::engine::maxSteps;
using arpbox::engine::PatternDocument;
using arpbox::engine::PatternSnapshot;
using arpbox::engine::QuantizeMode;
using arpbox::engine::StepEmission;
using arpbox::engine::StepNote;
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

// ── THE PHASE 7.2 SCENARIO'S OWN CONFIGURATION ───────────────────────────────
// Kept separate from `configurePatterns` rather than folded into it: swing and the
// ratchet ramp are PROJECT-level fields, so setting them there would silently change
// what the first case measures — and the first case is the Phase 6 regression guard,
// which must keep measuring exactly what it measured.

/** Non-straight swing: odd steps carry +0.32 steps of displacement, so the walk
    cannot be visiting only grid positions. */
constexpr double ratchetSwingPct = 66.0;

/** Non-flat ratchet ramp, so `ratchetVelocity`'s per-child arithmetic runs inside the
    armed region instead of short-circuiting on the 0 default. */
constexpr double ratchetRampPct = -40.0;

/** LEN at its §12.1 ceiling. Above 100 % for the same reason `longGate` is — a note
    must be sounding at every flush point or the flush emits nothing and the sweep
    counters below go vacuous — and at the ceiling specifically because that is what
    drives the retrigger lookahead to its deepest legal reach. */
constexpr int ratchetGate = 400;

/** MIRROR of `outgoingWarmupBytes` (SequencerProcessor.cpp), which is file-local there
    and cannot be named from a test. DIAGNOSTIC ONLY — it appears in an `INFO` and in
    no assertion, so if the engine constant moves this copy going stale can make the
    printed headroom misleading but can never make the case pass or fail wrongly. */
constexpr int mirroredOutgoingWarmupBytes = 8192;

/** MICRO for `step`, in §12.1's -50..+50 percent-of-a-step units: -40, -20, 0, +20,
    +40, repeating. It STRADDLES ZERO so the walk's widened scan runs in both
    directions (a one-sided MICRO would exercise only `stepScanForward`), and +40
    composed with `ratchetSwingPct` saturates the ±`maxSubStepShiftSteps` clamp, which
    is the bound the scan widening's two constants are derived from. */
int ratchetMicroFor (int step) noexcept
{
    return ((step % 5) - 2) * 20;
}

/** RATCHET 8 on every step of both patterns, displaced, gated long. The
    eight-children-per-step case §11's zero-allocation claim was never armed against
    before Phase 7.3. */
void configureRatchetPatterns (PatternDocument& document)
{
    document.beginTransaction ();
    document.setGrid (testGridPpq);
    document.setSwing (ratchetSwingPct);
    document.setRatchetVelocityRamp (ratchetRampPct);

    for (int step = 0; step < maxSteps; ++step)
        for (int pattern = 0; pattern < 2; ++pattern)
        {
            document.setLaneValue (pattern, LaneId::gate, step, 1);
            document.setLaneValue (pattern, LaneId::len, step, ratchetGate);
            document.setLaneValue (pattern, LaneId::ratchet, step, maxRatchetChildren);
            document.setLaneValue (pattern, LaneId::micro, step, ratchetMicroFor (step));

            // Pattern 1 audibly distinguishable, as in the first case.
            if (pattern == 1)
                document.setLaneValue (pattern, LaneId::vel, step, 111);
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

// ─────────────────────────────────────────────────────────────────────────────
// THIS CASE CONVERTS AN ARGUED MARGIN INTO A PROOF.
//
// `outgoingWarmupBytes` (SequencerProcessor.cpp) stayed at 8192 through Phase 7.2 on
// a paragraph of arithmetic: ratchets multiply this node's note events by eight, the
// tightest supported step bounds how many steps a block can hold, therefore the extra
// traffic fits beside the two-flush worst case. The comment on the constant says in
// as many words that the margin is ARGUED, NOT PROVED, and names this file as what
// closes the gap. So the scenario below is the argument's own worst case, armed:
// RATCHET 8 on EVERY step, at the configuration that puts ~3.4 steps in one block,
// with the first case's double-flush shape inside the same block.
//
// ── THE FAILS-WITHOUT, AND ITS HONEST THRESHOLD ─────────────────────────────
// The intended demonstration is to drop `outgoingWarmupBytes` and watch
// `juce::MidiBuffer` grow — a growth allocation is exactly what the counter sees, and
// the buffer here is UNSIZED (see the header) so the node's own warm-up is the only
// thing standing between this scenario and one. The parent agent runs that demo; the
// number to drop it TO is not a guess, because this case prints the block's PEAK BYTE
// USAGE (`midi.data.size()`, a raw read — no allocation, no Catch2 macro) beside the
// mirrored 8192. Drop the constant below that measured peak and the counter goes
// non-zero; drop it to 4096 and it may well NOT, because this scenario's traffic is
// bounded by the number of PITCHES the sounding-note table can hold at once, which
// ratchets do not raise (all eight children share their parent's pitch). Read the
// printed peak before choosing the threshold rather than assuming 4096 is under it.
//
// AND IF THIS CASE REDDENS AT 8192, THE ANSWER IS 16384 — not a weakened floor, not a
// pre-sized harness buffer, and not a re-derivation of the paragraph on the constant.
// The whole point of leaving the constant alone was that the margin be falsifiable.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE ("sequencer/alloc-guard: eight ratchet children per step allocate nothing", "[perf-budget]")
{
    SequencerRig rig { testSampleRate, testBlockSize };
    configureRatchetPatterns (rig.patternDocument);

    // Same discipline as the first case, and for the same reason: `publishTo` reclaims
    // (deletes) and builds (allocates), so publishing is done by hand from a pool built
    // before the region is armed.
    rig.patternDocument.setPublishTarget (nullptr);

    constexpr int prebuiltCount = 8;
    std::vector<std::unique_ptr<const PatternSnapshot>> prebuilt;
    prebuilt.reserve (prebuiltCount);
    for (int i = 0; i < prebuiltCount; ++i)
        prebuilt.push_back (buildPatternSnapshot (rig.patternDocument.state (), 2000 + static_cast<std::uint64_t> (i)));

    juce::AudioBuffer<float> audio (1, testBlockSize);
    juce::MidiBuffer midi; // UNSIZED, deliberately — see the header.

    const auto play = engineCommand (EngineCommandType::transportPlay);
    const auto stop = engineCommand (EngineCommandType::transportStop);
    const auto locate = engineCommand (EngineCommandType::transportLocate, doubleFlushLocatePpq);
    const auto tempo = engineCommand (EngineCommandType::setTempoBpm, testBpm);

    rig.applyCommand (tempo);
    rig.applyCommand (play);

    // ── WARMUP ───────────────────────────────────────────────────────────────
    // The same shape as the first case's, so every one-time lazy allocation — and in
    // particular the node's own `ensureSize` of the outgoing buffer — has already
    // happened by the time the sentinel is constructed.
    constexpr int warmupBlocks = 96;
    for (int i = 0; i < warmupBlocks; ++i)
    {
        if (i == 8)
            rig.patternChannel.publish (buildPatternSnapshot (rig.patternDocument.state (), 2));
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
    int noteOnsInside = 0;
    int worstBlockNoteOns = 0;
    int peakBufferBytes = 0;
    int nextPrebuilt = 0;

    {
        AllocationSentinel sentinel;

        for (int i = 0; i < measuredBlocks; ++i)
        {
            if (i % publishEvery == 0 && nextPrebuilt < prebuiltCount)
                rig.patternChannel.publish (std::move (prebuilt[static_cast<std::size_t> (nextPrebuilt++)]));

            if (i % 128 == 40)
            {
                rig.applyCommand (locate);
                rig.applyCommand (
                    patternSwitchCommand (rig.sequencer.activePattern () == 0 ? 1 : 0, QuantizeMode::beat));
            }
            else if (i % 128 == 90)
            {
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
            int noteOnsThisBlock = 0;

            for (const auto meta : midi)
            {
                if (isFlushSweep (meta))
                    ++sweepsThisBlock;
                else if (meta.numBytes == 3 && (meta.data[0] & 0xF0) == 0x90)
                    ++noteOnsThisBlock;
            }

            flushSweepsInside += sweepsThisBlock;
            noteOnsInside += noteOnsThisBlock;

            if (sweepsThisBlock >= 2)
                ++doubleFlushBlocks;

            // THE PER-BLOCK MAXIMUM, not the total: the constant being proved is a
            // capacity, so what matters is the fullest single block, and a total over
            // 512 blocks would be satisfied by 512 quiet ones.
            if (noteOnsThisBlock > worstBlockNoteOns)
                worstBlockNoteOns = noteOnsThisBlock;

            // Raw capacity read — this is the quantity the fails-without threshold is
            // chosen against (see the note above the case). `juce::Array::size()` is a
            // plain member read; nothing here allocates.
            if (const int bytes = midi.data.size (); bytes > peakBufferBytes)
                peakBufferBytes = bytes;
        }

        allocations = sentinel.allocations ();
    }

    // Deleting is a message-thread job and happens only now the region is closed.
    rig.patternChannel.reclaim ();

    INFO ("adopts " << adoptsInside << ", flush sweeps " << flushSweepsInside << ", blocks with >= 2 sweeps "
                    << doubleFlushBlocks << ", note-ons " << noteOnsInside << ", worst single block "
                    << worstBlockNoteOns << " note-ons, peak MidiBuffer bytes " << peakBufferBytes
                    << " against a mirrored warm-up of " << mirroredOutgoingWarmupBytes);

    // ── ANTI-VACUITY: the ratchets really fired, and really coincided with the
    //    two flushes ─────────────────────────────────────────────────────────────
    // 24 = three steps x eight children, the floor the 8192-byte margin was argued
    // against. A block spans 4096 / 1200 = 3.41 steps, so a block owning three whole
    // steps' worth of children is the least this configuration can produce; a case
    // that silently lost the RATCHET lane (a clamp, a lane-length mistake, a document
    // edit rejected and its `false` return ignored) would land at 3 or 4 here and
    // "zero allocations" would then be a statement about a Phase 6 pattern.
    REQUIRE (worstBlockNoteOns >= 24);
    REQUIRE (noteOnsInside > 0);

    // The double-flush block happened, and happened at least once WITH both sweeps in
    // the same buffer — which is the shape 8192 was sized for. Two counters rather
    // than one because a scenario that flushed twice in two different blocks would
    // satisfy the total and prove nothing about the capacity.
    REQUIRE (flushSweepsInside >= 2);
    REQUIRE (doubleFlushBlocks >= 1);

    REQUIRE (adoptsInside == prebuiltCount);
    REQUIRE (rig.patternChannel.getNumPendingRetirements () == 0); // reclaimed above
    REQUIRE (rig.patternChannel.getDroppedRetirementCount () == 0);

    // Eight children per step means eight times the table traffic; a dropped note-on
    // is a note the table never took ownership of, i.e. a §5.5 hanging note that no
    // flush can reach.
    REQUIRE (rig.sequencer.soundingNotes ().droppedNoteOnCount () == 0);

    // ── THE LOAD-BEARING ASSERTION ───────────────────────────────────────────
    INFO ("steady-state allocations across " << measuredBlocks
                                             << " ratcheted blocks with adoption and both flush kinds");
    REQUIRE (allocations == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// STRUCTURAL PINS — no arming, no rendering, no rig.
//
// `StepEmission`'s header carries `static_assert`s on trivial copyability, and those
// are the real enforcement: a `std::vector<StepNote>` for the note list is an
// audio-thread allocation and the build refuses it. What the `static_assert`s cannot
// do is TELL ANYBODY THE NUMBER. The type is constructed once per step per block plus
// up to nine times per emitted note by the retrigger lookahead, all by value, so its
// size is a per-block cost that a future field can quietly triple; pinning it at
// runtime puts the figure in the test output, where a reviewer reading a green run
// sees it. The triviality checks are restated here for the same reason and not because
// the compiler needs help.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE ("sequencer/step-emission: the note list stays a fixed-size POD", "[perf-budget]")
{
    // DERIVED FROM THE MEMBER LAYOUT, not measured and pasted back:
    //   int note (4) + int velocity (4) + double positionInStep (8)
    //     + double gateFractionOfStep (8) + std::uint32_t provenance (4)
    //     + 4 tail padding to the 8-byte alignment the doubles impose = 32.
    INFO ("sizeof (StepNote) == " << sizeof (StepNote));
    REQUIRE (sizeof (StepNote) == 32);

    //   bool gate (1) + 3 pad + int channel (4) + int noteCount (4) + 4 pad
    //     + double shiftSteps (8) + std::array<StepNote, 8> notes (8 x 32 = 256) = 280.
    // The array is the whole of it: the eight-child ceiling is §12.1's RATCHET range,
    // so this number moves if and only if that range or `StepNote` does.
    INFO ("sizeof (StepEmission) == " << sizeof (StepEmission) << ", of which notes[] is "
                                      << sizeof (StepEmission::notes));
    REQUIRE (sizeof (StepEmission) == 280);
    REQUIRE (sizeof (StepEmission::notes) == 256);

    // The header's `static_assert`s again, in a form that shows up in a test report.
    REQUIRE (std::is_trivially_copyable_v<StepEmission>);
    REQUIRE (std::is_trivially_copyable_v<StepNote>);
}
