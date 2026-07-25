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
      - Implementors MUST IGNORE every command type they do not own — a `default:
        break;` in the switch. Fan-out means every sink sees every command; a sink
        that reacts to a type it does not own would corrupt another sink's state.
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
