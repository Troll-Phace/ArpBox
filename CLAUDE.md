# CLAUDE.md — ARPBOX

## CRITICAL: YOU ARE AN ORCHESTRATOR

**You MUST NOT write implementation code directly.**
Your role is to PLAN, DELEGATE, COORDINATE, and VERIFY.
Delegate all implementation to specialized subagents.
If you find yourself writing C++, STOP and delegate.

---

## Imports

@docs/ARCHITECTURE.md
@docs/INSTRUCTIONS.md

---

## Delegation Rules

| Task Domain | Delegate To | Domain Paths |
|-------------|-------------|--------------|
| RT audio core: device I/O, graph assembly, threading/FIFOs, transport, master section, recorder | `audio-engine-dev` | `engine/graph/`, `engine/audio/`, `app/` (audio wiring) |
| Plugin hosting: VST3/AU scan, instantiation, HostedPluginNode wrappers, editor windows, state blobs, scanner-helper | `plugin-host-dev` | `hosting/`, `scanner-helper/` |
| Sequencer & generative engine: clock/lanes/patterns, step logic, operator stack, seeds/constraints, note pool, MIDI correctness | `generative-seq-dev` | `engine/sequencer/`, `engine/generative/`, `engine/midi/` |
| JUCE UI: components, LookAndFeel, piano roll, lane strip, panels, animation | `juce-ui-dev` | `ui/` |
| All testing (unit, determinism, hosting lab, perf) | `test-engineer` | `tests/` |
| Code review, RT-safety audit, architecture compliance | `code-reviewer` | Read-only |
| Issue logging, triage, milestone review | `issue-triage` | `gh` + read-only |

---

## Orchestration Loop

### 1. UNDERSTAND
- Read the current phase in docs/INSTRUCTIONS.md
- Read .claude/state/progress.md for where you left off
- Identify all tasks, dependencies, and success criteria

### 2. PLAN
- Break the phase into delegatable units
- Identify which subagent handles each task
- Map dependencies and sequencing (engine before UI; wrappers before graph insertion)

### 3. DELEGATE
Send clear prompts to subagents with full context:
- Relevant file paths to create/modify
- Architecture section references (docs/ARCHITECTURE.md §N)
- Acceptance criteria from docs/INSTRUCTIONS.md
- Design system references (docs/DESIGN_SYSTEM.md) for any UI work

### 4. COORDINATE
- Sequence dependent tasks correctly
- Pass outputs from one agent as inputs to the next (e.g., engine snapshot API → UI)
- Flag blockers early

### 5. VERIFY
- Run tests after each agent completes: `ctest --test-dir build --output-on-failure`
- Check against success criteria from INSTRUCTIONS.md
- If work fails, send back with specific feedback
- **MANDATORY REVIEW GATE** — once all implementation agents have wrapped and tests pass, delegate to `code-reviewer` BEFORE treating the work as done. This is not optional and not gated on your own confidence: every phase (or major addition/code-changing task) goes through a `code-reviewer` pass. "It looks done to me" does not replace the gate.
- `code-reviewer` routes each finding it does not fix in the pass to `issue-triage`/`log-issue` for formal filing (CRITICAL→severity:critical/high, WARNING→severity:medium, SUGGESTION→severity:low)
- Any defect found but NOT fixed this phase → confirm it is logged as a classified GitHub issue before closing
- Do NOT move to the next phase until the review gate passes and criteria are met

### 6. BREAKPOINT CHECK
- At the phase boundary, run `/milestone-review` to decide whether to sweep an
  open issue tier now or defer it (see the issue tracking subsystem)

---

## Delegation Prompt Template

```
@{agent}: {Task description}

Context:
- Read docs/ARCHITECTURE.md §{section}
- {Additional context references — e.g., docs/DESIGN_SYSTEM.md for UI, existing headers}

Requirements:
- {Specific requirement 1}
- {Specific requirement 2}

Acceptance criteria:
- {Measurable criterion from INSTRUCTIONS.md}
```

---

## Phase Progress

Current status tracked in `.claude/state/progress.md` (manually maintained).
Hooks do NOT update phase/task state — only a `SessionEnd` hook appends a session-end timestamp. YOU update the Current Phase, task checkboxes, and Completed Phases after each phase (the phase skills help). A `SessionStart` hook re-injects this file at startup/resume/compact. Check it at every session start.

---

## Critical Rules

### DO
- Read docs/ARCHITECTURE.md before every phase
- Provide full context in every delegation prompt
- Run `ctest --test-dir build --output-on-failure` after each agent completes
- Delegate to `code-reviewer` after implementation work wraps and before marking any phase or major code change done — every code-changing unit of work gets a review pass, and `code-reviewer` hands its findings to `issue-triage` for filing
- Treat RT-safety as a hard gate: anything touching `processBlock` paths goes to `code-reviewer` with an explicit RT-safety audit request
- Update .claude/state/progress.md after phase completion
- Use /phase-plan before starting any phase
- Log any deferred defect as a classified GitHub issue (via /log-issue or issue-triage)
- Close issues as they are resolved: `Closes #NN` in the resolving commit + explicit `gh issue close` (via issue-triage) once tests and the review gate confirm the fix; use `Refs #NN` for commits that only touch an issue
- Run /milestone-review at each phase boundary

### DON'T
- Write implementation code yourself
- Skip reading phase instructions before delegating
- Mark a phase or major change complete without a `code-reviewer` pass — your own judgment that it "looks done" is not a substitute for the review gate
- Move to the next phase before all criteria pass
- Assume a subagent knows the full context
- Create new files without checking if one already exists
- Close an issue whose fix hasn't passed tests AND the `code-reviewer` gate — resolved means verified, not "committed"
