#pragma once

#include "../EngineGuiGuard.h"

namespace arpbox::engine
{
struct EngineCommand;

/** Interface for any engine object that consumes UI→engine commands
    (ARCHITECTURE §3.4, channel 1; §4, step 1).

    WHY AN INTERFACE: `EngineCommandQueue` is strict SPSC — it may have exactly
    ONE consumer. From Phase 5 that consumer is the transport head node
    (`TransportProcessor`), which drains the queue ONCE at the top of its
    `processBlock` and fans every drained command out to the registered sinks. A
    second drainer anywhere in the graph would be a correctness bug (each command
    would reach only one of the two consumers, non-deterministically).

    DISPATCH CONTRACT:
      - `applyCommand` is called on the AUDIO thread, from the transport head
        node's drain loop, at the head of the block and BEFORE any other graph
        node renders. Every sink therefore sees the command in time to affect the
        SAME block.
      - Sinks are fanned out in registration order for each command, and commands
        are delivered in producer (FIFO) order.
      - Implementors MUST IGNORE every command type they do not own. Fan-out means
        every sink sees every command; a sink that reacts to a type it does not own
        would corrupt another sink's state.
      - HOW to ignore them is part of the contract, not a style preference: list
        every unowned enumerator EXPLICITLY in one grouped no-op arm
        (`case a: case b: … break;`) as the LAST arm of the switch, and write NO
        `default:`. This says "ignored" exactly as loudly as `default: break;` did
        and behaves identically at runtime — a `type` value outside the enum matches
        no arm and leaves the switch untouched, which is the same no-op — while
        making a NEW `EngineCommandType` a compile diagnostic at every dispatch
        site: `-Wswitch-enum` (via `juce_recommended_warning_flags`) and, because
        there is no `default:`, `-Wswitch` (inside `-Wall`) as well. Two independent
        flags, and `.claude/skills/run-lint/lint.sh warnings` fails the build on
        either.
        WHY THAT MATTERS ENOUGH TO MANDATE THE LONGER ARM: adding an enumerator to
        `EngineCommandType` is exactly the moment someone must decide, per sink,
        whether that sink cares — and getting it wrong is a silent bug (Phase 7's
        `setFillHeld` fanned out to two sinks that had never considered it, which is
        also how #70/#79 stayed invisible until a review gate found them). A
        `default:` arm makes that decision for you, invisibly and always "no". The
        grouped arm forces the author to visit each sink and write the answer down.
        The arm is long and gets longer; that growth IS the reminder, and the cost
        of not having it was #70/#79.
      - Implementors MUST be RT-safe: no allocation, no locks, no I/O, no logging,
        no `juce::String`. Hence the `noexcept` on the signature.

    Sinks are non-owning observers registered on the message thread before the
    graph runs (see `TransportProcessor::setCommandSinks`) and must outlive the
    node that dispatches to them. */
class ICommandSink
{
public:
    /** ~ICommandSink. Virtual: sinks are held by non-owning base pointers. */
    virtual ~ICommandSink () = default;

    // RT-SAFE: audio thread, called from the transport head node's command drain
    // at the top of the block. Must be allocation-free, lock-free and must ignore
    // any command type this sink does not own.
    /** Applies one drained command, or ignores it if its type is not owned here. */
    virtual void applyCommand (const EngineCommand& command) noexcept = 0;
};
} // namespace arpbox::engine
