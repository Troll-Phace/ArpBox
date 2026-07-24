---
name: run-tests
description: "Run the ARPBOX test suite with detailed reporting. Use after any implementation work, when asked to test, or as part of phase verification."
argument-hint: "[suite-name or test filter]"
context: fork
allowed-tools: Bash Read Grep
---

# Run Tests

Execute the project's test suite and report results.

## Steps

1. Ensure the build is current: `cmake --build build`
2. If a specific suite or filter was provided ($0), run only those tests:
   `ctest --test-dir build -R "$0" --output-on-failure`
   Named contract suites: `determinism`, `midi-conformance`, `hosting-lab`, `perf-budget`
3. Otherwise, run the full suite:
   `ctest --test-dir build --output-on-failure`
4. Parse output for pass/fail/skip counts
5. For any failures:
   - Show the test name and error message (and the reproducing seed for property tests)
   - For golden-MIDI diffs: report WHICH events differ (never suggest silently regenerating goldens)
   - Show the relevant source code context
   - Suggest a likely fix
6. Report summary: {passed}/{total} tests in {duration}, with per-contract-suite status
