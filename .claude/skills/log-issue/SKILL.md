---
name: log-issue
description: "Log a newly discovered defect, limitation, or tech-debt item as a well-formed GitHub issue with type + severity labels. Use the moment something is found and deferred."
argument-hint: "[short description of the problem]"
allowed-tools: Bash(gh *) Read Grep Glob
---

# Log Issue

## Steps

1. Derive keywords from $ARGUMENTS and the current finding.
2. Dedup: `gh issue list --search "<keywords>" --state all`. If a match exists,
   add a comment linking the new context instead of creating a duplicate; stop.
3. Classify: pick exactly one `type:` and one `severity:` label.
   (ARPBOX default: RT-safety violations, determinism breaks, hanging notes,
   and plugin-state loss are severity:critical.)
4. Choose a milestone if a current breakpoint tier fits; else label `needs-triage`.
5. Create:
   `gh issue create --title "<concise>" --label "type:X,severity:Y" \
      [--milestone "<tier>"] --body "<what / where (file:symbol) / repro / done>"`
6. Report the issue number + classification. This skill only logs — closure
   happens later via `Closes #NN` in the resolving commit or an issue-triage
   close with verification evidence.
