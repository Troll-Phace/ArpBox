# Orchestrator Behavior Rules

## Delegation Protocol
- NEVER write implementation code. Delegate ALL code to subagents.
- ALWAYS include in delegation prompts:
  1. File paths to create or modify
  2. Architecture reference (section number from docs/ARCHITECTURE.md)
  3. Acceptance criteria from docs/INSTRUCTIONS.md
  4. Design system references (docs/DESIGN_SYSTEM.md) for any UI work
- ALWAYS run the project test suite after each subagent completes.
- NEVER move to the next phase until all success criteria pass.

## Review Gate (mandatory)
- After all implementation subagents have wrapped and tests pass — and BEFORE you
  consider a phase or any major/code-changing task done — you MUST delegate to the
  `code-reviewer` subagent. This gate is unconditional: it does not depend on how
  confident you are that the work is correct. Every code-changing unit of work is reviewed.
- The handoff chain is: implementation agents → `code-reviewer` → `issue-triage`.
  `code-reviewer` reports findings and routes anything it does not fix in the pass to
  `issue-triage`/`log-issue` for formal filing.
- A phase is not "done" and you do not advance progress.md or open a commit until the
  `code-reviewer` pass has run and its blocking findings are resolved or logged as issues.
- Do not self-review in place of this gate. "It looks correct to me" is not a review pass.
- For any change touching audio-thread code (`processBlock`, FIFOs, snapshot swaps),
  explicitly request an RT-safety audit in the review delegation.

## Phase Workflow
- Check .claude/state/progress.md for current phase
- Use /phase-plan to create implementation plans
- Use /phase-implement to execute the plan
- Use /phase-review to verify completion
- Use /phase-status for a progress dashboard
- Use /milestone-review at each phase boundary to decide the next issue sweep

## Issue Tracking
- Log any deferred defect/limitation/tech-debt item as a classified GitHub issue
  (via /log-issue or the issue-triage subagent) before moving on — never fix-and-forget.
- Close issues as they are resolved: `Closes #NN` in the resolving commit, plus an
  explicit `gh issue close` (delegated to issue-triage) once tests and the
  `code-reviewer` gate confirm the fix. `Refs #NN` for touch-but-not-resolve commits.

## Error Recovery
- If a subagent's work fails verification, send back with specific feedback
- Use /rewind for in-session rollbacks of bad changes
- Use git stash or branches for cross-session recovery
- Use worktree isolation for experimental or risky subagent work
