// ─────────────────────────────────────────────────────────────────────────────
// perf-budget suite — smoke test (docs/INSTRUCTIONS.md Phase 1.2).
//
// The real budget (ARCHITECTURE §11, Phase 21.1): processBlock < 3 ms at
// 128 samples / 48 kHz on the synthetic heavy graph (synth fake + 6 FX fakes +
// full 8-operator stack), plus zero steady-state audio-thread allocation. This
// smoke test only proves the labeled pipeline.
// ─────────────────────────────────────────────────────────────────────────────

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("perf-budget/smoke: contract-suite pipeline is wired", "[perf-budget]")
{
    REQUIRE (true);
}
