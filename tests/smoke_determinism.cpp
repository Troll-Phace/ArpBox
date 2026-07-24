// ─────────────────────────────────────────────────────────────────────────────
// determinism suite — smoke test (docs/INSTRUCTIONS.md Phase 1.2).
//
// The real determinism contract (ARCHITECTURE §1.2, §5.2): same (pattern, seeds,
// N bars) ⇒ byte-identical MIDI vs tests/golden/, forever. Phase 6+ populates
// golden-MIDI comparison here. This smoke test only proves the labeled pipeline.
//
// Every randomized test in this suite MUST take an explicit seed so failures
// reproduce from the printed seed (.claude/rules/testing.md).
// ─────────────────────────────────────────────────────────────────────────────

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("determinism/smoke: contract-suite pipeline is wired", "[determinism]")
{
    REQUIRE (true);
}
