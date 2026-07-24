---
paths:
  - "tests/**/*"
  - "**/*Test.cpp"
  - "**/*Tests.cpp"
  - "**/*.test.cpp"
---

# Testing Standards — ARPBOX

## General Rules
- Write tests for every new engine class, operator, and hosting behavior
- Test happy path, edge cases, and error handling
- Never instantiate real third-party plugins in unit tests — use the `FakePlugin` corpus in `tests/fakes/` (crash-on-scan, allocate-in-process, wrong-latency-reporting, state-corrupting variants)
- Descriptive test names: `TEST_CASE("sequencer/clock: tempo change lands on block boundary")` — `module/unit: behavior` convention
- One logical assertion cluster per SECTION
- Tests must be deterministic: every randomized test takes an explicit seed; no wall-clock timing dependencies (drive the engine by rendering blocks, not by sleeping)

## Catch2 Patterns
- Framework: Catch2 v3 via CTest (`ctest --test-dir build --output-on-failure`)
- Engine tests run headless: construct engine objects directly, call `prepareToPlay(48000, 128)` + `processBlock` in a loop — no audio device, no message loop
- Golden-MIDI determinism suite: (pattern, seeds, N bars) → rendered `MidiBuffer` stream serialized and compared byte-for-byte against `tests/golden/*.mid`. Any diff is a FAILURE — saved projects must never change sound across versions. Regenerating goldens requires an explicit justification in the PR body
- Hanging-note fuzzer: property test that randomly churns transport/pattern/pool/plugin-swap events and asserts the sounding-note table is empty after all-notes-off
- RT-safety: audio-thread code paths run under TSan/ASan CI jobs; allocation guards (overridden operator new counter) assert zero allocations inside processBlock during steady-state tests

## Coverage Expectations
- Engine (`engine/`): 85%+ — this is the product
- Hosting (`hosting/`): every public behavior exercised against the fake-plugin corpus
- UI (`ui/`): component smoke tests (construct, resize, paint to image) + interaction tests for the piano roll edit model; pixel-perfection is verified by review, not asserted
- Critical paths (MIDI correctness, snapshot swap, state save/load round-trip): 95%+
