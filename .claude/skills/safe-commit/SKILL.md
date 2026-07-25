---
name: safe-commit
description: "Create a well-formatted git commit with safety checks. Use when asked to commit, save progress, or checkpoint work."
argument-hint: "[commit message]"
allowed-tools: Bash(git *) Read Grep
---

# Safe Commit

## Steps

1. Run `git status` to review all changes
2. Run `git diff --stat` for an overview
3. Safety checks:
   - Verify no .env, secrets, credentials, signing identities, or API keys are staged
   - Verify no large binary files are staged (built plugins, .app bundles, DMGs)
   - Verify no `tests/golden/*` changes are staged without a justification line ready for the commit body (the goldens are canonical TEXT event streams, not `.mid` — see `tests/golden/README.md`; the glob covers every future golden type: Phase 7 condition matrices, Phase 12/13 operator goldens)
   - Check that lockfiles aren't accidentally modified
4. Run the test suite if any source files changed: `ctest --test-dir build --output-on-failure`
5. Stage relevant files (specific files, NOT `git add .` or `git add -A`)
6. Commit with format: `phase({N}): {description}` (or `fix:` for fixes outside a phase)
   - If $0 was provided, use it as the commit message
   - Otherwise, generate a message from the staged changes
   - If this commit RESOLVES an issue (tests green + review gate passed), use `Closes #NN` / `Fixes #NN` in the body; for issues merely touched, use `Refs #NN`
   - For resolving commits on non-default branches, follow up by delegating explicit closure to issue-triage (`gh issue close NN --comment "Resolved in <hash>: ..."`), since keywords only fire on the default branch
7. Report: commit hash, files changed, insertions/deletions
