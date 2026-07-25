# Git Conventions

## Commit Format
phase({N}): {concise description of what changed}

Examples:
- phase(1): scaffold CMake + JUCE app shell with signing entitlements
- phase(5): implement sample-accurate transport clock and sequencer node
- fix: flush sounding-note table on pattern switch (hanging notes)

## Branch Naming
- Feature: phase/{N}-{short-description}
- Fix: fix/{issue-description}
- Experiment: experiment/{description}

## Rules
- Never force-push to main/master
- Stage specific files, not `git add .`
- Never commit .env, secrets, credentials, or signing identities
- Never regenerate `tests/golden/*` files in a commit without an explicit justification line in the body (goldens are canonical TEXT event streams, not `.mid` — see `tests/golden/README.md`)
- PR titles under 70 characters
- PR body includes: Summary, Test Plan, and phase reference

## Issue References
- When a commit RESOLVES an issue, use an auto-closing keyword in the body:
  `Closes #NN` (or `Fixes #NN` for bugs) — one per line for multiple issues.
- Auto-close keywords only fire when the commit lands on the default branch;
  for work on phase/fix branches, ALSO close the issue explicitly after the
  review gate passes: `gh issue close NN --comment "Resolved in <hash>: <summary>"`.
- Closure requires resolution, verified: tests green and the `code-reviewer`
  pass complete. Never close an issue the commit merely touches or partially
  addresses — use `Refs #NN` for those.
- When a batch resolves every issue in a milestone, note "milestone #N complete"
  in the phase summary (milestone closure itself is left to the user).
