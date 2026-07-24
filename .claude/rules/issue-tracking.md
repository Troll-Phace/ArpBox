# Issue Tracking Protocol

## Capture on sight
- When you discover a defect, limitation, perf smell, or tech-debt item that you
  are NOT fixing in the current task, LOG IT as a GitHub issue before moving on.
- Do not fix-and-forget silently, and do not let findings live only in prose notes.
- Before logging, search existing issues to avoid duplicates:
  `gh issue list --search "<keywords>" --state all`

## Every issue is fully classified
- Exactly one `type:` label (bug | feature | perf | refactor | docs | test | security)
- Exactly one `severity:` label (critical | high | medium | low)
- A milestone if one fits the roadmap breakpoint; otherwise leave `needs-triage`
- A body with: what, where (file/symbol), how to reproduce or observe, and
  what "fixed" looks like (a verifiable outcome)

## Delegate the mechanics
- Route issue creation/triage through the `issue-triage` subagent or the
  `log-issue` / `triage-issues` / `milestone-review` skills — don't hand-run
  long `gh` sequences in the orchestrator context.

## Breakpoint discipline
- At every phase boundary, run `milestone-review` to decide what to sweep next.

## Closure on resolution
- When an issue's fix is verified (tests green + `code-reviewer` pass complete),
  CLOSE IT: use `Closes #NN`/`Fixes #NN` in the resolving commit, and — since
  keywords only fire on the default branch — close explicitly via
  `gh issue close NN --comment "Resolved in <hash>: <summary>"` after the gate.
- Use `Refs #NN` for commits that touch but do not resolve an issue.
- Never close unverified or partially addressed issues; milestone closure
  stays with the user.
