// ─────────────────────────────────────────────────────────────────────────────
// midi-conformance suite — smoke test (docs/INSTRUCTIONS.md Phase 1.2).
//
// The real suite (ARCHITECTURE §5.5, Phase 8.3): the hanging-note fuzzer plus
// overlap/retrigger property tests — assert the sounding-note table is empty
// after every flush point and there are zero orphan note-ons. This smoke test
// only proves the labeled pipeline.
// ─────────────────────────────────────────────────────────────────────────────

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("midi-conformance/smoke: contract-suite pipeline is wired", "[midi-conformance]")
{
    REQUIRE (true);
}
