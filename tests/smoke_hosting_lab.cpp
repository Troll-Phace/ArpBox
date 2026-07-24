// ─────────────────────────────────────────────────────────────────────────────
// hosting-lab suite — smoke test (docs/INSTRUCTIONS.md Phase 1.2).
//
// The real lab (ARCHITECTURE §6, Phase 3.3+): scan/instantiate/save/reload/
// editor/param-storm against the tests/fakes/ hostile-plugin corpus. Real
// third-party plugins are NEVER instantiated here — fakes only
// (.claude/rules/testing.md). This smoke test only proves the labeled pipeline.
// ─────────────────────────────────────────────────────────────────────────────

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("hosting-lab/smoke: contract-suite pipeline is wired", "[hosting-lab]")
{
    REQUIRE (true);
}
