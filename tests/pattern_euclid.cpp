// ─────────────────────────────────────────────────────────────────────────────
// pattern_euclid — Phase 6.4: the euclidean GATE-lane generator's pinned
// musical semantics (ARCHITECTURE §5.1 L1 "Euclidean generator (steps/pulses/
// rotate) writes the GATE lane", §8.1 `euclid {steps,pulses,rotate}`).
//
// WHY [unit] AND NOT [determinism]. Phase 6 is establishing a labelling
// convention: [determinism] means equality against a FROZEN BYTE STREAM in
// tests/golden/, [midi-conformance] means a MIDI-semantic invariant, and [unit]
// means a value checked against a HAND-DERIVED expectation. Everything here is
// the third kind — every literal below was computed by hand from the necklace
// formula in Euclid.h and can be re-derived by a reader with a pencil. Phase 6.4
// then freezes these same rhythms into golden MIDI; when it does, THIS file is
// what says the goldens were baked from the right rhythm rather than merely from
// whatever the code happened to emit that day.
//
// ── ANTI-VACUITY ────────────────────────────────────────────────────────────
// A generator that returned all-zeros would satisfy a surprising number of weak
// euclid assertions (rotation is a no-op on a constant, "pulses == 0 ⇒ all off"
// holds trivially, and every "these two differ" test would have to be written as
// an inequality to catch it). So the spine of this file is the POPCOUNT SWEEP:
// over every (steps, pulses, rotate) in a wide range, the number of set entries
// must equal the CLAMPED pulse request exactly. Nothing constant, nothing
// truncated and nothing overflowing survives that. The literal necklaces then pin
// WHICH steps, and the rotation-relationship test pins the phase.
// ─────────────────────────────────────────────────────────────────────────────

#include "engine/sequencer/Euclid.h"
#include "engine/sequencer/PatternTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using arpbox::engine::EuclidParams;
using arpbox::engine::LaneId;
using arpbox::engine::LaneState;
using arpbox::engine::maxSteps;
using arpbox::engine::numLanes;
using arpbox::engine::PatternState;

namespace
{
/** `euclid::generate` into a right-sized vector, so a test reads as one line. */
std::vector<std::uint8_t> generate (int steps, int pulses, int rotate)
{
    // Always the full lane width, so an implementation that wrote PAST `steps`
    // would corrupt the tail rather than run off the end of the allocation — and
    // the tail is checked by `tailUntouched` below.
    std::vector<std::uint8_t> out (static_cast<std::size_t> (maxSteps), 0xEE);
    arpbox::engine::euclid::generate (steps, pulses, rotate, out.data ());
    return out;
}

/** The first `steps` entries of a generate() result, as the necklace proper. */
std::vector<std::uint8_t> necklace (int steps, int pulses, int rotate)
{
    auto full = generate (steps, pulses, rotate);
    full.resize (static_cast<std::size_t> (steps));
    return full;
}

/** `"x..x..x."` → `{1,0,0,1,0,0,1,0}`. Lets a rhythm be written the way a
    musician reads it, which is the whole point of pinning it as a literal. */
std::vector<std::uint8_t> pattern (const std::string& glyphs)
{
    std::vector<std::uint8_t> out;
    out.reserve (glyphs.size ());

    for (const char c : glyphs)
        out.push_back (c == 'x' ? std::uint8_t (1) : std::uint8_t (0));

    return out;
}

/** Rendered back to glyphs, so a failure prints `x.x.xx.x` and not 8 numbers. */
std::string describe (const std::vector<std::uint8_t>& steps)
{
    std::string out;

    for (const auto v : steps)
        out.push_back (v != 0 ? 'x' : '.');

    return out;
}

int pulseCount (const std::vector<std::uint8_t>& steps)
{
    int count = 0;

    for (const auto v : steps)
        if (v != 0)
            ++count;

    return count;
}

/** True when every entry from `steps` to the end still holds the 0xEE fill —
    i.e. `generate` wrote exactly `steps` entries and not one more. */
bool tailUntouched (const std::vector<std::uint8_t>& full, int steps)
{
    for (std::size_t i = static_cast<std::size_t> (steps); i < full.size (); ++i)
        if (full[i] != 0xEE)
            return false;

    return true;
}

/** Non-negative remainder, mirroring `positiveMod` in Euclid.cpp — but written
    independently here so the test does not inherit the very convention it checks. */
int wrap (int value, int m)
{
    const int r = value % m;
    return r < 0 ? r + m : r;
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE ("sequencer/euclid: the pinned necklaces are exactly the header's contract", "[unit]")
{
    // Each of these is spelled out in the "FROZEN CONTRACTS" block of Euclid.h and
    // re-derivable from out[i] = ((i * pulses) % steps) < pulses.

    SECTION ("E(3,8) puts a pulse on step 0")
    {
        // The user-pinned case. (0*3)%8=0<3 ✓ · (3*3)%8=1 ✓ · (6*3)%8=2 ✓.
        const auto got = necklace (8, 3, 0);
        INFO ("E(3,8,0) = " << describe (got) << ", expected x..x..x.");
        REQUIRE (got == pattern ("x..x..x."));
    }

    SECTION ("E(5,8) is x.x.xx.x — Bresenham's phase, NOT the textbook rotation")
    {
        // DO NOT "FIX" THIS AGAINST A PAPER. Bjorklund's answer is usually WRITTEN
        // x.xx.xx. — the same necklace (inter-onset intervals {2,2,2,1,1}) at a
        // different starting rotation. Euclid.h pins Bresenham's phase, which always
        // places a pulse on step 0 because that is what `rotate` is measured from.
        // Changing it is a determinism-contract break (§1.2/§5.2), not a bug fix.
        // See the "NOTE on E(5,8)" paragraph in engine/sequencer/Euclid.h.
        const auto got = necklace (8, 5, 0);
        INFO ("E(5,8,0) = " << describe (got) << ", expected x.x.xx.x (textbook writes x.xx.xx.)");
        REQUIRE (got == pattern ("x.x.xx.x"));

        // The two ARE the same necklace: the textbook phase is this one rotated.
        // Asserting the relationship (rather than only the literal) is what keeps
        // "different phase" from quietly becoming "different rhythm".
        REQUIRE (necklace (8, 5, 6) == pattern ("x.xx.xx."));
    }

    SECTION ("E(4,16) is the plain four-on-the-floor")
    {
        const auto got = necklace (16, 4, 0);
        INFO ("E(4,16,0) = " << describe (got));
        REQUIRE (got == pattern ("x...x...x...x..."));
    }

    SECTION ("E(7,16) — the dense 16-step necklace, at rest and rotated")
    {
        // No golden has been baked at the time of writing, so the rotation a golden
        // will eventually use is not yet a fact. Several are pinned instead, so
        // whichever one 6.4 picks is already covered here and a rotation-convention
        // flip is caught by THIS file rather than surfacing as an unexplained
        // golden diff months later.
        //
        // (i*7)%16 < 7 fires at i = 0,3,5,7,10,12,14.
        const auto got = necklace (16, 7, 0);
        INFO ("E(7,16,0) = " << describe (got));
        REQUIRE (got == pattern ("x..x.x.x..x.x.x."));

        REQUIRE (necklace (16, 7, 1) == pattern (".x..x.x.x..x.x.x"));
        REQUIRE (necklace (16, 7, 4) == pattern ("x.x.x..x.x.x..x."));
        REQUIRE (pulseCount (necklace (16, 7, 4)) == 7);
    }

    SECTION ("classic three-against-eight, both phases")
    {
        REQUIRE (necklace (8, 3, 1) == pattern (".x..x..x")); // header: E(3,8) rotate 1
        REQUIRE (necklace (16, 5, 0) == pattern ("x...x..x..x..x.."));
    }
}

TEST_CASE ("sequencer/euclid: positive rotate moves pulses later, and rotate is modular", "[unit]")
{
    SECTION ("positive rotate moves pulses LATER in the bar")
    {
        // The direction is the half of the convention a reader is most likely to get
        // backwards, so it is asserted as a DIRECTION and not only as a literal:
        // the first pulse of E(3,8) sits on step 0, and rotate 1 must move it to 1.
        const auto base = necklace (8, 3, 0);
        const auto moved = necklace (8, 3, 1);

        INFO ("base " << describe (base) << " → rotate 1 " << describe (moved));
        REQUIRE (base[0] == 1);
        REQUIRE (moved[0] == 0);
        REQUIRE (moved[1] == 1);
    }

    SECTION ("rotation genuinely rotates — the relationship, not just inequality")
    {
        // out[(i + rotate) % n] = base[i], i.e. rotated[j] == base[(j - rotate) mod n].
        // Asserting the RELATIONSHIP is what distinguishes a real rotation from a
        // generator that merely produces a different-looking necklace per rotate.
        int mismatches = 0;
        int combinations = 0;
        int comparedEntries = 0;

        for (int steps = 1; steps <= 32; ++steps)
        {
            for (int pulses = 0; pulses <= steps; ++pulses)
            {
                const auto base = necklace (steps, pulses, 0);

                for (int rotate = -2 * steps; rotate <= 2 * steps; ++rotate)
                {
                    ++combinations;
                    const auto rotated = necklace (steps, pulses, rotate);

                    for (int j = 0; j < steps; ++j)
                    {
                        ++comparedEntries;

                        if (rotated[static_cast<std::size_t> (j)] !=
                            base[static_cast<std::size_t> (wrap (j - rotate, steps))])
                            ++mismatches;
                    }
                }
            }
        }

        INFO ("swept " << combinations << " (steps, pulses, rotate) combinations, " << comparedEntries
                       << " entries compared");
        REQUIRE (mismatches == 0);

        // Counts derived from first principles, not copied from a run:
        //   combinations   = Σ(s=1..32) (s+1)(4s+1) = 4·Σs² + 5·Σs + 32
        //                  = 4(11440) + 5(528) + 32 = 48432
        //   comparedEntries = Σ(s=1..32) s(s+1)(4s+1) = 4·Σs³ + 5·Σs² + Σs
        //                  = 4(278784) + 5(11440) + 528 = 1172864
        REQUIRE (combinations == 48432);
        REQUIRE (comparedEntries == 1172864);
    }

    SECTION ("a rotation of a whole necklace is the identity")
    {
        for (const int steps : { 1, 5, 8, 16, 64 })
        {
            const int pulses = steps / 2;
            INFO ("steps " << steps);
            REQUIRE (necklace (steps, pulses, steps) == necklace (steps, pulses, 0));
            REQUIRE (necklace (steps, pulses, -steps) == necklace (steps, pulses, 0));
            REQUIRE (necklace (steps, pulses, 3 * steps) == necklace (steps, pulses, 0));
        }
    }

    SECTION ("negative rotate wraps: -1 == steps-1")
    {
        // C++ `%` keeps the dividend's sign (-1 % 8 == -1), so this is the guard on
        // Euclid.cpp's `positiveMod`. Without it, rotate -1 would index out of bounds.
        for (const int steps : { 2, 5, 8, 16, 64 })
        {
            INFO ("steps " << steps);
            REQUIRE (necklace (steps, 3, -1) == necklace (steps, 3, steps - 1));
            REQUIRE (necklace (steps, 3, -3) == necklace (steps, 3, steps - 3));
            REQUIRE (necklace (steps, 3, -(steps + 1)) == necklace (steps, 3, steps - 1));
        }

        REQUIRE (necklace (8, 3, -1) == pattern ("..x..x.x"));
    }

    SECTION ("negative-control: rotation is not a no-op")
    {
        // Guards the degenerate implementation the two tests above would otherwise
        // both accept — one that ignores `rotate` entirely (base == rotated for every
        // r, and rotated[j] == base[j - r] holds vacuously on a constant necklace).
        REQUIRE (necklace (8, 5, 0) != necklace (8, 5, 1));
        REQUIRE (necklace (16, 7, 0) != necklace (16, 7, 1));
        REQUIRE (necklace (8, 3, 1) != necklace (8, 3, 2));
    }
}

TEST_CASE ("sequencer/euclid: the pulse count is exactly the clamped request across the sweep", "[unit]")
{
    // THE ANTI-VACUITY SPINE OF THIS FILE. Every other euclid assertion here checks
    // WHICH steps fire; this one checks HOW MANY, over the whole input space, and it
    // is what an all-zeros / all-ones / truncating / overflowing generator dies on.
    //
    // It doubles as the clamp test: `pulses` outside [0, steps] must be CLAMPED (the
    // header's contract) rather than wrapped, saturated at the wrong end, or used to
    // index past the buffer.
    int wrongPopcount = 0;
    int tailWrites = 0;
    int combinations = 0;
    int firstBadSteps = 0;
    int firstBadPulses = 0;
    int firstBadRotate = 0;
    int firstBadCount = 0;

    for (int steps = 1; steps <= maxSteps; ++steps)
    {
        // Deliberately overshoots BOTH ends of the legal pulse range so the clamp is
        // exercised in the same sweep that checks the popcount.
        for (int pulses = -8; pulses <= steps + 8; ++pulses)
        {
            const int expected = pulses < 0 ? 0 : (pulses > steps ? steps : pulses);

            for (int rotate = -steps - 1; rotate <= steps + 1; ++rotate)
            {
                ++combinations;

                // No Catch2 macros in this loop: ~500k combinations, aggregated after.
                const auto full = generate (steps, pulses, rotate);
                const std::vector<std::uint8_t> head (full.begin (), full.begin () + steps);

                if (pulseCount (head) != expected)
                {
                    if (wrongPopcount == 0)
                    {
                        firstBadSteps = steps;
                        firstBadPulses = pulses;
                        firstBadRotate = rotate;
                        firstBadCount = pulseCount (head);
                    }

                    ++wrongPopcount;
                }

                if (! tailUntouched (full, steps))
                    ++tailWrites;
            }
        }
    }

    INFO ("swept " << combinations << " (steps, pulses, rotate) combinations");
    INFO ("first offender: steps " << firstBadSteps << " pulses " << firstBadPulses << " rotate " << firstBadRotate
                                   << " → " << firstBadCount << " pulses");
    REQUIRE (wrongPopcount == 0);
    REQUIRE (tailWrites == 0);
    // Derived, not captured: Σ(s=1..64) (s+17)(2s+3) = 2·Σs² + 37·Σs + 51·64
    //                       = 2(89440) + 37(2080) + 3264 = 259104.
    REQUIRE (combinations == 259104);
}

TEST_CASE ("sequencer/euclid: degenerate inputs fail safe", "[unit]")
{
    SECTION ("pulses == 0 ⇒ every step off")
    {
        for (const int steps : { 1, 3, 8, 16, 64 })
        {
            INFO ("steps " << steps);
            REQUIRE (pulseCount (necklace (steps, 0, 0)) == 0);
            REQUIRE (pulseCount (necklace (steps, 0, 5)) == 0);
            REQUIRE (pulseCount (necklace (steps, -1, 0)) == 0); // clamped, not wrapped
        }
    }

    SECTION ("pulses == steps ⇒ every step on")
    {
        for (const int steps : { 1, 3, 8, 16, 64 })
        {
            INFO ("steps " << steps);
            REQUIRE (pulseCount (necklace (steps, steps, 0)) == steps);
            REQUIRE (pulseCount (necklace (steps, steps, 7)) == steps);
            REQUIRE (pulseCount (necklace (steps, steps + 100, 0)) == steps); // clamped
        }
    }

    SECTION ("steps <= 0 writes nothing")
    {
        for (const int steps : { 0, -1, -64 })
        {
            INFO ("steps " << steps);
            auto out = generate (steps, 4, 0);
            REQUIRE (tailUntouched (out, 0)); // the whole buffer still holds 0xEE
        }
    }

    SECTION ("a null buffer writes nothing and does not crash")
    {
        arpbox::engine::euclid::generate (16, 4, 0, nullptr);
        SUCCEED ("no write through a null out pointer");
    }

    SECTION ("steps above maxSteps clamps rather than overruns")
    {
        // EuclidParams::steps is a uint8_t and can carry 255, which is wider than the
        // 64-entry lane. The header pins CLAMPING to maxSteps.
        auto out = generate (200, 200, 0);
        REQUIRE (pulseCount (std::vector<std::uint8_t> (out.begin (), out.begin () + maxSteps)) == maxSteps);
        REQUIRE (tailUntouched (out, maxSteps));
    }

    SECTION ("a single-step necklace is on or off, never anything else")
    {
        REQUIRE (necklace (1, 0, 0) == pattern ("."));
        REQUIRE (necklace (1, 1, 0) == pattern ("x"));
        REQUIRE (necklace (1, 1, 7) == pattern ("x")); // rotate mod 1 == 0
    }
}

TEST_CASE ("sequencer/euclid: applyToGateLane writes only the GATE lane", "[unit]")
{
    SECTION ("it writes 0/1 into the GATE lane and sets the lane length to the necklace")
    {
        PatternState state;
        auto& gate = laneOf (state, LaneId::gate);

        // A recognisable pre-existing lane: length 64, every step ON, so both a
        // failure to write and a failure to clear are visible.
        gate.length = 64;
        gate.values.fill (1);

        state.euclid = EuclidParams { 8, 3, 0, true };
        arpbox::engine::euclid::applyToGateLane (state.euclid, gate);

        // "The necklace length IS the lane length" — this is how euclid participates
        // in polymeter (Euclid.h), so a euclid of 8 makes the GATE lane loop every 8.
        REQUIRE (gate.length == 8);

        for (int i = 0; i < 8; ++i)
        {
            INFO ("step " << i);
            REQUIRE (gate.values[static_cast<std::size_t> (i)] == (i == 0 || i == 3 || i == 6 ? 1 : 0));
        }

        // Steps beyond the necklace keep their previous values: they sit outside the
        // active length, so `laneValueAt` never reads them. Pinned, because "clears
        // the tail" and "preserves the tail" are both defensible and only one is real.
        for (int i = 8; i < maxSteps; ++i)
        {
            INFO ("tail step " << i);
            REQUIRE (gate.values[static_cast<std::size_t> (i)] == 1);
        }
    }

    SECTION ("no other lane is touched")
    {
        // The anti-vacuity control for the section above: a generator that scribbled
        // across the whole PatternState would still have produced a correct GATE lane.
        PatternState state;

        for (int lane = 0; lane < numLanes; ++lane)
        {
            auto& l = state.lanes[static_cast<std::size_t> (lane)];
            l.length = static_cast<std::uint8_t> (16 + lane);
            l.division = static_cast<std::uint8_t> (1 + (lane % 4));
            l.values.fill (static_cast<std::int16_t> (100 + lane));
        }

        const auto before = state.lanes;

        state.euclid = EuclidParams { 16, 5, 2, true };
        arpbox::engine::euclid::applyToGateLane (state.euclid, laneOf (state, LaneId::gate));

        for (int lane = 1; lane < numLanes; ++lane) // lane 0 is GATE and is expected to change
        {
            const auto& now = state.lanes[static_cast<std::size_t> (lane)];
            const auto& was = before[static_cast<std::size_t> (lane)];

            INFO ("lane ordinal " << lane);
            REQUIRE (now.length == was.length);
            REQUIRE (now.division == was.division);
            REQUIRE (now.values == was.values);
        }

        // …and GATE really did change, so the loop above is not passing because
        // applyToGateLane did nothing at all.
        REQUIRE (laneOf (state, LaneId::gate).values != before[0].values);
        REQUIRE (laneOf (state, LaneId::gate).length == 16);
    }

    SECTION ("GATE values are clamped to the lane's {0,1} range")
    {
        PatternState state;
        auto& gate = laneOf (state, LaneId::gate);
        gate.values.fill (999); // out of range for GATE

        state.euclid = EuclidParams { 16, 7, 0, true };
        arpbox::engine::euclid::applyToGateLane (state.euclid, gate);

        int outOfRange = 0;
        int onSteps = 0;

        for (int i = 0; i < 16; ++i)
        {
            const auto v = gate.values[static_cast<std::size_t> (i)];

            if (v != 0 && v != 1)
                ++outOfRange;

            if (v == 1)
                ++onSteps;
        }

        REQUIRE (outOfRange == 0);
        REQUIRE (onSteps == 7); // and the right number of them
    }

    SECTION ("a disabled euclid leaves a hand-edited GATE lane exactly as drawn")
    {
        PatternState state;
        auto& gate = laneOf (state, LaneId::gate);
        gate.length = 12;
        gate.values.fill (0);
        gate.values[0] = 1;
        gate.values[5] = 1;

        const auto before = gate;

        state.euclid = EuclidParams { 16, 7, 3, false }; // enabled == false
        arpbox::engine::euclid::applyToGateLane (state.euclid, gate);

        REQUIRE (gate.length == before.length);
        REQUIRE (gate.values == before.values);
    }

    SECTION ("zero steps is a no-op rather than a zero-length lane")
    {
        PatternState state;
        auto& gate = laneOf (state, LaneId::gate);
        gate.length = 16;
        gate.values.fill (1);

        const auto before = gate;

        state.euclid = EuclidParams { 0, 4, 0, true };
        arpbox::engine::euclid::applyToGateLane (state.euclid, gate);

        // A length of 0 would make `laneValueAt` divide by zero were it not guarded;
        // refusing the edit is the safer contract and is what the code does.
        REQUIRE (gate.length == before.length);
        REQUIRE (gate.values == before.values);
    }
}
