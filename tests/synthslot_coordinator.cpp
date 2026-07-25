// ─────────────────────────────────────────────────────────────────────────────
// synthslot_coordinator — SynthSlot message-thread coordinator unit tests
// (issue #22: coordinator had ZERO coverage; issue #24: bounded fade-out
// regression). ARCHITECTURE §6.2, §6.3; INSTRUCTIONS Phase 4.3; .claude/rules/
// testing.md.
//
// SynthSlot drives the engine through the GUI-free app/ISynthEngine.h seam, so
// these tests inject a lightweight FakeSynthEngine (records setSynth/removeSynth/
// allNotesOff, owns the handed-off node, reports a configurable SR/block) instead
// of the concrete AudioEngine (which drags in juce_audio_utils/devices). Plugin
// instantiation travels the REAL async path (PluginInstantiator ← FakePluginFormat
// ← the baseline FakePluginInstance corpus); no real third-party plugin is ever
// created (.claude/rules/testing.md).
//
// DETERMINISM: no wall-clock and no real audio device drive behaviour. The message
// loop is pumped in bounded slices only as a hang guard (the deadline is a timeout,
// not a synchronisation primitive — the same rationale as HostingLabSupport's
// pumpUntil). The swap/remove state machine is advanced by explicit poll() calls,
// and the #24 budget is exercised by counting polls — never by sleeping.
// ─────────────────────────────────────────────────────────────────────────────

#include "ISynthEngine.h"
#include "SynthSlot.h"

#include "fakes/FakePluginFormat.h"
#include "fakes/FakePlugins.h"
#include "fakes/HostingLabSupport.h"

#include "hosting/HostedPluginNode.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>

using namespace arpbox::testing;

namespace
{
// ── Fake engine seam ─────────────────────────────────────────────────────────
// GUI-free ISynthEngine test double. OWNS the node SynthSlot hands to setSynth
// (production: the graph owns it) so SynthSlot's non-owning `currentSynth` raw
// pointer stays valid; a subsequent setSynth/removeSynth frees the previous node
// on the message thread, matching the real graph swap/remove behaviour. Records
// call counts + the currently-held node for the assertions below.
class FakeSynthEngine final : public arpbox::app::ISynthEngine
{
public:
    FakeSynthEngine (double sr, int block)
        : sampleRate (sr)
        , blockSize (block)
    {
    }

    // MESSAGE-THREAD ONLY.
    void setSynth (std::unique_ptr<juce::AudioProcessor> synth) override
    {
        ++setSynthCalls;
        current = std::move (synth); // frees any previous node (message-thread).
    }

    void removeSynth () override
    {
        ++removeSynthCalls;
        current.reset ();
    }

    void allNotesOff () override { ++allNotesOffCalls; }

    double getCurrentSampleRate () const noexcept override { return sampleRate; }
    int getCurrentBlockSize () const noexcept override { return blockSize; }

    // ── Probes ───────────────────────────────────────────────────────────────
    juce::AudioProcessor* currentNode () const noexcept { return current.get (); }
    juce::String currentName () const { return current != nullptr ? current->getName () : juce::String {}; }
    int setSynthCount () const noexcept { return setSynthCalls; }
    int removeSynthCount () const noexcept { return removeSynthCalls; }
    int allNotesOffCount () const noexcept { return allNotesOffCalls; }

    double sampleRate;
    int blockSize;

private:
    std::unique_ptr<juce::AudioProcessor> current;
    int setSynthCalls = 0;
    int removeSynthCalls = 0;
    int allNotesOffCalls = 0;
};

// ── Test fixture ─────────────────────────────────────────────────────────────
// Stands up JUCE (for async delivery), a format manager carrying the fake corpus,
// and the fake engine. `juceInit` is declared first so it outlives every member's
// destruction (the held HostedPluginNode must free while JUCE is alive).
struct SlotFixture
{
    explicit SlotFixture (double sr = 48000.0, int block = 128)
        : engine (sr, block)
    {
        auto fmt = std::make_unique<FakePluginFormat> ();
        format = fmt.get ();
        formats.addFormat (std::move (fmt));
    }

    MessageScope juceInit;
    juce::AudioPluginFormatManager formats;
    FakePluginFormat* format = nullptr;
    FakeSynthEngine engine;
};

// Pumps the message loop in bounded slices until `pred` is true or `timeoutMs`
// elapses (hang guard). Returns pred's final value. Message thread only.
template <typename Predicate>
bool pumpUntilPred (Predicate&& pred, int timeoutMs = 5000)
{
    const auto deadline = juce::Time::getMillisecondCounter () + static_cast<juce::uint32> (timeoutMs);

    while (! pred ())
    {
        if (juce::Time::getMillisecondCounter () >= deadline)
            return false;

        juce::MessageManager::getInstance ()->runDispatchLoopUntil (20);
    }

    return true;
}

juce::PluginDescription descFor (FakeBehavior behavior)
{
    return makeDescription (specFor (behavior));
}

juce::String nameOf (FakeBehavior behavior)
{
    return juce::String (specFor (behavior).name);
}

// A description a registered format cannot instantiate (createFailed fail-path):
// fake-looking identifier ⇒ passes fileMightContainThisPluginType ⇒ reaches the
// format's own creation-failure branch.
juce::PluginDescription badDescription ()
{
    juce::PluginDescription d;
    d.pluginFormatName = kFakeFormatName;
    d.name = "Ghost";
    d.fileOrIdentifier = "arpbox.fake.does-not-exist";
    d.uniqueId = 0x0FA0EDED;
    d.deprecatedUid = 0x0FA0EDED;
    return d;
}

// The #24 fade-out poll budget. Mirrors the private SynthSlot::kMaxFadeOutPolls;
// the boundedness tests assert the swap/remove completes at exactly this count.
constexpr int kFadeOutBudget = 30;
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1. First load
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE ("synthslot/load: first load inserts exactly one prepared synth (#22)", "[unit]")
{
    SlotFixture fx; // real 48000/128 config
    arpbox::app::SynthSlot slot (fx.engine, fx.formats);

    slot.load (descFor (FakeBehavior::baseline));
    REQUIRE (pumpUntilPred ([&] { return ! slot.isPending (); }));

    REQUIRE (slot.isLoaded ());
    REQUIRE (slot.getCurrentSynthName () == nameOf (FakeBehavior::baseline));
    REQUIRE (slot.getLastError ().isEmpty ());
    REQUIRE (fx.engine.setSynthCount () == 1);
    REQUIRE (fx.engine.removeSynthCount () == 0);
    REQUIRE (fx.engine.currentNode () != nullptr);
    REQUIRE (fx.engine.currentName () == nameOf (FakeBehavior::baseline));
    REQUIRE (fx.engine.allNotesOffCount () == 0); // first load ⇒ no outgoing flush

    // Prepare-before-insertion at the engine's REAL reported SR/block (§6.2).
    auto* node = dynamic_cast<arpbox::hosting::HostedPluginNode*> (fx.engine.currentNode ());
    REQUIRE (node != nullptr);
    auto* inner = dynamic_cast<FakePluginInstance*> (node->getWrappedInstance ());
    REQUIRE (inner != nullptr);
    REQUIRE (inner->getPreparedSampleRate () == 48000.0);
    REQUIRE (inner->getPreparedBlockSize () == 128);
}

// ─────────────────────────────────────────────────────────────────────────────
// W4 — the 0/0 "no device" config clamps the safe-instantiation prepare
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE ("synthslot/load: no-device 0/0 config clamps prepare to safe defaults (W4)", "[unit]")
{
    SlotFixture fx (0.0, 0); // engine reports no open device yet
    arpbox::app::SynthSlot slot (fx.engine, fx.formats);

    slot.load (descFor (FakeBehavior::baseline));
    REQUIRE (pumpUntilPred ([&] { return ! slot.isPending (); }));
    REQUIRE (slot.isLoaded ());

    auto* node = dynamic_cast<arpbox::hosting::HostedPluginNode*> (fx.engine.currentNode ());
    REQUIRE (node != nullptr);
    auto* inner = dynamic_cast<FakePluginInstance*> (node->getWrappedInstance ());
    REQUIRE (inner != nullptr);
    // 0 SR / 0 block would crash real plugins that divide by SR in prepareToPlay;
    // SynthSlot clamps to 44100/512 for the seed prepare.
    REQUIRE (inner->getPreparedSampleRate () == 44100.0);
    REQUIRE (inner->getPreparedBlockSize () == 512);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Generation-counter invalidation (#22, successor of #16)
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE ("synthslot/load: a superseding load drops the stale result and keeps the newest (#22)", "[unit]")
{
    SlotFixture fx;
    arpbox::app::SynthSlot slot (fx.engine, fx.formats);

    slot.load (descFor (FakeBehavior::baseline));        // A
    slot.load (descFor (FakeBehavior::stateCorrupting)); // B supersedes A before either lands

    // Settle: !isPending is reached only once BOTH callbacks have been processed
    // (the winner inserted at idle, the loser dropped). Order-independent.
    REQUIRE (pumpUntilPred ([&] { return ! slot.isPending (); }));

    // Superseded A dropped as stale; its prepared instance destructs on the message
    // thread as onInstantiated unwinds — the JUCE leak detector validates no leak.
    REQUIRE (slot.getDroppedAsStaleCount () == 1);
    REQUIRE (slot.isLoaded ());
    REQUIRE (slot.getCurrentSynthName () == nameOf (FakeBehavior::stateCorrupting)); // B is live
    REQUIRE (fx.engine.currentName () == nameOf (FakeBehavior::stateCorrupting));
    REQUIRE (fx.engine.setSynthCount () == 1); // only B reached the engine; A never did
    REQUIRE (fx.engine.removeSynthCount () == 0);
    REQUIRE (slot.getLastError ().isEmpty ());
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Remove while a load is pending
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE ("synthslot/remove: remove before the load callback drops the pending load (#22)", "[unit]")
{
    SlotFixture fx;
    arpbox::app::SynthSlot slot (fx.engine, fx.formats);

    slot.load (descFor (FakeBehavior::baseline));
    slot.remove (); // bumps generation ⇒ invalidates the in-flight load

    REQUIRE (pumpUntilPred ([&] { return ! slot.isPending (); }));

    REQUIRE (slot.getDroppedAsStaleCount () == 1);
    REQUIRE_FALSE (slot.isLoaded ());
    REQUIRE_FALSE (slot.isPending ());
    REQUIRE (fx.engine.setSynthCount () == 0);    // nothing was ever inserted
    REQUIRE (fx.engine.removeSynthCount () == 0); // nothing to remove (never loaded)
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Teardown while pending (WeakReference guard) — the use-after-free guard
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE ("synthslot/teardown: destroying the slot neutralises an in-flight load (#22)", "[unit]")
{
    SlotFixture fx;
    const juce::String id = specFor (FakeBehavior::baseline).identifier;

    {
        arpbox::app::SynthSlot slot (fx.engine, fx.formats);
        slot.load (descFor (FakeBehavior::baseline));
        // Destroy the slot BEFORE pumping — the async instantiation is still queued.
    }

    // The late instantiation still runs (proves the guard is genuinely exercised,
    // not a vacuous pass): the instance is created...
    REQUIRE (pumpUntilPred ([&] { return fx.format->getCreateInstanceCallCount (id) >= 1; }));

    // ...and drain any trailing async so the completion callback runs to completion
    // and its prepared instance frees on the message thread. Bounded (hang guard).
    std::atomic<bool> never { false };
    pumpUntil (never, 100);

    // The callback no-op'd via the WeakReference guard: it never touched the engine
    // (no use-after-free). ASan/leak-detector confirm the freed slot was not read.
    REQUIRE (fx.engine.setSynthCount () == 0);
    REQUIRE (fx.engine.currentNode () == nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5a. #24 — bounded fade-out on SWAP with audio NOT running
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE ("synthslot/swap: fade-out wait is bounded when audio is stopped (#24)", "[unit]")
{
    SlotFixture fx;
    arpbox::app::SynthSlot slot (fx.engine, fx.formats);
    const juce::String idB = specFor (FakeBehavior::stateCorrupting).identifier;

    // Load A and settle.
    slot.load (descFor (FakeBehavior::baseline));
    REQUIRE (pumpUntilPred ([&] { return ! slot.isPending (); }));
    REQUIRE (slot.isLoaded ());
    auto* nodeA = fx.engine.currentNode ();
    REQUIRE (nodeA != nullptr);

    // Load B ⇒ stages a swap; A begins fading out (state = awaitingFadeOut).
    slot.load (descFor (FakeBehavior::stateCorrupting));
    REQUIRE (pumpUntilPred ([&] { return fx.format->getCreateInstanceCallCount (idB) >= 1; }));

    // A still live, swap staged and pending, stuck-note flush issued on the swap.
    REQUIRE (slot.isPending ());
    REQUIRE (fx.engine.currentNode () == nodeA);
    REQUIRE (fx.engine.setSynthCount () == 1);
    REQUIRE (fx.engine.allNotesOffCount () == 1);

    // The fake engine runs no processBlock, so HostedPluginNode::isFadeOutComplete()
    // never turns true — WITHOUT the #24 budget this would poll forever (hang).
    // Count polls until the swap is forced through; it must terminate at exactly the
    // budget. The hard cap turns a regression (infinite wait) into a test failure
    // rather than a hung suite.
    int polls = 0;
    constexpr int kHardCap = 500;
    while (slot.isPending () && polls < kHardCap)
    {
        REQUIRE (fx.engine.currentNode () == nodeA); // still A, not yet swapped
        REQUIRE (fx.engine.setSynthCount () == 1);
        slot.poll ();
        ++polls;
    }

    REQUIRE (polls == kFadeOutBudget); // bounded — never hangs (#24)
    REQUIRE_FALSE (slot.isPending ());
    REQUIRE (fx.engine.setSynthCount () == 2); // B inserted on the budget poll
    REQUIRE (fx.engine.currentNode () != nodeA);
    REQUIRE (slot.getCurrentSynthName () == nameOf (FakeBehavior::stateCorrupting));
    REQUIRE (slot.isLoaded ());
}

// ─────────────────────────────────────────────────────────────────────────────
// 5b. #24 — bounded fade-out on REMOVE with audio NOT running
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE ("synthslot/remove: fade-out wait is bounded when audio is stopped (#24)", "[unit]")
{
    SlotFixture fx;
    arpbox::app::SynthSlot slot (fx.engine, fx.formats);

    slot.load (descFor (FakeBehavior::baseline));
    REQUIRE (pumpUntilPred ([&] { return ! slot.isPending (); }));
    REQUIRE (slot.isLoaded ());

    slot.remove (); // A begins fading out; no processBlock ⇒ never reports silent
    REQUIRE (slot.isPending ());
    REQUIRE (fx.engine.allNotesOffCount () == 1); // stuck-note guard before removal

    int polls = 0;
    constexpr int kHardCap = 500;
    while (slot.isPending () && polls < kHardCap)
    {
        REQUIRE (slot.isLoaded ()); // still present until the budget fires
        REQUIRE (fx.engine.removeSynthCount () == 0);
        slot.poll ();
        ++polls;
    }

    REQUIRE (polls == kFadeOutBudget); // bounded — never hangs (#24)
    REQUIRE_FALSE (slot.isPending ());
    REQUIRE_FALSE (slot.isLoaded ());
    REQUIRE (fx.engine.removeSynthCount () == 1);
    REQUIRE (fx.engine.currentNode () == nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. Failure isolation — a bad load never drops the good synth
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE ("synthslot/load: an instantiation failure is isolated and keeps the current synth (#22)", "[unit]")
{
    SlotFixture fx;
    arpbox::app::SynthSlot slot (fx.engine, fx.formats);

    // A good synth first.
    slot.load (descFor (FakeBehavior::baseline));
    REQUIRE (pumpUntilPred ([&] { return ! slot.isPending (); }));
    REQUIRE (slot.isLoaded ());
    auto* good = fx.engine.currentNode ();
    REQUIRE (good != nullptr);

    // Then a description the fake format fails to instantiate.
    slot.load (badDescription ());
    REQUIRE (pumpUntilPred ([&] { return ! slot.isPending (); }));

    REQUIRE (slot.getLastError ().isNotEmpty ()); // typed failure surfaced
    REQUIRE (slot.isLoaded ());                   // good synth untouched
    REQUIRE (fx.engine.currentNode () == good);
    REQUIRE (slot.getCurrentSynthName () == nameOf (FakeBehavior::baseline));
    REQUIRE (fx.engine.setSynthCount () == 1); // no second insert
    REQUIRE (fx.engine.removeSynthCount () == 0);
}
