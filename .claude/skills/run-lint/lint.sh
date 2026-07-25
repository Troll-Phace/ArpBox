#!/usr/bin/env bash
#
# ARPBOX lint driver — the ONE mechanism shared by the /run-lint skill and CI's
# `clang-tidy (single-arch DB, non-blocking)` job (issue #31).
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
#   lint.sh format-fix                    clang-format -i  (rewrites files — see #30)
#   lint.sh tools                         print resolved tool paths and exit
#
# Exit status: non-zero if any finding was reported (.clang-tidy sets
# WarningsAsErrors: '*'), so this is usable as a gate. CI's lint job keeps
# `continue-on-error: true` at the job level while the tree is still a scaffold.
#
# Tool resolution: clang-tidy / clang-format / run-clang-tidy are NOT on PATH on
# a stock macOS box — the Xcode toolchain does not ship them (`xcrun -f clang-tidy`
# fails) and they come from Homebrew LLVM. resolve_tool() therefore probes
# $ARPBOX_LLVM_BIN, the Homebrew LLVM prefixes, xcrun, then PATH, and reports
# what it used instead of failing obscurely.

set -uo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || (cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd))"
cd "$REPO_ROOT" || exit 1

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

# tracked_sources <dir>...  — git-tracked C++ sources/headers under those trees.
tracked_sources() {
    local patterns=() d
    for d in "$@"; do
        patterns+=("$d/*.cpp" "$d/*.h" "$d/*.hpp" "$d/*.mm")
    done
    git ls-files -- "${patterns[@]}"
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
 REFUSING to reformat without an explicit override.
 Issue #30 is OPEN: the tree has repo-wide clang-format drift, so `-i` rewrites
 large tracts of code untouched by the current change and buries the real diff.
 The repo-wide reformat decision is the user's, not a lint step's.
 If you have that decision, re-run with ARPBOX_ALLOW_FORMAT_FIX=1.
================================================================================
WARN
        [[ "${ARPBOX_ALLOW_FORMAT_FIX:-0}" == "1" ]] || return 2
        tracked_sources "${FORMAT_DIRS[@]}" | xargs "$fmt_bin" -i
        return $?
    fi

    # Check mode. Expect PRE-EXISTING findings until #30 is resolved; treat new
    # ones as belonging to the change under review, not as a green/red gate.
    tracked_sources "${FORMAT_DIRS[@]}" | xargs "$fmt_bin" --dry-run --Werror
}

main() {
    local mode="${1:-tidy}"
    [[ $# -gt 0 ]] && shift
    case "$mode" in
        tidy)       run_tidy "$@" ;;
        format)     run_format check ;;
        format-fix) run_format fix ;;
        tools)
            local t
            for t in clang-tidy clang-format run-clang-tidy; do
                printf '%-16s %s\n' "$t" "$(resolve_tool "$t" || echo '(not found)')"
            done
            ;;
        -h|--help|help)
            sed -n '3,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            ;;
        *) echo "error: unknown mode '$mode' (tidy|format|format-fix|tools)" >&2; return 2 ;;
    esac
}

main "$@"
