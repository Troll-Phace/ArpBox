#!/usr/bin/env bash
#
# ARPBOX lint driver — the ONE mechanism shared by the /run-lint skill, CI's
# `clang-tidy (single-arch DB, non-blocking)` job, CI's blocking `clang-format`
# job, and the format-on-save PostToolUse hook (issues #31, #30).
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
#   lint.sh format                        clang-format --dry-run --Werror (CHECK only)
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
# CI's FORMAT job is blocking, because format drift is exactly what #30 was.
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

# tracked_sources_z <dir>...  — NUL-delimited, for `xargs -0` (issue #43: a path
# containing whitespace must reach the tool as ONE argument, not two).
tracked_sources_z() {
    _tracked_patterns "$@"
    git ls-files -z -- "${TRACKED_PATTERNS[@]}"
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

# check_tidy_coverage — issue #42.
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
check_tidy_coverage() {
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
        echo " database, so clang-tidy would skip them without saying so (issue #42):"
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
    check_tidy_coverage || return 1

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
        tracked_sources_z "${FORMAT_DIRS[@]}" | xargs -0 "$fmt_bin" -i
        return $?
    fi

    # Check mode. This is the gate: --Werror makes any drift a non-zero exit, and
    # CI's format job is blocking on it, so the tree cannot drift again (#30).
    tracked_sources_z "${FORMAT_DIRS[@]}" | xargs -0 "$fmt_bin" --dry-run --Werror
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
            sed -n '3,36p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            ;;
        *) echo "error: unknown mode '$mode' (tidy|format|format-fix|tools)" >&2; return 2 ;;
    esac
}

main "$@"
