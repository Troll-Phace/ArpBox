---
name: run-lint
description: "Run linters and formatters across the ARPBOX codebase. Use before committing, after implementation, or when code quality checks are needed."
context: fork
allowed-tools: Bash Read Grep Glob
---

# Run Lint & Format

All invocations go through `.claude/skills/run-lint/lint.sh`, which is the SAME
script CI's `clang-tidy (single-arch DB, non-blocking)` job runs. Do not
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
3. Formatting check — **read this before running it**:
   `bash .claude/skills/run-lint/lint.sh format`
   - Issue **#30 is open**: the tree has repo-wide clang-format drift, so this
     reports PRE-EXISTING findings in code the current change never touched.
     Report only the findings inside the change under review; do not present the
     repo-wide count as a red gate.
   - Do NOT reformat the repo to make it pass. `lint.sh format-fix` deliberately
     refuses unless `ARPBOX_ALLOW_FORMAT_FIX=1` is set, because the repo-wide
     reformat decision belongs to the user, not to a lint step. Until #30 is
     decided, hand-match surrounding style in files you edit.
4. Report summary: `{N} clang-tidy findings`, `{M} files with format drift
   inside the change under review`.

## Troubleshooting

- `bash .claude/skills/run-lint/lint.sh tools` prints the resolved paths for
  `clang-tidy`, `clang-format` and `run-clang-tidy`.
- If a tool is missing: `brew install llvm`, or point `ARPBOX_LLVM_BIN` at an
  LLVM `bin/` directory.
