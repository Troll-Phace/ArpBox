---
name: phase-status
description: "Show current project progress dashboard. Use when asked about project status, at the start of a session, or when the user wants an overview of where things stand."
allowed-tools: Read Bash(git log *) Bash(git status) Bash(git diff --stat) Grep Glob
---

# Project Status Dashboard

## Steps

1. Read .claude/state/progress.md for phase tracking state
2. Check git log --oneline -10 for recent commits
3. Run the test suite (`ctest --test-dir build --output-on-failure`) and count pass/fail
4. Count source files and lines of code across app/ engine/ hosting/ ui/ scanner-helper/
5. Check for TODO/FIXME/HACK comments across the codebase

## Output Format

**Current Phase**: {N} — {Title} ({status})
**Completed Phases**: {list with dates}
**Build Status**: pass/fail
**Tests**: X passing, Y failing, Z total (note contract-suite status: determinism / conformance / hosting lab / perf)
**Codebase**: N files, M lines of code
**Open TODOs**: count (list locations if < 10)
**Last Commit**: {hash} — {message} — {date}
**Next Steps**: {what to work on based on progress.md}
