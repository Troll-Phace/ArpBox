// ─────────────────────────────────────────────────────────────────────────────
// unit suite — smoke test (docs/INSTRUCTIONS.md Phase 1.2).
//
// Proves the unit pipeline links the engine static library (arpbox_engine) and
// can call into it headless. Later phases add real per-class engine unit tests
// here (see .claude/rules/testing.md — 85%+ engine coverage target).
//
// Test-name convention: "module/unit: behavior" (.claude/rules/testing.md).
// ─────────────────────────────────────────────────────────────────────────────

#include "engine/graph/EnginePlaceholder.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("engine/placeholder: reports a non-empty semantic version", "[unit]")
{
    // Exercises something real where cheap: linking + calling the engine lib
    // proves the unit-test seam through to arpbox_engine works.
    const auto version = arpbox::engine::EnginePlaceholder::getEngineVersion ();

    REQUIRE_FALSE (version.isEmpty ());
    REQUIRE (version.containsChar ('.')); // MAJOR.MINOR.PATCH form
}
