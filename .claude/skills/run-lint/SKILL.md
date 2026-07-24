---
name: run-lint
description: "Run linters and formatters across the ARPBOX codebase. Use before committing, after implementation, or when code quality checks are needed."
context: fork
allowed-tools: Bash Read Grep Glob
---

# Run Lint & Format

## Steps

1. Run clang-format in check mode across source trees:
   `find app engine hosting scanner-helper ui tests -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.mm' | xargs clang-format --dry-run --Werror`
2. Run clang-tidy against the compile database:
   `run-clang-tidy -p build -quiet app engine hosting scanner-helper ui`
3. Report any issues found with file paths and line numbers, grouped by check (flag `concurrency-*` and `bugprone-*` hits as high priority — they are usually RT-safety adjacent)
4. If the user wants auto-fix, run the formatter in write mode:
   `find app engine hosting scanner-helper ui tests -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.mm' | xargs clang-format -i`
5. Report summary: {N} issues found, {M} auto-fixed
