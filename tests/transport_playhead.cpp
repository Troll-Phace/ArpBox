// ─────────────────────────────────────────────────────────────────────────────
// transport_playhead — INSTRUCTIONS Phase 5.3 success criterion #2, "Hosted fake
// plugin observes correct BPM/PPQ via AudioPlayHead", on the path a REAL synth
// actually takes (ARCHITECTURE §3.3 the custom AudioPlayHead, §6.3 the wrapper).
//
// WHY A SECOND PLAYHEAD FILE. tests/transport_clock.cpp already proves the graph
// hands its playhead to a BARE node (its local `PlayHeadProbe`). No shipped plugin
// is a bare node: it is a `juce::AudioPluginInstance` living INSIDE a
// `HostedPluginNode`, and the graph installs the playhead on the WRAPPER only — it
// knows nothing about the instance the wrapper owns. The wrapper's one-line
// `setPlayHead` override is what bridges that gap, and it is exactly the kind of
// override a future refactor drops without noticing. So everything here drives the
// WRAPPED path, with `PlayHeadObservingFake` (tests/fakes/FakePlugins.h) as the
// inner instance.
//
// INDEPENDENCE OF THE EXPECTED VALUES (the point of the exercise). Every expected
// PPQ below is computed from FIRST PRINCIPLES —
//     ppqPerSample  = bpm / (60 * sampleRate)
//     expectedPpq(i) = (i * blockSize) * ppqPerSample
// — from the bpm/sampleRate/blockSize constants the test itself chose. Nothing is
// read back out of the `Transport` under test; reading it back would make these
// cases tautologies that pass even if the transport were wrong.
//
// Phase 4 shipped a defect for weeks because its assertions used a meter value as a
// "signal proxy" instead of the signal. These cases assert the observed BPM/PPQ/
// play-state themselves.
// ─────────────────────────────────────────────────────────────────────────────

#include "fakes/FakePlugins.h"
#include "fakes/HostedSynthGraphSupport.h"

#include "engine/graph/EngineCommand.h"
#include "engine/graph/EngineGraph.h"

#include "hosting/HostedPluginNode.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_events/juce_events.h>

#include <cmath>
#include <cstdint>
#include <memory>

using arpbox::engine::EngineCommand;
using arpbox::engine::EngineCommandType;
using arpbox::engine::EngineGraph;
using arpbox::hosting::HostedPluginNode;
using arpbox::testing::FakeBehavior;
using arpbox::testing::kGraphBlockSize;
using arpbox::testing::kGraphSampleRate;
using arpbox::testing::makeFakeInstance;
using arpbox::testing::PlayHeadObservation;
using arpbox::testing::PlayHeadObservingFake;
using arpbox::testing::settleGraphEdits;
using arpbox::testing::specFor;

namespace
{
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

/** Quarter notes per sample, from first principles. The ONLY place this test file
    derives musical rate, and it never consults the engine to do it. */
constexpr double ppqPerSampleFor (double bpm, double sampleRate) noexcept
{
    return bpm / (60.0 * sampleRate);
}

/** Expected PPQ at the start of measured block `blockIndex`, counting from a
    transport that started at PPQ 0. */
double expectedPpqAtBlock (int blockIndex, int blockSize, double bpm, double sampleRate) noexcept
{
    const auto sample = static_cast<std::int64_t> (blockIndex) * static_cast<std::int64_t> (blockSize);
    return static_cast<double> (sample) * ppqPerSampleFor (bpm, sampleRate);
}

/** A real `EngineGraph` with a `PlayHeadObservingFake` WRAPPED in a
    `HostedPluginNode` in the synth slot — the production shape (§6.3), headless.

    The wrapper adopts an ALREADY-PREPARED inner (ctor contract), and `setSynth`
    applies with `UpdateKind::async`, so the constructor pumps `settleGraphEdits()`
    before returning: the rig is renderable the moment you have it. */
struct WrappedObserverRig
{
    EngineGraph graph;
    HostedPluginNode* node = nullptr;      ///< Non-owning; graph owns the node.
    PlayHeadObservingFake* fake = nullptr; ///< Non-owning; the wrapper owns the inner.
    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;
    double sampleRate;
    int blockSize;

    explicit WrappedObserverRig (double rate = kGraphSampleRate, int size = kGraphBlockSize)
        : buffer (2, size), sampleRate (rate), blockSize (size)
    {
        graph.prepareToPlay (rate, size);

        auto inner = makeFakeInstance (specFor (FakeBehavior::playHeadObserving));
        fake = dynamic_cast<PlayHeadObservingFake*> (inner.get ());
        REQUIRE (fake != nullptr);
        inner->prepareToPlay (rate, size); // wrapper ctor contract: prepared inner

        auto wrapper = std::make_unique<HostedPluginNode> (std::move (inner));
        node = wrapper.get ();

        graph.setSynth (std::move (wrapper));
        settleGraphEdits ();
    }

    ~WrappedObserverRig ()
    {
        graph.removeSynth ();
        settleGraphEdits ();
    }

    void push (const EngineCommand& command) { graph.commands ().push (command); }

    void render (int blocks)
    {
        for (int i = 0; i < blocks; ++i)
        {
            buffer.clear ();
            graph.getProcessor ().processBlock (buffer, midi);
        }
    }

    /** Renders a couple of blocks so the async insertion is definitely live and JUCE's
        lazy render-sequence buffers exist, then clears the log so observation index 0
        is the first block of the run under test. The transport is still stopped here,
        so no musical time passes during warmup. */
    void warmUpAndReset (int warmupBlocks = 2)
    {
        render (warmupBlocks);
        REQUIRE (fake->blocksRendered () == warmupBlocks); // the node IS in the graph
        fake->resetObservations ();
    }
};
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// The criterion: a WRAPPED hosted plugin sees the engine's BPM and PPQ
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("hosting/playhead: a wrapped hosted plugin observes the engine BPM and PPQ", "[hosting-lab]")
{
    constexpr double bpm = 96.0;
    constexpr int measuredBlocks = 32;

    WrappedObserverRig rig;
    rig.warmUpAndReset ();

    rig.push (cmdDouble (EngineCommandType::setTempoBpm, bpm));
    rig.push (cmd (EngineCommandType::transportPlay));
    rig.render (measuredBlocks);

    const auto& observations = rig.fake->observations ();
    REQUIRE (rig.fake->droppedObservations () == 0);
    REQUIRE (static_cast<int> (observations.size ()) == measuredBlocks);

    // Not one block without a playhead, and not one without a position: this is the
    // §3.3 contract, and the wrapper-forwarding signal (see the regression case).
    REQUIRE (rig.fake->nullPlayHeadBlocks () == 0);
    REQUIRE (rig.fake->missingPositionBlocks () == 0);

    for (int i = 0; i < measuredBlocks; ++i)
    {
        const PlayHeadObservation& observation = observations[static_cast<std::size_t> (i)];
        INFO ("block " << i << ": bpm=" << observation.bpm << " ppq=" << observation.ppqPosition
                       << " samples=" << observation.timeInSamples
                       << " playing=" << observation.isPlaying);

        REQUIRE (observation.blockIndex == i);
        REQUIRE (observation.hadPlayHead);
        REQUIRE (observation.hadPosition);
        REQUIRE (observation.isPlaying); // criterion: play-state reaches the plugin

        // Tempo: the exact value the test asked for, not the 120 BPM default.
        REQUIRE (observation.bpm == bpm);

        // Position, from first principles. EXACT equality is deliberate: the
        // transport re-derives PPQ from an int64 sample counter (Transport.h), so a
        // per-block float accumulator — which WOULD drift over 32 blocks — fails here.
        REQUIRE (observation.timeInSamples
                 == static_cast<std::int64_t> (i) * static_cast<std::int64_t> (rig.blockSize));
        REQUIRE (observation.ppqPosition == expectedPpqAtBlock (i, rig.blockSize, bpm, rig.sampleRate));

        // The 4/4 bar grid a synced plugin's dotted delay depends on.
        REQUIRE (observation.ppqOfLastBarStart == std::floor (observation.ppqPosition / 4.0) * 4.0);
    }

    // PPQ advanced by exactly one block's worth per block — stated independently of
    // the loop above so a constant-but-wrong-rate playhead cannot slip through.
    const double perBlockPpq = static_cast<double> (rig.blockSize) * ppqPerSampleFor (bpm, rig.sampleRate);
    REQUIRE (observations.back ().ppqPosition - observations.front ().ppqPosition
             == static_cast<double> (measuredBlocks - 1) * perBlockPpq);
}

TEST_CASE ("hosting/playhead: a wrapped hosted plugin observes a stopped transport", "[hosting-lab]")
{
    // The other half of "play-state": a plugin must be told when the host is NOT
    // playing (a synced LFO should not free-run), both before a first play and after
    // a stop.
    WrappedObserverRig rig;
    rig.warmUpAndReset ();

    // Never started: stopped, parked at PPQ 0.
    rig.render (4);
    for (const auto& observation : rig.fake->observations ())
    {
        REQUIRE (observation.hadPosition);
        REQUIRE (observation.isPlaying == false);
        REQUIRE (observation.ppqPosition == 0.0);
        REQUIRE (observation.timeInSamples == 0);
    }

    // Play, then stop. `transportStop` also rewinds (groovebox convention,
    // Transport.h), so the plugin must see BOTH the state change and the rewind.
    rig.push (cmd (EngineCommandType::transportPlay));
    rig.render (8);
    REQUIRE (rig.fake->lastObservation ().isPlaying);
    REQUIRE (rig.fake->lastObservation ().ppqPosition > 0.0);

    rig.push (cmd (EngineCommandType::transportStop));
    rig.render (4);

    REQUIRE (rig.fake->lastObservation ().isPlaying == false);
    REQUIRE (rig.fake->lastObservation ().ppqPosition == 0.0);
    REQUIRE (rig.fake->lastObservation ().timeInSamples == 0);
    REQUIRE (rig.fake->nullPlayHeadBlocks () == 0);
}

TEST_CASE ("hosting/playhead: the same absolute sample yields the same PPQ at any block size", "[hosting-lab]")
{
    // Phase 5.3's buffer-size independence, as seen THROUGH the wrapper. The playhead
    // must report the BLOCK-START position, so absolute sample 2048 carries one PPQ
    // value no matter whether the blocks were 64 or 256 samples long. If the playhead
    // ever reported a block-END or mid-block position, the two sizes would disagree.
    constexpr double bpm = 132.0;
    constexpr std::int64_t targetSample = 2048; // divisible by both sizes below

    double reference = -1.0;

    for (const int blockSize : { 64, 256 })
    {
        WrappedObserverRig rig (kGraphSampleRate, blockSize);
        rig.warmUpAndReset ();

        rig.push (cmdDouble (EngineCommandType::setTempoBpm, bpm));
        rig.push (cmd (EngineCommandType::transportPlay));

        const auto blockAtTarget = static_cast<int> (targetSample / blockSize);
        rig.render (blockAtTarget + 1); // +1 so index `blockAtTarget` exists

        const auto& observation = rig.fake->observations ()[static_cast<std::size_t> (blockAtTarget)];
        INFO ("block size " << blockSize << ", observation ppq " << observation.ppqPosition);

        REQUIRE (observation.timeInSamples == targetSample);
        REQUIRE (observation.ppqPosition
                 == expectedPpqAtBlock (blockAtTarget, blockSize, bpm, kGraphSampleRate));

        if (reference < 0.0)
            reference = observation.ppqPosition;
        else
            REQUIRE (observation.ppqPosition == reference); // bit-identical across sizes
    }

    REQUIRE (reference > 0.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Regression guard for the wrapper's forwarding override
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("hosting/playhead: HostedPluginNode forwards the graph playhead to its inner "
           "instance (fails if the setPlayHead override is removed)",
           "[hosting-lab]")
{
    // NAMED FOR ITS INTENT because the behavior it protects is ONE LINE —
    // `inner->setPlayHead (newPlayHead)` in HostedPluginNode::setPlayHead. Delete
    // that line and this case fails while every other hosting test stays green: the
    // graph sets the playhead on the WRAPPER, so a wrapper that does not forward
    // leaves the actual plugin permanently playhead-less, and every tempo-synced LFO
    // and delay in every hosted plugin silently free-runs.
    //
    // Verified fails-without / passes-with by temporarily removing the forwarding
    // line locally (see the task report).
    constexpr double bpm = 150.0;

    WrappedObserverRig rig;
    rig.warmUpAndReset ();

    rig.push (cmdDouble (EngineCommandType::setTempoBpm, bpm));
    rig.push (cmd (EngineCommandType::transportPlay));
    rig.render (8);

    REQUIRE (rig.fake->blocksRendered () == 8);

    // Step 1 — the graph DID reach the wrapper. This isolates the failure: if this
    // passes and the inner assertions below fail, the bug is the wrapper's
    // forwarding, not the graph's playhead installation.
    REQUIRE (rig.node->getPlayHead () != nullptr);

    // Step 2 — the wrapper forwarded the SAME playhead object down to the instance it
    // owns. Pointer identity is the tightest statement of the §6.3 contract.
    REQUIRE (rig.fake->getPlayHead () != nullptr);
    REQUIRE (rig.fake->getPlayHead () == rig.node->getPlayHead ());

    // Step 3 — and it was live for EVERY rendered block, carrying real transport
    // state (not merely non-null once at the end).
    REQUIRE (rig.fake->nullPlayHeadBlocks () == 0);
    REQUIRE (rig.fake->missingPositionBlocks () == 0);
    REQUIRE (rig.fake->lastObservation ().bpm == bpm);
    REQUIRE (rig.fake->lastObservation ().isPlaying);
    REQUIRE (rig.fake->lastObservation ().ppqPosition
             == expectedPpqAtBlock (7, rig.blockSize, bpm, rig.sampleRate));
}

TEST_CASE ("hosting/playhead: a tempo change reaches the wrapped plugin on the next block", "[hosting-lab]")
{
    // A synced plugin must see a tempo change promptly and land on a block boundary
    // (INSTRUCTIONS 5.1). The command is drained by the head node before any node
    // renders, so the block that consumed it already reports the NEW bpm, and PPQ
    // continues from where the old tempo left it (a tempo change moves the rate, not
    // the position).
    constexpr double firstBpm = 100.0;
    constexpr double secondBpm = 200.0;
    constexpr int firstLeg = 12;
    constexpr int secondLeg = 12;

    WrappedObserverRig rig;
    rig.warmUpAndReset ();

    rig.push (cmdDouble (EngineCommandType::setTempoBpm, firstBpm));
    rig.push (cmd (EngineCommandType::transportPlay));
    rig.render (firstLeg);

    const double ppqAtChange = expectedPpqAtBlock (firstLeg, rig.blockSize, firstBpm, rig.sampleRate);
    REQUIRE (rig.fake->observations ()[firstLeg - 1].bpm == firstBpm);

    rig.push (cmdDouble (EngineCommandType::setTempoBpm, secondBpm));
    rig.render (secondLeg);

    const double secondRate = ppqPerSampleFor (secondBpm, rig.sampleRate);
    const auto& observations = rig.fake->observations ();
    REQUIRE (static_cast<int> (observations.size ()) == firstLeg + secondLeg);

    // The block that consumed the change: new tempo, position unchanged by the change.
    REQUIRE (observations[firstLeg].bpm == secondBpm);
    REQUIRE (observations[firstLeg].ppqPosition == ppqAtChange);

    // ...and from there PPQ advances at the NEW rate.
    for (int i = 0; i < secondLeg; ++i)
    {
        const auto& observation = observations[static_cast<std::size_t> (firstLeg + i)];
        INFO ("second-leg block " << i << " ppq " << observation.ppqPosition);
        REQUIRE (observation.bpm == secondBpm);
        REQUIRE (observation.ppqPosition
                 == ppqAtChange + static_cast<double> (static_cast<std::int64_t> (i) * rig.blockSize) * secondRate);
    }
}
