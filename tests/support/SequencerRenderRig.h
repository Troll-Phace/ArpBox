#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// SequencerRenderRig — a headless `Transport` + `SequencerProcessor` pair driven
// exactly the way the graph drives them, plus the scaffold-grid arithmetic the
// Phase 5.3 tests assert against (INSTRUCTIONS Phase 5.3c/5.3e; ARCHITECTURE §4
// step 2, §5.5).
//
// WHY NOT DRIVE THE WHOLE EngineGraph. The graph path is exercised too (see the
// graph-level flush case in sequencer_node.cpp, which is what
// `EngineGraph::getSequencer()` exists for), but the buffer-size sweep needs to run
// the SAME musical timeline at nine different block sizes and two sample rates. A
// graph must be re-prepared for each, drags the master/limiter/tone nodes and an
// async synth insertion along with it, and mixes the sequencer's output into a
// buffer the caller cannot see. This rig is the two objects under test and nothing
// else, so a byte difference between two renders can only have come from them.
//
// THE ORDERING THIS RIG PRESERVES is the load-bearing part: `TransportProcessor`
// (the graph head node) drains the command queue and calls `Transport::beginBlock`
// BEFORE any other node renders, which is what makes a command land in the very
// block it was pushed for and every tempo change land on a block boundary. So
// `renderSequencer`'s per-block hook applies the scheduled commands and THEN calls
// `beginBlock`, and only then does `renderProcessor` call the sequencer. Get that
// order wrong and a stop would flush one block late.
//
// BLOCK ALIGNMENT IS A PRECONDITION, NOT A DETAIL. Commands are consumed at block
// HEADS, so a command scheduled at a sample that is not a multiple of the block size
// lands at a DIFFERENT absolute sample at each block size — which would make a
// cross-block-size comparison fail for a reason that has nothing to do with the
// sequencer. `scheduleIsBlockAligned()` exists so the sweep asserts that precondition
// out loud instead of assuming it.
// ─────────────────────────────────────────────────────────────────────────────

#include "support/MidiRenderHarness.h"

#include "engine/graph/EngineCommand.h"
#include "engine/graph/Transport.h"
#include "engine/sequencer/PatternChannel.h"
#include "engine/sequencer/PatternDocument.h"
#include "engine/sequencer/SequencerProcessor.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace arpbox::testing
{
// ─────────────────────────────────────────────────────────────────────────────
// Scaffold-grid arithmetic (mirrors SequencerProcessor's constants, derives nothing
// from its implementation)
// ─────────────────────────────────────────────────────────────────────────────

/** Samples per scaffold step (a 16th note) at `bpm` / `sampleRate`. Computed from
    the musical definition, not from the node, so it is an independent check:
    `stepPpq * secondsPerQuarter * sampleRate`. */
inline double samplesPerScaffoldStep (double bpm, double sampleRate) noexcept
{
    return engine::SequencerProcessor::scaffoldStepPpq * (60.0 / bpm) * sampleRate;
}

/** Scaffold gate length in samples, as the node resolves it (LEN-shaped fraction of
    the step, rounded, minimum one sample). */
inline std::int64_t scaffoldGateSamples (double bpm, double sampleRate) noexcept
{
    const double length = engine::SequencerProcessor::scaffoldGateFraction * samplesPerScaffoldStep (bpm, sampleRate);
    const auto rounded = static_cast<std::int64_t> (std::llround (length));
    return rounded < 1 ? 1 : rounded;
}

/** Step boundaries inside the half-open sample span `[0, spanSamples)`.

    Boundaries sit at PPQ `n * stepPpq` for n = 0, 1, 2, …, and a boundary belongs to
    the span iff its PPQ is strictly less than the span's exclusive end PPQ — so the
    count is `ceil(endPpq / stepPpq)`, which is also correct (and exact) when the span
    ends precisely on a boundary, because that final boundary is excluded. */
inline int expectedScaffoldSteps (std::int64_t spanSamples, double bpm, double sampleRate) noexcept
{
    const double endPpq = static_cast<double> (spanSamples) * (bpm / (60.0 * sampleRate));
    return static_cast<int> (std::ceil (endPpq / engine::SequencerProcessor::scaffoldStepPpq));
}

// ─────────────────────────────────────────────────────────────────────────────
// Command helpers
// ─────────────────────────────────────────────────────────────────────────────

/** A payload-free engine command (`transportPlay` / `transportStop`). */
inline engine::EngineCommand engineCommand (engine::EngineCommandType type) noexcept
{
    engine::EngineCommand command {};
    command.type = type;
    return command;
}

/** An engine command carrying a double payload (`transportLocate` / `setTempoBpm`). */
inline engine::EngineCommand engineCommand (engine::EngineCommandType type, double value) noexcept
{
    engine::EngineCommand command {};
    command.type = type;
    command.value.d = value;
    return command;
}

/** A `queuePatternSwitch` command (§5.2 quantized apply, §6.1): `targetId` carries the
    destination pattern index and `value.u` the `QuantizeMode` ordinal, exactly as
    `SequencerProcessor::applyCommand` decodes them. */
inline engine::EngineCommand patternSwitchCommand (int patternIndex, engine::QuantizeMode quantize) noexcept
{
    engine::EngineCommand command {};
    command.type = engine::EngineCommandType::queuePatternSwitch;
    command.targetId = static_cast<std::uint16_t> (patternIndex);
    command.value.u = static_cast<std::uint32_t> (quantize);
    return command;
}

/** A command to apply at the head of the block containing `atSample`. */
struct ScheduledCommand
{
    std::int64_t atSample = 0;     ///< Absolute sample the command should land on.
    engine::EngineCommand command; ///< What to apply.
};

/** True when every scheduled command sits exactly on a `blockSize` boundary — the
    precondition for comparing renders across block sizes (see the header note). */
inline bool scheduleIsBlockAligned (const std::vector<ScheduledCommand>& schedule, int blockSize) noexcept
{
    if (blockSize <= 0)
        return false;

    for (const auto& entry : schedule)
        if (entry.atSample < 0 || entry.atSample % static_cast<std::int64_t> (blockSize) != 0)
            return false;

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// The rig
// ─────────────────────────────────────────────────────────────────────────────

/** A prepared `Transport` + `PatternDocument` + `PatternChannel` +
    `SequencerProcessor` set, wired as the graph wires them. All members are public:
    tests drive the transport directly (that IS the head node's job), edit the
    document (which republishes automatically), and read the sounding-note table
    straight off the node.

    ── WHY THE DOCUMENT AND CHANNEL ARE HERE ───────────────────────────────────
    Since Phase 6 the node plays the adopted `PatternSnapshot`, not a hardcoded
    pattern, so a rig without a publisher would render pure silence. The
    DEFAULT-CONSTRUCTED document reproduces the Phase-5 scaffold exactly (see the
    ctor note in PatternDocument.h), which is why the timing and lifecycle suites
    written against `scaffold*` still hold with no change to their expectations.

    MEMBER ORDER IS A LIFETIME CONTRACT, the same one `EngineGraph` documents:
    channel, then document (it points at the channel), then transport, then the node
    (it points at both the transport and the channel, and hands its held snapshot
    back to the channel in its destructor). Destruction is reverse-declaration, so
    the node dies first and the channel last. */
struct SequencerRig
{
    /** Prepares everything at `sampleRate` / `blockSize`, primes the channel with one
        snapshot and injects the shared state, exactly as `EngineGraph::buildGraph`
        does — publish BEFORE the first block, or block 0 has nothing to adopt. */
    SequencerRig (double sampleRate, int blockSize)
    {
        transport.prepare (sampleRate);

        patternDocument.setPublishTarget (&patternChannel);
        patternDocument.publishTo (patternChannel);

        sequencer.setSharedState (&transport, &patternChannel);
        sequencer.prepareToPlay (sampleRate, blockSize);
    }

    /** THE COMMAND FAN-OUT, exactly as `EngineGraph` wires it
        (`transportProcessor->setCommandSinks ({ &transport, master, sequencer, … })`):
        every sink sees every command and ignores what it does not own, and the
        TRANSPORT IS FIRST because it is the only sink whose state the others read.

        Without this the rig could only deliver the four transport commands, and
        `queuePatternSwitch` — which `SequencerProcessor` consumes as an `ICommandSink`,
        not through any document edit — would be untestable at rig level. Transport
        commands behave exactly as before: `SequencerProcessor::applyCommand` falls
        through every type it does not own. */
    void applyCommand (const engine::EngineCommand& command) noexcept
    {
        transport.applyCommand (command);
        sequencer.applyCommand (command);
    }

    /** Head-node emulation for a single block: latch the transport, then render the
        sequencer into `midi` (which the caller clears/pre-sizes — the node never
        clears it, because it carries live THRU MIDI in the real graph). */
    void renderBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
    {
        transport.beginBlock (audio.getNumSamples ());
        sequencer.processBlock (audio, midi);
    }

    // Declaration order is load-bearing — see the note above.
    engine::PatternChannel patternChannel;   ///< §3.4 mechanism 3: publish / adopt / retire.
    engine::PatternDocument patternDocument; ///< Message-thread model; republishes on every edit.
    engine::Transport transport;             ///< The musical clock (drive it as the head node would).
    engine::SequencerProcessor sequencer;    ///< The node under test.
};

/** Renders `config.numBlocks` blocks of `rig`, applying each scheduled command at the
    head of the block that contains its sample — command drain, then `beginBlock`,
    then the node — and collecting every emitted event at its ABSOLUTE sample
    position.

    The rig is NOT reset: successive calls continue the same musical timeline (only
    the returned result's sample origin restarts at 0), which is how the flush tests
    render "up to a note sounding" and then "the block with the stop in it". */
inline MidiRenderResult
renderSequencer (SequencerRig& rig, const MidiRenderConfig& config, const std::vector<ScheduledCommand>& schedule = {})
{
    return renderProcessor (rig.sequencer,
                            config,
                            [&] (const RenderBlockContext& context)
                            {
                                const std::int64_t blockEnd =
                                    context.blockBase + static_cast<std::int64_t> (context.numSamples);

                                for (const auto& entry : schedule)
                                    if (entry.atSample >= context.blockBase && entry.atSample < blockEnd)
                                        rig.applyCommand (entry.command);

                                rig.transport.beginBlock (context.numSamples);
                            });
}
} // namespace arpbox::testing
