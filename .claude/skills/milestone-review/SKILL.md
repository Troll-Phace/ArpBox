---
name: milestone-review
description: "Review open milestones at a roadmap breakpoint and recommend which issues to fix now vs defer. Use at phase boundaries or when deciding the next batch."
argument-hint: "[milestone/tier name]"
context: fork
allowed-tools: Bash(gh *) Bash(git log *) Read Grep Glob
---

# Milestone Review

## Steps

1. Snapshot progress:
   `gh api repos/{owner}/{repo}/milestones --jq '.[] | "\(.number) \(.title) open:\(.open_issues) closed:\(.closed_issues)"'`
2. For the target tier (or the most urgent one), list issues by severity:
   `gh issue list --milestone "<tier>" --state open --json number,title,labels`
3. Recommend a fix-now BATCH: order by severity (critical → high → …), then by
   dependency; group issues touching disjoint files so they can run in parallel
   (map each issue to the owning agent: audio-engine-dev / plugin-host-dev /
   generative-seq-dev / juce-ui-dev / test-engineer).
4. Identify the ideal breakpoint: is now a good moment to sweep this tier, or
   should it wait until after the current phase? State the tradeoff.
5. Cross-check recently resolved work: if verified fixes (commit + green tests +
   review gate) left issues open — e.g. keyword closure didn't fire off the
   default branch — list them for explicit closure via issue-triage.
6. Output: a batch plan (issues, suggested agent per issue, parallel groups) +
   issues to close now (with evidence) + any milestone reported "complete"
   (milestone closure itself is left to the user).
