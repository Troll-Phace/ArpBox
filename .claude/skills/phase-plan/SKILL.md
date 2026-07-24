---
name: phase-plan
description: "Plan the current development phase. Use when starting a new phase, when asked to plan, or at the beginning of a development session after checking status. ALWAYS fetches up-to-date library documentation via Context7 before planning."
argument-hint: "[phase-number]"
allowed-tools: Read Grep Glob Bash(git status) Bash(git log *) mcp__Context7__resolve-library-id mcp__Context7__get-library-docs mcp__Context7__query-docs
---

# Plan Phase

Review the current development phase and create an implementation plan.

## MANDATORY: Fetch Current Library Documentation (Context7)

**Before doing ANY planning work**, you MUST use the Context7 MCP to fetch up-to-date documentation for every library and framework involved in the current phase. This ensures your plan reflects current APIs, not stale training data.

1. Identify all libraries/frameworks relevant to this phase's tasks — for ARPBOX that is almost always **JUCE 8** (hosting, AudioProcessorGraph, GUI), plus **Catch2** for test-heavy phases and **CMake** for build phases
2. For EACH library, call `mcp__Context7__resolve-library-id` with the library name to get its Context7 ID
3. For EACH resolved library, fetch current documentation scoped to this phase's topics (e.g. "AudioProcessorGraph node lifecycle", "ChildProcessCoordinator", "VBlankAttachment")
4. Use this documentation as the source of truth for API signatures, patterns, and best practices in your plan

**Do NOT skip this step.** Even if you think you know JUCE well, fetch the docs — hosting and rendering APIs have changed across JUCE 8 point releases.

## Steps

1. **Fetch library docs** (see above — this is step 1, always)
2. Read .claude/state/progress.md to identify the current phase
3. If a phase number was provided ($0), use that phase instead
4. Read docs/INSTRUCTIONS.md for the phase's tasks and success criteria
5. Read docs/ARCHITECTURE.md sections referenced by the phase
6. If the phase involves UI work, also read docs/DESIGN_SYSTEM.md
7. For each task, identify: assignee (audio-engine-dev / plugin-host-dev / generative-seq-dev / juce-ui-dev / test-engineer), files to create/modify, dependencies, and architecture refs
8. Present the plan in table format for approval before execution

## Output Format

### Phase {N}: {Title}

**Objective**: {one sentence}
**Prerequisites**: {completed phases}

| # | Task | Assignee | Files | Dependencies | Arch Reference |
|---|------|----------|-------|-------------|----------------|
| 1 | ...  | ...      | ...   | ...         | ...            |

**Parallel opportunities**: {tasks that can run simultaneously}
**Risk areas**: {concerns, unknowns, or complexity hotspots — flag anything touching the audio thread or the determinism contract}
**Estimated delegations**: {count of subagent prompts needed}
