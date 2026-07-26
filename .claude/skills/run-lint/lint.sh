#!/usr/bin/env bash
#
# ARPBOX lint driver — the ONE mechanism shared by the /run-lint skill, CI's
# `clang-tidy (single-arch DB, non-blocking)` job, CI's blocking `clang-format`
# job, CI's blocking `compiler warnings` job, and the format-on-save PostToolUse
# hook (issues #31, #30, #79).
#
# Before this existed, CI had a working single-arch clang-tidy invocation and the
# local path had none, so a single-arch compile database plus an explicit
# --extra-arg=-isysroot had to be improvised by hand during Phase 4 and again
# during Phase 5. Both callers now go through this script and the `tidy` CMake
# preset, so a finding CI can see is a finding a developer can see, with the same
# check set over the same file set.
#
# Usage:
#   lint.sh tidy [--reconfigure] [-j N]   clang-tidy over the ARPBOX sources (default)
#   lint.sh warnings [--reconfigure] [-j N]
#                                         COMPILER-warning gate: recompiles every
#                                         ARPBOX translation unit and fails on any
#                                         warning in our own sources (issue #79)
#   lint.sh format                        clang-format --dry-run --Werror (CHECK only)
#                                         over tracked AND untracked-but-not-ignored
#                                         sources (issue #58)
#   lint.sh format-fix                    clang-format -i over the whole tree
#   lint.sh format-file <path>...         clang-format -i on specific files
#                                         (this is what the PostToolUse hook in
#                                          .claude/settings.json calls, so tool
#                                          resolution lives in ONE place — #30)
#   lint.sh tools                         print resolved tool paths and exit
#
# Exit status: non-zero if any finding was reported (.clang-tidy sets
# WarningsAsErrors: '*'), so this is usable as a gate. CI's clang-tidy job keeps
# `continue-on-error: true` at the job level while the tree is still a scaffold;
# CI's FORMAT job is blocking, because format drift is exactly what #30 was, and
# CI's WARNINGS job is blocking for the same reason (see run_warnings below).
#
# Tool resolution: clang-tidy / clang-format / run-clang-tidy are NOT on PATH on
# a stock macOS box. The Xcode toolchain ships NO clang-tidy at all
# (`xcrun -f clang-tidy` fails) and only an older clang-format (`xcrun -f
# clang-format` -> Apple clang-format 21); Homebrew LLVM supplies both.
# resolve_tool() therefore probes $ARPBOX_LLVM_BIN, the Homebrew LLVM prefixes,
# xcrun, then PATH, and reports what it used instead of failing obscurely.
# Homebrew LLVM is preferred so local runs, CI and the hook agree; the xcrun
# fallback is a courtesy — checked on 2026-07 to produce byte-identical output to
# Homebrew clang-format 22 across all 85 tracked sources under this .clang-format.

set -uo pipefail

# Captured BEFORE the cd below: `format-file` may be handed a relative path by a
# caller running somewhere else, and it must resolve against the caller's cwd.
INVOKED_PWD="$PWD"

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || (cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd))"
cd "$REPO_ROOT" || exit 1
# Symlink-resolved twin of REPO_ROOT. CMake writes physical paths into the
# compile database, so the coverage check below must be able to strip either form
# or it would report phantom "unlinted" files on a symlinked checkout.
REPO_ROOT_PHYS="$(pwd -P)"

# Trees clang-tidy lints, mirroring .clang-tidy's HeaderFilterRegex and the set
# CI linted before this script existed. tests/ is deliberately excluded: Catch2
# test code is not held to the engine's RT-safety checks.
SRC_DIRS=(app engine hosting scanner-helper ui)

# Trees clang-format covers — the same list PLUS tests/, which is held to the
# project style even though it is not held to the tidy checks.
FORMAT_DIRS=("${SRC_DIRS[@]}" tests)

TIDY_PRESET="tidy"
TIDY_BUILD_DIR="$REPO_ROOT/build-tidy"

# ---------------------------------------------------------------------------
# tool resolution
# ---------------------------------------------------------------------------

resolve_tool() {
    local tool="$1" candidate brew_prefix
    for candidate in \
        "${ARPBOX_LLVM_BIN:-}/$tool" \
        "/opt/homebrew/opt/llvm/bin/$tool" \
        "/usr/local/opt/llvm/bin/$tool"; do
        [[ "$candidate" == "/$tool" ]] && continue
        if [[ -x "$candidate" ]]; then echo "$candidate"; return 0; fi
    done
    if brew_prefix="$(brew --prefix llvm 2>/dev/null)" && [[ -x "$brew_prefix/bin/$tool" ]]; then
        echo "$brew_prefix/bin/$tool"; return 0
    fi
    if candidate="$(xcrun -f "$tool" 2>/dev/null)" && [[ -x "$candidate" ]]; then
        echo "$candidate"; return 0
    fi
    if candidate="$(command -v "$tool" 2>/dev/null)"; then
        echo "$candidate"; return 0
    fi
    return 1
}

require_tool() {
    local tool="$1" path
    if ! path="$(resolve_tool "$tool")"; then
        echo "error: could not find '$tool'." >&2
        echo "       Install Homebrew LLVM (brew install llvm) or set" >&2
        echo "       ARPBOX_LLVM_BIN=/path/to/llvm/bin. Note the Xcode" >&2
        echo "       toolchain does NOT ship $tool." >&2
        return 1
    fi
    echo "$path"
}

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

# Escape a literal string for use inside a POSIX/Python-style regex.
regex_escape() { printf '%s' "$1" | sed 's/[].[^$()*+?{}|\\/]/\\&/g'; }

# _tracked_patterns <dir>...  — fills TRACKED_PATTERNS with the git pathspecs for
# the C++ sources/headers under those trees. One definition, two output formats
# below, so the line-oriented and NUL-oriented views can never disagree about
# which files they cover.
TRACKED_PATTERNS=()
_tracked_patterns() {
    TRACKED_PATTERNS=()
    local d
    for d in "$@"; do
        TRACKED_PATTERNS+=("$d/*.cpp" "$d/*.h" "$d/*.hpp" "$d/*.mm")
    done
}

# tracked_sources <dir>...  — newline-delimited. For `while IFS= read -r` loops.
tracked_sources() {
    _tracked_patterns "$@"
    git ls-files -- "${TRACKED_PATTERNS[@]}"
}

# format_sources_z <dir>...  — NUL-delimited, for `xargs -0` (issue #43: a path
# containing whitespace must reach the tool as ONE argument, not two).
#
# TRACKED **PLUS** UNTRACKED-BUT-NOT-IGNORED (issue #58). Plain `git ls-files`
# enumerates the INDEX, so a source that had not been `git add`ed yet was never
# format-checked and `lint.sh format` still exited 0 — a false green. The window is
# narrow (the PostToolUse hook formats on write, so agent- and editor-created files
# are already clean, and CI only ever sees tracked files) but it is exactly the
# no-op-gate shape #30 was, so it is closed rather than documented.
#
# `--others` adds precisely what `git status` would call untracked, and
# `--exclude-standard` keeps .gitignore / .git/info/exclude / the global excludes as
# the sole authority on what is ignored — WHAT COUNTS AS IGNORED IS UNCHANGED, which
# is what keeps build-*/ and JUCE/ out. `--cached` and `--others` are disjoint by
# git's own definition (`--others` means "not in the index"), so one invocation
# yields no duplicates and needs no dedup pass.
#
# NOT APPLIED TO THE TIDY ENUMERATION, deliberately: `check_db_coverage` compares
# git's view against the COMPILE DATABASE, and an untracked .cpp is legitimately
# absent from a CMakeLists until it is added. Folding untracked files in there would
# manufacture coverage holes instead of finding them, turning #42's gate into noise.
format_sources_z() {
    _tracked_patterns "$@"
    git ls-files -z --cached --others --exclude-standard -- "${TRACKED_PATTERNS[@]}"
}

ensure_compile_db() {
    local reconfigure="$1"
    if [[ "$reconfigure" == "yes" || ! -f "$TIDY_BUILD_DIR/compile_commands.json" ]]; then
        echo "== cmake --preset $TIDY_PRESET  (single-arch compile database) =="
        # Configure only. The tidy tree is never built: juceaide runs during
        # configure and generates the JUCE headers clang-tidy needs.
        cmake --preset "$TIDY_PRESET" >/dev/null || {
            echo "error: 'cmake --preset $TIDY_PRESET' failed" >&2; return 1; }
    fi
    [[ -f "$TIDY_BUILD_DIR/compile_commands.json" ]] || {
        echo "error: $TIDY_BUILD_DIR/compile_commands.json not produced" >&2; return 1; }
}

# db_translation_units — repo-relative .cpp/.mm paths present in the compile
# database, restricted to SRC_DIRS. Extracted with grep/sed rather than jq or
# python so the gate keeps working on a box that has neither.
db_translation_units() {
    local db="$TIDY_BUILD_DIR/compile_commands.json" p
    grep -o '"file"[[:space:]]*:[[:space:]]*"[^"]*"' "$db" \
        | sed -e 's/^.*:[[:space:]]*"//' -e 's/"$//' \
        | while IFS= read -r p; do
            # CMake writes absolute paths; tolerate build-dir-relative ones too.
            [[ "$p" == /* ]] || p="$TIDY_BUILD_DIR/$p"
            p="${p#"$REPO_ROOT"/}"
            p="${p#"$REPO_ROOT_PHYS"/}"
            case "$p" in /*) continue ;; esac   # outside the repo (JUCE, SDKs)
            case "$p" in *.cpp|*.mm) ;; *) continue ;; esac
            local d
            for d in "${SRC_DIRS[@]}"; do
                case "$p" in "$d"/*) printf '%s\n' "$p"; break ;; esac
            done
        done | sort -u
}

# check_db_coverage — issue #42. Shared by BOTH database-driven gates (`tidy` and
# `warnings`), which is why the name is no longer tidy-specific.
#
# run-clang-tidy lints COMPILE-DATABASE entries; git is what actually defines
# "our code". A tracked .cpp that nobody added to a CMake target is absent from
# the database, so the DB-driven path skips it in silence while the serial
# fallback path lints it — two answers to "what is covered". This makes the two
# agree loudly.
#
# It FAILS rather than warns: this is a gate, and a gate that quietly checks less
# than you think is the same failure shape as #30's no-op format hook. The fix is
# one line in a CMakeLists. If an omission is ever deliberate, set
# ARPBOX_TIDY_ALLOW_UNLINTED=1 to downgrade it to a warning — deliberately an
# explicit, visible opt-out rather than a default.
check_db_coverage() {
    local db_list tracked_count db_count missing=() f
    db_list="$(db_translation_units)"
    db_count="$(printf '%s' "$db_list" | grep -c . )"

    tracked_count=0
    while IFS= read -r f; do
        case "$f" in *.cpp|*.mm) ;; *) continue ;; esac
        tracked_count=$((tracked_count + 1))
        printf '%s\n' "$db_list" | grep -Fxq -- "$f" || missing+=("$f")
    done < <(tracked_sources "${SRC_DIRS[@]}")

    echo "== coverage: $db_count compile-database TUs vs $tracked_count tracked .cpp/.mm sources =="
    [[ ${#missing[@]} -eq 0 ]] && return 0

    {
        echo
        echo "================================================================================"
        echo " LINT COVERAGE HOLE: ${#missing[@]} git-tracked source(s) are NOT in the compile"
        echo " database, so the database-driven gates (clang-tidy, warnings) would skip"
        echo " them without saying so (issue #42):"
        printf '   - %s\n' "${missing[@]}"
        echo
        echo " Add each file to its target's CMakeLists.txt, or re-run with --reconfigure if"
        echo " the database is merely stale. To accept the omission deliberately, set"
        echo " ARPBOX_TIDY_ALLOW_UNLINTED=1 (downgrades this to a warning)."
        echo "================================================================================"
    } >&2

    if [[ "${ARPBOX_TIDY_ALLOW_UNLINTED:-0}" == "1" ]]; then
        echo "warning: continuing anyway (ARPBOX_TIDY_ALLOW_UNLINTED=1)" >&2
        return 0
    fi
    return 1
}

# ---------------------------------------------------------------------------
# the COMPILER-warning gate (issue #79)
# ---------------------------------------------------------------------------
#
# WHY IT EXISTS. `lint.sh tidy` and `lint.sh format` both exited 0 with three live
# `-Wswitch-enum` warnings in the tree (#79, #70): clang-tidy runs its own check
# set and never sees compiler diagnostics, and clang-format sees text. So a
# warning class the codebase DELIBERATELY relies on — `engine/sequencer/StepLogic.cpp`
# documents that its 39-condition ladder is written as a comparison chain rather
# than a switch *precisely because* `-Wswitch-enum` is on — was enforced by nothing
# at all. `-Wswitch-enum` arrives via `juce_recommended_warning_flags`; this gate is
# what makes it load-bearing instead of decorative.
#
# WHY IT CANNOT GO VACUOUS. A gate whose silence is indistinguishable from its
# approval is worse than no gate: #30 (`2>/dev/null; exit 0` made the format hook a
# silent no-op for five phases) and the `--no-tests=error` lesson (`ctest -L
# determinism` exits 0 when the label matches nothing) are both that shape. So four
# independent guards run BEFORE any warning count is believed, and each one FAILS
# the gate rather than warning. G0-G4 test COMPONENTS; G5 tests the join between the
# real compiler and the extractor, which is the one thing no component test can reach
# (issue #87 — five green guards were compatible with a fully blind gate):
#
#   G1  parser self-test — `warn_parser_selftest` pushes a FIXED synthetic build log
#       through the very extractor the real log goes through, and asserts it keeps
#       the two ARPBOX diagnostics while dropping the JUCE-internal one, the `note:`
#       continuation and the `error:` line. A regex that stopped matching anything
#       can no longer read as "clean".
#   G2  flags actually present — `warn_assert_sentinel_flags` asserts every in-scope
#       source has at least one compile-database entry carrying each flag in
#       WARN_SENTINEL_FLAGS, with no `-Wno-<flag>` and no bare `-w` cancelling it. If
#       -Wswitch-enum ever stops reaching our TUs, the gate says so instead of
#       reporting zero warnings that it was structurally blind to.
#   G5  end-to-end canary — `warn_canary` makes the REAL toolchain emit a REAL
#       -Wswitch-enum diagnostic with the REAL flags of an in-scope TU, and asserts
#       the extractor keeps it. See its own note below for why the other five guards
#       can all be green while the gate sees nothing.
#   G3  a real recompile happened — the gate DELETES the object file of every
#       in-scope TU, builds, then asserts every one of those objects exists again.
#       An up-to-date no-op build (by far the likeliest way to "find" zero warnings)
#       cannot pass, and neither can a TU whose target is excluded from the default
#       build. Object existence is used deliberately in preference to grepping
#       ninja's progress lines: it does not depend on the phrasing of any build-tool
#       message, and it is a direct consequence of "we deleted it, one build ran,
#       it is back" — i.e. its diagnostics are in the log we captured.
#   G4  build status — the build's exit status is captured into a variable and
#       checked on its own line. No `|| true`, no `2>/dev/null`, no dependence on a
#       pipeline's exit status. A failed build fails the gate; it never degrades to
#       "no warnings found".
#
# G0, inherited: `check_db_coverage` (#42) already proves every git-tracked source
# under SRC_DIRS is in the compile database, so "zero warnings" cannot silently mean
# "that file is in no CMake target".
#
# SCOPE. In-scope == compile-database entries whose SOURCE is under SRC_DIRS — the
# same set the tidy gate covers, and the set whose shipping targets carry
# `juce_recommended_warning_flags`. Diagnostics are filtered on the path in the
# DIAGNOSTIC, so a warning raised inside a JUCE header while compiling one of our
# TUs is excluded (not ours to fix, and it would make the gate noise) while a
# warning in one of our own headers is kept.
#
# tests/ is deliberately OUT of scope — a real limitation, tracked as issue #86, not
# an oversight: the arpbox_tests target carries no `-W` flags at all today, so
# putting tests/ in scope would advertise coverage that does not exist (the #30
# shape again). Note the exclusion is per-TARGET, not per-file: app/SynthSlot.cpp
# compiles into BOTH ARPBOX (fully flagged) and arpbox_tests (unflagged), and this
# gate covers it via the ARPBOX entry — which is exactly why G2 asserts the sentinel
# flags per SOURCE rather than per entry. #86 carries the measured cost of closing
# it; do not re-derive it here.
#
# WHICH TREE IT BUILDS. The `tidy` preset's build-tidy/ tree, reused rather than
# forked: single-arch (so each diagnostic appears ONCE, not twice as in the
# universal `build/` tree), already the home of the compile database this gate
# reads, and already configured by `ensure_compile_db`. Unlike the tidy gate, this
# one does build that tree.

# Warning flags whose presence is asserted before the gate believes a clean result.
# -Wswitch-enum is the one with documented in-repo dependents (#70/#79, StepLogic.cpp);
# -Wall is the canary that the recommended-warning set reached the TU at all.
WARN_SENTINEL_FLAGS=(-Wswitch-enum -Wall)

# Build log location: inside the gate's own build tree, so a developer (or a CI
# artefact step) can read the full context of a failure after the fact.
WARN_LOG="$TIDY_BUILD_DIR/arpbox-warnings.log"

# extract_our_warnings — stdin: a build log. stdout: the `warning:` diagnostic lines
# whose own file path is a source/header under SRC_DIRS. Both REPO_ROOT and its
# symlink-resolved twin are accepted, for the same reason db_translation_units does.
extract_our_warnings() {
    local dir_alt escaped_root escaped_phys
    dir_alt="$(IFS='|'; echo "${SRC_DIRS[*]}")"
    escaped_root="$(regex_escape "$REPO_ROOT")"
    escaped_phys="$(regex_escape "$REPO_ROOT_PHYS")"
    grep -E "^(${escaped_root}|${escaped_phys})/(${dir_alt})/[^:]*:[0-9]+:[0-9]+: warning:" || true
}

# G1 — prove the extractor extracts. Fixture-driven, so it needs no compiler and
# runs in milliseconds; it is the difference between "no warnings" and "no idea".
warn_parser_selftest() {
    local fixture kept expected=2
    fixture="[1/2] Building CXX object engine/CMakeFiles/arpbox_engine.dir/graph/Transport.cpp.o
${REPO_ROOT}/engine/graph/Transport.cpp:129:13: warning: enumeration values 'a' and 'b' not explicitly handled in switch [-Wswitch-enum]
    switch (command.type)
            ^
${REPO_ROOT}/engine/graph/Transport.h:42:5: note: expanded from here
${REPO_ROOT}/JUCE/modules/juce_core/juce_core.cpp:12:1: warning: JUCE-internal, not ours [-Wunused-variable]
${REPO_ROOT}/tests/unit/SomeTest.cpp:7:1: warning: tests are out of scope [-Wunused-variable]
${REPO_ROOT}/app/Main.cpp:7:3: warning: second in-scope diagnostic [-Wshadow]
${REPO_ROOT}/app/Main.cpp:8:3: error: an error is not a warning"

    kept="$(printf '%s\n' "$fixture" | extract_our_warnings | grep -c . )"
    if [[ "$kept" != "$expected" ]]; then
        {
            echo "error: warnings-gate self-test FAILED — the diagnostic extractor kept"
            echo "       $kept line(s) of the fixture, expected $expected."
            echo "       The gate is therefore NOT trustworthy and refuses to report a"
            echo "       result: a silent extractor would report every tree as clean."
        } >&2
        return 1
    fi
    echo "== self-test: extractor kept $kept/$expected fixture diagnostics (JUCE, tests/, note:, error: correctly dropped) =="
}

# db_scope_entries — one TAB-separated line per in-scope compile-database entry:
#   <repo-relative source>  <build-dir-relative object>  <sentinel flags present, comma-joined>
# A source compiled into two targets yields two lines (app/SynthSlot.cpp does: once
# into ARPBOX with the warning flags, once into arpbox_tests without them).
db_scope_entries() {
    local db="$TIDY_BUILD_DIR/compile_commands.json" dir_alt sentinels
    dir_alt="$(IFS='|'; echo "${SRC_DIRS[*]}")"
    sentinels="$(IFS=','; echo "${WARN_SENTINEL_FLAGS[*]}")"
    # NOTE: awk user-function calls must NOT have a space before the `(` — one-true-awk
    # (macOS /usr/bin/awk) parses `unquote ($0)` as a concatenation and dies with
    # "can't read value of unquote; it's a function". So this block deliberately does
    # not follow the project's C++ space-before-paren style.
    awk -v root="$REPO_ROOT/" -v phys="$REPO_ROOT_PHYS/" -v dirs="$dir_alt" -v sent="$sentinels" '
        function unquote(line) { sub(/^[^:]*:[[:space:]]*"/, "", line); sub(/",?$/, "", line); return line }
        BEGIN { n = split(sent, S, ",") }
        /^\{/                                 { file = ""; out = ""; cmd = "" }
        /^[[:space:]]*"file"[[:space:]]*:/    { file = unquote($0) }
        /^[[:space:]]*"output"[[:space:]]*:/  { out  = unquote($0) }
        /^[[:space:]]*"command"[[:space:]]*:/ { cmd  = $0 }
        /^\}/ {
            if (file == "" || out == "") next
            rel = file
            if (index(rel, root) == 1)      rel = substr(rel, length(root) + 1)
            else if (index(rel, phys) == 1) rel = substr(rel, length(phys) + 1)
            else next                                  # outside the repo (JUCE, SDKs)
            if (rel !~ "^(" dirs ")/") next
            if (rel !~ /\.(cpp|mm)$/) next
            # A bare `-w` disables EVERY warning, so it cancels each sentinel just as
            # surely as a targeted -Wno- does — and it would otherwise sail past a
            # presence-only check (issue #87). Matched as a whole token: `-Wall` and
            # `-Wno-…` are capital-W and cannot collide, and the trailing-quote branch
            # catches a `-w` that is the final argument of the command.
            # (No apostrophes in this awk block: it is bash-single-quoted.)
            suppressed = (cmd ~ /[ ]-w([ ]|")/)
            found = ""
            for (i = 1; i <= n; i++)
            {
                neg = "-Wno-" substr(S[i], 3)          # -Wswitch-enum -> -Wno-switch-enum
                if (! suppressed && index(cmd, S[i]) > 0 && index(cmd, neg) == 0)
                    found = found (found == "" ? "" : ",") S[i]
            }
            print rel "\t" out "\t" found
        }
    ' "$db"
}

# G2 — every in-scope SOURCE must be compiled with every sentinel flag by at least
# one entry. Per-source rather than per-entry on purpose: app/SynthSlot.cpp is built
# twice and only the flag-carrying build needs to exist for the source to be covered.
warn_assert_sentinel_flags() {
    local entries="$1" flag src missing=()
    for flag in "${WARN_SENTINEL_FLAGS[@]}"; do
        while IFS= read -r src; do
            [[ -n "$src" ]] || continue
            printf '%s\n' "$entries" \
                | awk -F'\t' -v s="$src" -v f="$flag" \
                    '$1 == s && index("," $3 ",", "," f ",") > 0 { ok = 1 } END { exit ok ? 0 : 1 }' \
                || missing+=("$flag  $src")
        done < <(printf '%s\n' "$entries" | cut -f1 | sort -u)
    done

    [[ ${#missing[@]} -eq 0 ]] && {
        echo "== sentinel flags present on every in-scope source: ${WARN_SENTINEL_FLAGS[*]} =="
        return 0
    }

    {
        echo
        echo "================================================================================"
        echo " WARNINGS GATE IS BLIND: ${#missing[@]} (flag, source) pair(s) where the flag does"
        echo " not reach the translation unit, so this gate could not see that class of"
        echo " warning even if it were present (issue #79):"
        printf '   - %s\n' "${missing[@]}"
        echo
        echo " Usually this means the target stopped linking juce::juce_recommended_warning_flags,"
        echo " or a -Wno- / bare -w override was added. Fix the target, or drop the flag from"
        echo " WARN_SENTINEL_FLAGS in this script and say why — but do not let the gate"
        echo " keep reporting green for a class it cannot observe."
        echo "================================================================================"
    } >&2
    return 1
}

# G5 — the END-TO-END canary (issue #87).
#
# WHAT THE OTHER GUARDS CANNOT DO. G0-G4 each test a COMPONENT. Not one of them
# proves that the string the REAL compiler prints is a string `extract_our_warnings`
# accepts. The concrete failure: if a toolchain or CMake-generator change made
# diagnostics carry build-dir-relative paths (`../engine/graph/Transport.cpp:129:`)
# instead of absolute ones, the extractor's `^$REPO_ROOT/(app|engine|…)/` anchor
# would silently drop EVERY real warning — while G1 still passed on its synthetic
# fixture, G2 still found the sentinel flags, G3 still confirmed 25 recompiles and G4
# still saw a clean build. Five guards green, gate blind, "0 compiler warnings"
# printed. That is #30's shape one level up.
#
# WHY THIS IS NOT A SIXTH COMPONENT TEST. G1 proves the regex handles a string WE
# wrote. This proves the COMPILER writes a string the regex handles — it makes the
# real toolchain, invoked with the real flags a real in-scope TU is built with, emit
# a real diagnostic, and pushes it through the same `extract_our_warnings` the real
# verdict goes through. It is the JOIN between the two halves that no component test
# can cover, and it is the only guard here whose input nobody in this file authored.
#
# The provoked class is `-Wswitch-enum` deliberately: it is the class #70/#79 were
# about, the one StepLogic.cpp documents a design decision against, and the one whose
# silent loss this whole gate exists to prevent. A `default:` arm in the canary is
# what keeps it -Wswitch-enum rather than the broader -Wswitch — i.e. the canary is
# shaped exactly like the bug this cluster fixed.
#
# The canary TU lives in the top-level tree of the entry whose flags it borrows, and
# not under tests/ (out of scope, #86) or in $TMPDIR: the path shape IS the thing
# under test, so it has to sit where a real in-scope source sits. It is a mktemp'd
# extensionless dotfile compiled with `-x c++ -fsyntax-only`, so it matches no git or
# clang-format pathspec, writes no object, cannot pollute build-tidy/, and is removed
# by a trap on EXIT/INT/TERM as well as on every return path.
WARN_CANARY_FILE=""

warn_canary_cleanup() {
    [[ -n "$WARN_CANARY_FILE" ]] && rm -f "$WARN_CANARY_FILE"
    WARN_CANARY_FILE=""
}

warn_canary() {
    local entries="$1" chosen src obj raw diag kept
    trap warn_canary_cleanup EXIT INT TERM

    # Borrow from the first in-scope entry carrying EVERY sentinel flag, so the canary
    # is compiled the way a fully-flagged real TU is compiled.
    chosen="$(printf '%s\n' "$entries" | awk -F'\t' -v want="$(IFS=','; echo "${WARN_SENTINEL_FLAGS[*]}")" '
        { n = split(want, W, ","); ok = 1
          for (i = 1; i <= n; i++) if (index("," $3 ",", "," W[i] ",") == 0) ok = 0
          if (ok) { print; exit } }')"
    if [[ -z "$chosen" ]]; then
        echo "error: canary: no in-scope entry carries all of ${WARN_SENTINEL_FLAGS[*]}" >&2
        echo "       (G2 should have caught this first — the gate is inconsistent, not clean)." >&2
        return 1
    fi
    src="$(printf '%s' "$chosen" | cut -f1)"
    obj="$(printf '%s' "$chosen" | cut -f2)"

    # The exact command the build uses, from ninja rather than the compile database:
    # `-t commands` emits it already shell-quoted, so it needs no JSON unescaping and
    # `-D…=\"0.1.0\"` survives intact. ninja is necessarily present — `cmake --build`
    # on this tree is ninja.
    # Invoked twice on purpose: stdout is about to be `eval`ed, so ninja's stderr must
    # be collected on a separate pass rather than merged into it. Cheap (a graph query,
    # no compilation) and it keeps a ninja error message out of the eval.
    local ninja_err
    ninja_err="$(ninja -C "$TIDY_BUILD_DIR" -t commands "$obj" 2>&1 >/dev/null)"
    raw="$(ninja -C "$TIDY_BUILD_DIR" -t commands "$obj" 2>/dev/null | tail -1)"
    if [[ -z "$raw" ]]; then
        echo "error: canary: could not recover the compile command for $obj" >&2
        [[ -n "$ninja_err" ]] && echo "       ninja said: $ninja_err" >&2
        echo "       Refusing to report a verdict the gate cannot back up." >&2
        return 1
    fi

    # Token-wise filtering via the shell's own parsing of ninja's quoting, so a repo
    # path containing spaces cannot split an argument. Dropping -o/-c/-M* is what
    # keeps the canary from writing an object or a stale .d into build-tidy/.
    local -a argv=() kept_argv=()
    eval "argv=($raw)"
    local a skip_next=0
    for a in "${argv[@]}"; do
        if [[ "$skip_next" -eq 1 ]]; then skip_next=0; continue; fi
        case "$a" in
            -o|-MT|-MF) skip_next=1; continue ;;
            -c|-MD) continue ;;
            "$REPO_ROOT/$src"|"$REPO_ROOT_PHYS/$src") continue ;;
        esac
        kept_argv+=("$a")
    done

    WARN_CANARY_FILE="$(mktemp "$REPO_ROOT/${src%%/*}/.arpbox-warnings-canary-XXXXXX")" || {
        echo "error: canary: mktemp failed under $REPO_ROOT/${src%%/*}/" >&2; return 1; }

    # A switch that COVERS every enumerator through a `default:` but does not list
    # them: -Wswitch-enum fires, -Wswitch does not. Prototyped + static so no
    # -Wmissing-prototypes / -Wunused-function noise rides along.
    cat > "$WARN_CANARY_FILE" <<'CANARY'
enum class ArpboxCanaryEnum { alpha, beta };
static int arpboxWarningsCanary (ArpboxCanaryEnum e)
{
    switch (e)
    {
    case ArpboxCanaryEnum::alpha: return 1;
    default: break;
    }
    return 0;
}
int arpboxWarningsCanaryEntry ();
int arpboxWarningsCanaryEntry () { return arpboxWarningsCanary (ArpboxCanaryEnum::beta); }
CANARY

    diag="$("${kept_argv[@]}" -fsyntax-only -x c++ "$WARN_CANARY_FILE" 2>&1)"
    kept="$(printf '%s\n' "$diag" | extract_our_warnings | grep -c . )"
    warn_canary_cleanup

    if [[ "$kept" -ge 1 ]] && printf '%s\n' "$diag" | extract_our_warnings | grep -q -- '-Wswitch-enum'; then
        echo "== canary: real toolchain emitted a real -Wswitch-enum diagnostic and the extractor kept it =="
        return 0
    fi

    {
        echo
        echo "================================================================================"
        echo " WARNINGS-GATE CANARY FAILED — THE DIAGNOSTIC FORMAT HAS CHANGED (issue #87)."
        echo
        if printf '%s\n' "$diag" | grep -q ': error:'; then
            echo " The canary translation unit did not COMPILE with the flags borrowed from"
            echo " $src, so nothing can be concluded about the extractor:"
        elif [[ -z "$diag" ]]; then
            echo " The compiler emitted NOTHING for a translation unit that must warn. The"
            echo " warning was suppressed (a bare -w or a -Wno- this gate does not know about)"
            echo " or the compiler no longer diagnoses this class at all:"
        else
            echo " The compiler DID warn, but \`extract_our_warnings\` did not keep the"
            echo " diagnostic — so it would not keep a real one either, and this gate would"
            echo " report every tree as clean. Update the extractor's pattern to match the"
            echo " format below; do NOT relax this check."
        fi
        echo
        printf '%s\n' "${diag:-(no compiler output at all)}" | sed 's/^/   /'
        echo
        echo " Extractor expects: <abs repo path>/(${SRC_DIRS[*]})/<file>:LINE:COL: warning: …"
        echo "================================================================================"
    } >&2
    return 1
}

run_warnings() {
    local reconfigure="no" jobs=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --reconfigure) reconfigure="yes"; shift ;;
            -j) jobs="$2"; shift 2 ;;
            -j*) jobs="${1#-j}"; shift ;;
            *) echo "error: unknown warnings option '$1'" >&2; return 2 ;;
        esac
    done

    ensure_compile_db "$reconfigure" || return 1
    check_db_coverage || return 1          # G0
    warn_parser_selftest || return 1      # G1

    local entries
    entries="$(db_scope_entries)"
    if [[ -z "$entries" ]]; then
        echo "error: no in-scope compile-database entries found under: ${SRC_DIRS[*]}" >&2
        echo "       Refusing to report 'no warnings' about nothing (try --reconfigure)." >&2
        return 1
    fi

    warn_assert_sentinel_flags "$entries" || return 1   # G2
    warn_canary "$entries" || return 1                  # G5 — before the build: one
                                                        # small -fsyntax-only compile,
                                                        # so a blind gate fails fast.

    # G3 part 1 — invalidate. Only OUR objects: the JUCE module TUs stay cached, which
    # is what keeps the run to ~25 s and keeps JUCE's own diagnostics out of the log.
    local objects obj object_count=0
    objects="$(printf '%s\n' "$entries" | cut -f2 | sort -u)"
    while IFS= read -r obj; do
        [[ -n "$obj" ]] || continue
        rm -f "$TIDY_BUILD_DIR/$obj"
        object_count=$((object_count + 1))
    done <<< "$objects"

    echo "== warnings gate: rebuilding $object_count in-scope object(s) in $TIDY_BUILD_DIR =="
    echo "== scope: ${SRC_DIRS[*]}  (tests/ excluded — arpbox_tests carries no -W flags; issue #86) =="

    # G4 — status captured and checked on its own, never inferred from a pipeline.
    local build_status=0
    cmake --build "$TIDY_BUILD_DIR" ${jobs:+-j "$jobs"} > "$WARN_LOG" 2>&1 || build_status=$?
    if [[ "$build_status" -ne 0 ]]; then
        echo >&2
        echo "error: the build FAILED (exit $build_status) — the warnings gate reports no" >&2
        echo "       verdict on a tree that does not compile. Full log: $WARN_LOG" >&2
        tail -n 40 "$WARN_LOG" >&2
        return 1
    fi

    # G3 part 2 — every object we deleted must be back, or this build did not compile
    # what the gate claims to have inspected.
    local not_rebuilt=()
    while IFS= read -r obj; do
        [[ -n "$obj" ]] || continue
        [[ -f "$TIDY_BUILD_DIR/$obj" ]] || not_rebuilt+=("$obj")
    done <<< "$objects"

    if [[ ${#not_rebuilt[@]} -gt 0 ]]; then
        {
            echo
            echo "================================================================================"
            echo " WARNINGS GATE DID NOT ACTUALLY BUILD ${#not_rebuilt[@]} of its $object_count translation unit(s)."
            echo " Their objects were deleted before the build and the build did not recreate"
            echo " them, so any 'clean' result would be about files nobody compiled (#79):"
            printf '   - %s\n' "${not_rebuilt[@]}"
            echo
            echo " Most likely the owning target is EXCLUDE_FROM_ALL, or the default build"
            echo " target set no longer covers it."
            echo "================================================================================"
        } >&2
        return 1
    fi
    echo "== verified: all $object_count in-scope object(s) were recompiled by this run =="

    local found count
    found="$(extract_our_warnings < "$WARN_LOG")"
    count="$(printf '%s' "$found" | grep -c . )"

    if [[ "$count" -eq 0 ]]; then
        echo "== 0 compiler warnings in ${SRC_DIRS[*]} (log: $WARN_LOG) =="
        return 0
    fi

    {
        echo
        echo "================================================================================"
        echo " $count COMPILER WARNING(S) in ARPBOX sources — this is a gate (issue #79):"
        echo
        printf '%s\n' "$found"
        echo
        echo " Full build log: $WARN_LOG"
        echo "================================================================================"
    } >&2
    return 1
}

# ---------------------------------------------------------------------------
# modes
# ---------------------------------------------------------------------------

run_tidy() {
    local reconfigure="no" jobs=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --reconfigure) reconfigure="yes"; shift ;;
            -j) jobs="$2"; shift 2 ;;
            -j*) jobs="${1#-j}"; shift ;;
            *) echo "error: unknown tidy option '$1'" >&2; return 2 ;;
        esac
    done

    local tidy_bin runner
    tidy_bin="$(require_tool clang-tidy)" || return 1
    runner="$(resolve_tool run-clang-tidy)" || runner=""

    ensure_compile_db "$reconfigure" || return 1

    # Before linting anything, prove the DB-driven file set and the git-tracked
    # file set agree (issue #42). Checked for BOTH paths below: a mismatch means
    # the database is incomplete, which is a problem regardless of which runner
    # ends up doing the work.
    check_db_coverage || return 1

    # Anchored on the repo root so JUCE/, tests/ and the generated sources under
    # build-tidy/ are excluded, and so a vendored path that merely *contains*
    # "ui" or "app" cannot sneak in.
    local dir_alt escaped_root filter
    dir_alt="$(IFS='|'; echo "${SRC_DIRS[*]}")"
    escaped_root="$(regex_escape "$REPO_ROOT")"
    filter="^${escaped_root}/(${dir_alt})/.*\.(cpp|mm)$"

    echo "== clang-tidy: $tidy_bin =="
    "$tidy_bin" --version | sed -n '1,2p'

    if [[ -n "$runner" ]]; then
        echo "== run-clang-tidy -p build-tidy  (filter: ${dir_alt}) =="
        # -clang-tidy-binary is required: run-clang-tidy defaults to whatever
        # 'clang-tidy' is on PATH, and on a stock macOS box that is nothing.
        # shellcheck disable=SC2086
        "$runner" -p "$TIDY_BUILD_DIR" \
            -clang-tidy-binary "$tidy_bin" \
            -quiet ${jobs:+-j "$jobs"} "$filter"
        return $?
    fi

    # Fallback when run-clang-tidy is unavailable: serial per-file pass over the
    # git-tracked sources (what CI did before this script existed).
    echo "== run-clang-tidy not found — serial clang-tidy pass =="
    local status=0 f
    while IFS= read -r f; do
        case "$f" in *.cpp|*.mm) ;; *) continue ;; esac
        echo "== clang-tidy $f =="
        "$tidy_bin" -p "$TIDY_BUILD_DIR" "$f" || status=1
    done < <(tracked_sources "${SRC_DIRS[@]}")
    return "$status"
}

run_format() {
    local mode="$1" fmt_bin
    fmt_bin="$(require_tool clang-format)" || return 1
    echo "== clang-format: $fmt_bin =="
    "$fmt_bin" --version

    if [[ "$mode" == "fix" ]]; then
        cat >&2 <<'WARN'
================================================================================
 REFUSING to reformat the whole tree without an explicit override.
 A repo-wide `-i` pass rewrites every file that has drifted, so it belongs in its
 OWN commit and nowhere near a change under review. That decision is the user's,
 not a lint step's.
 If you have that decision, re-run with ARPBOX_ALLOW_FORMAT_FIX=1.
 (To format just the files you touched, use `lint.sh format-file <path>...`.)
================================================================================
WARN
        [[ "${ARPBOX_ALLOW_FORMAT_FIX:-0}" == "1" ]] || return 2
        # NUL-delimited: a path with a space must arrive as one argument (#43).
        format_sources_z "${FORMAT_DIRS[@]}" | xargs -0 "$fmt_bin" -i
        return $?
    fi

    # Check mode. This is the gate: --Werror makes any drift a non-zero exit, and
    # CI's format job is blocking on it, so the tree cannot drift again (#30). The
    # file set includes untracked-but-not-ignored sources (#58), so a file created
    # outside the write hook can no longer pass through unchecked.
    format_sources_z "${FORMAT_DIRS[@]}" | xargs -0 "$fmt_bin" --dry-run --Werror
}

# format-file <path>...  — in-place format of specific files. Used by the
# PostToolUse hook in .claude/settings.json so the hook does not carry its own
# copy of resolve_tool() that could drift from this one (#30).
#
# Quiet on success; a missing formatter is a hard, visible error, never swallowed.
run_format_file() {
    local fmt_bin f status=0
    [[ $# -gt 0 ]] || { echo "error: format-file needs at least one path" >&2; return 2; }
    fmt_bin="$(require_tool clang-format)" || return 1

    for f in "$@"; do
        # Resolve against the CALLER's cwd, not the repo root we cd'd to.
        [[ "$f" == /* ]] || f="$INVOKED_PWD/$f"
        case "$f" in *.cpp|*.h|*.hpp|*.mm) ;; *) continue ;; esac
        if [[ ! -f "$f" ]]; then
            echo "error: format-file: no such file: $f" >&2
            status=1
            continue
        fi
        "$fmt_bin" -i "$f" || status=1
    done
    return "$status"
}

main() {
    local mode="${1:-tidy}"
    [[ $# -gt 0 ]] && shift
    case "$mode" in
        tidy)        run_tidy "$@" ;;
        warnings)    run_warnings "$@" ;;
        format)      run_format check ;;
        format-fix)  run_format fix ;;
        format-file) run_format_file "$@" ;;
        tools)
            local t
            for t in clang-tidy clang-format run-clang-tidy; do
                printf '%-16s %s\n' "$t" "$(resolve_tool "$t" || echo '(not found)')"
            done
            ;;
        -h|--help|help)
            sed -n '3,45p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            ;;
        *) echo "error: unknown mode '$mode' (tidy|warnings|format|format-fix|format-file|tools)" >&2; return 2 ;;
    esac
}

main "$@"
