---
name: run-lint
description: "Run linters and formatters across the ARPBOX codebase. Use before committing, after implementation, or when code quality checks are needed."
context: fork
allowed-tools: Bash Read Grep Glob
---

# Run Lint & Format

All invocations go through `.claude/skills/run-lint/lint.sh`, which is the SAME
script CI's `clang-tidy (single-arch DB, non-blocking)` and
`clang-format (blocking)` jobs run, and the same script the format-on-save
`PostToolUse` hook calls. Do not
hand-roll `clang-tidy` / `run-clang-tidy` invocations: local and CI drifting
apart is what made a single-arch compile database get improvised by hand during
Phase 4 and again during Phase 5 (issue #31). The script also resolves the LLVM
binaries itself — `clang-tidy` and `clang-format` are NOT on `PATH` and the
Xcode toolchain does not ship them.

## Steps

1. Run clang-tidy against the single-arch compile database:
   `bash .claude/skills/run-lint/lint.sh tidy -j 8`
   - Configures the `tidy` CMake preset into `build-tidy/` on first use
     (configure only — that tree is never built) and reuses it afterwards.
     Add `--reconfigure` after adding or removing source files.
   - Never lints in `build/`: the universal build's dual `-arch` flags make
     clang-tidy fail with `expected exactly one compiler job`.
   - Exit status is non-zero if anything was reported (`.clang-tidy` sets
     `WarningsAsErrors: '*'`).
2. Report any issues found with file paths and line numbers, grouped by check
   (flag `concurrency-*` and `bugprone-*` hits as high priority — they are
   usually RT-safety adjacent).
3. Compiler-warning gate (issue #79):
   `bash .claude/skills/run-lint/lint.sh warnings -j 8`
   - **Separate from step 1 and equally blocking.** clang-tidy runs its own check
     set and never sees compiler diagnostics; `lint.sh tidy` and `lint.sh format`
     both exited 0 for a whole phase with three live `-Wswitch-enum` warnings in
     the tree. CI's `compiler warnings (blocking)` job runs this exact command.
   - Builds the `tidy` preset's `build-tidy/` tree (unlike step 1, which only
     configures it) and fails on any `warning:` in `app/ engine/ hosting/
     scanner-helper/ ui/`. Full log: `build-tidy/arpbox-warnings.log`.
   - It refuses to report a clean result it cannot justify — six guards, each of
     which fails the gate rather than warning. It deletes the object file of every
     in-scope TU and asserts they all came back (so a no-op build cannot pass),
     self-tests its diagnostic parser against a fixture, asserts
     `-Wswitch-enum`/`-Wall` actually reach every in-scope source (rejecting a
     `-Wno-` or a bare `-w` that would cancel them), and runs an **end-to-end
     canary** (issue #87): a throwaway TU compiled with a real in-scope TU's own
     flags, provoking a real `-Wswitch-enum`, asserted to survive the extractor.
     The canary is the only guard that tests the *join* between compiler output and
     extractor — the other five can all be green while a diagnostic-format change
     has made the gate completely blind.
   - If a guard trips, the gate fails loudly instead of printing "0 warnings". A
     canary failure means the diagnostic format changed: fix the extractor's
     pattern (it prints the new format for you), never relax the check.
   - `tests/` is out of scope (issue **#86**): the `arpbox_tests` target carries no
     `-W` flags at all, so claiming coverage there would be a false green. The
     exclusion is per-target — `app/SynthSlot.cpp` builds into both targets and is
     covered via the flagged `ARPBOX` one.
4. Formatting check:
   `bash .claude/skills/run-lint/lint.sh format`
   - This is a **blocking gate** — CI's `clang-format (blocking)` job runs the
     exact same command, so any finding is a real failure, not background noise
     (issue #30: the tree drifted for the whole project history because the
     format-on-save hook was a silent no-op; that is now fixed and swept).
   - Normal fix: format only the files your change touched —
     `bash .claude/skills/run-lint/lint.sh format-file <path>...` (the same mode
     the `PostToolUse` hook calls on every C++ Write/Edit).
   - A whole-tree reformat is still user-only: `lint.sh format-fix` refuses
     unless `ARPBOX_ALLOW_FORMAT_FIX=1` is set, because a repo-wide reformat
     belongs in its own dedicated commit, never mixed into a change under review.
5. Report summary: `{N} clang-tidy findings`, `{W} compiler warnings`,
   `{M} files with format drift`.

## Troubleshooting

- `bash .claude/skills/run-lint/lint.sh tools` prints the resolved paths for
  `clang-tidy`, `clang-format` and `run-clang-tidy`.
- If a tool is missing: `brew install llvm`, or point `ARPBOX_LLVM_BIN` at an
  LLVM `bin/` directory.
