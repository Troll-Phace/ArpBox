---
name: triage-issues
description: "Sweep untriaged and unlabeled GitHub issues, assigning type + severity + milestone. Use to clear the needs-triage backlog or after a batch of quick logs."
context: fork
allowed-tools: Bash(gh *) Read Grep Glob
---

# Triage Issues

## Steps

1. List candidates: `gh issue list --label needs-triage --state open`
   and `gh issue list --search "no:label" --state open`.
2. For each, read the body (and referenced files if needed) and assign:
   one `type:`, one `severity:`, and a milestone if a tier fits.
   `gh issue edit <n> --add-label "type:X,severity:Y" \
      --remove-label needs-triage [--milestone "<tier>"]`
3. Report a table: # | title | type | severity | milestone.
4. Flag anything ambiguous for the user rather than guessing.
