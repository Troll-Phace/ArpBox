---
name: phase-implement
description: "Execute the planned implementation for the current phase by delegating tasks to subagents. Use after phase-plan has been approved."
allowed-tools: Read Write Edit Bash Grep Glob Agent
---

# Implement Phase

Execute all tasks in the current phase via delegation to subagents.

## Steps

1. Confirm the plan from /phase-plan is approved
2. For each task in dependency order:
   a. Prepare the delegation prompt with full context (architecture refs, design specs, acceptance criteria)
   b. Delegate to the assigned subagent (audio-engine-dev / plugin-host-dev / generative-seq-dev / juce-ui-dev / test-engineer)
   c. Review the subagent's output against the task's success criteria
   d. If issues found, provide specific feedback and re-delegate
3. After all tasks complete:
   a. Run the full test suite: `cmake --build build && ctest --test-dir build --output-on-failure`
   b. Verify every success criterion from docs/INSTRUCTIONS.md
4. MANDATORY review gate — do NOT skip:
   a. Delegate to the `code-reviewer` subagent for a full review pass over the phase's changes (request an explicit RT-safety audit if any engine code changed)
   b. `code-reviewer` hands any finding it does not fix to `issue-triage`/`log-issue` for filing
   c. Resolve blocking (CRITICAL) findings — re-delegate fixes to the implementing agent — or confirm they are logged as issues before proceeding
5. Update .claude/state/progress.md:
   - Check off completed tasks
   - If all pass and the review gate is clear: advance to next phase
   - If any fail: document what needs fixing
