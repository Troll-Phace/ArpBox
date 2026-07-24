---
name: phase-review
description: "Comprehensive review of completed phase work. Use after phase-implement to verify quality before moving to the next phase."
context: fork
agent: Plan
allowed-tools: Read Grep Glob Bash
---

# Review Phase

Independently verify the completed phase meets all requirements.

## Steps

1. Read .claude/state/progress.md to identify the phase being reviewed
2. Run `git diff` to see all files changed in this phase
3. Read every modified/created file
4. Run the full test suite (`ctest --test-dir build --output-on-failure`) and record results — including the golden-MIDI determinism suite
5. Check each success criterion from docs/INSTRUCTIONS.md
6. Check code-style compliance against .claude/rules/code-style.md (RT-safety section for any engine changes)
7. If UI work: check design-system compliance against .claude/rules/design-system.md
8. Report pass/fail with specifics

## Output Format

### Phase {N} Review: {PASS / FAIL}

**Build**: pass/fail
**Tests**: X/Y passing, Z failing (call out determinism/conformance suite status explicitly)
**Success Criteria**:
- [ ] or [x] each criterion from INSTRUCTIONS.md

**Issues Found**: (list with severity, file, and description — or "None")
**Action Required**: (next steps, or "Ready for next phase")
