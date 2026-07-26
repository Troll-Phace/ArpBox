# ARPBOX

ARPBOX is a standalone macOS generative-arp workstation. It hosts a single synth
plugin (VST3 or AUv2) and a 6-slot serial FX rack, and drives them with a deeply
programmable, seed-based generative sequencer. Every random outcome passes an
always-on constraint gate (scale, range, note budget, repeat suppression), so
randomness can be cranked hard without ever leaving the musical space — and every
roll is seeded, reproducible, and reversible. The UI is "retro chassis, modern
surface": MPC-style chassis around clean, modern dark-UI working surfaces.

## Prerequisites

- macOS 12.0 or later
- CMake ≥ 3.22
- Ninja
- A C++20 toolchain — Xcode command-line tools (`xcode-select --install`)

JUCE 8 is vendored as a git submodule (`JUCE/`, pinned to 8.0.15); it is not a
separate install.

## Clone

Clone with submodules so JUCE is fetched in one step:

```
git clone --recurse-submodules <repo-url>
```

If you already cloned without `--recurse-submodules`, initialize the submodule:

```
git submodule update --init --recursive
```

## Build & test

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
open build/app/ARPBOX_artefacts/RelWithDebInfo/ARPBOX.app
```

### Contract test suites

Tests are grouped into labeled contract suites. Run one by label:

```
ctest --test-dir build -L determinism
```

Available labels: `determinism`, `midi-conformance`, `hosting-lab`,
`perf-budget`, `unit`.

### Build variants (CMake presets)

`CMakePresets.json` defines the shipping build plus four developer-loop
variants, each in its own build directory so they never clobber `build/`:

| Preset | Build dir | Purpose |
|---|---|---|
| `default` | `build/` | Universal (arm64 + x86_64) RelWithDebInfo — the **shipping** artefact and the standard test run |
| `asan` | `build-asan/` | AddressSanitizer: use-after-free / out-of-bounds / heap-overflow |
| `tsan` | `build-tsan/` | ThreadSanitizer: the FIFO / snapshot cross-thread machinery |
| `tidy` | `build-tidy/` | clang-tidy compile database (configure only — never built) |
| `debug` | `build-debug/` | Debug: live `jassert`s and JUCE's leak detector |

```
cmake --preset asan  && cmake --build --preset asan  && ctest --preset asan
cmake --preset tsan  && cmake --build --preset tsan  && ctest --preset tsan
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

#### The four variants are single-arch (arm64) by design

`asan`, `tsan`, `tidy` and `debug` all inherit a hidden `single-arch-base`
preset that pins `CMAKE_OSX_ARCHITECTURES=arm64`. This is a deliberate,
permanent stance, not an oversight:

- Sanitizer runtimes are **per-arch** and do not link into a universal binary,
  so ASan/TSan builds cannot be universal at all.
- clang-tidy rejects a compile database whose entries carry two `-arch` flags
  (`error: expected exactly one compiler job`), so the tidy database cannot be
  universal either.
- These are developer-loop tools, run on arm64 dev machines and arm64 CI
  runners. The **shipping** artefact is always universal — built by `default`
  and verified slice-by-slice (`lipo -archs`) in CI's build-and-test job.

Accepted consequences: there is no ASan/TSan/clang-tidy coverage of
x86_64-specific code paths, and on an Intel host these four presets cross-compile
at best — nothing they produce runs natively, so the sanitizer and Debug trees
are unusable there. If Intel coverage is ever needed, add sibling presets
(`asan-x86_64`, …)
rather than deriving the architecture from the host — an implicit host arch
would silently change which slice CI sanitizes. Revisit at Phase 21
(Performance & RT-Safety Validation) at the latest.

#### Leak detection is NOT ASan's job on macOS

LeakSanitizer **does not exist on Darwin**: `ASAN_OPTIONS=detect_leaks=1` is
rejected and aborts Catch2's test-discovery step. The `asan` preset finds memory
*errors*, never leaks. Use instead, in this order:

```
# 1. Enforcing gate — non-zero exit if anything leaked (works in any config):
leaks --atExit -- ./build/tests/arpbox_tests "<test name or tag>"

# 2. Attribution — names the leaking class, but only PRINTS unless a debugger
#    is attached, and is compiled out entirely in RelWithDebInfo:
ctest --preset debug        # JUCE LeakedObjectDetector, needs the Debug build
```

Instruments' Leaks instrument covers the manual and soak passes. See
`docs/INSTRUCTIONS.md` Phase 10 and Phase 21.3.

### Linting

`clang-tidy` and `clang-format` are not on `PATH` on a stock macOS box (the
Xcode toolchain does not ship them — `brew install llvm`). One script drives
linting for both developers and CI:

```
bash .claude/skills/run-lint/lint.sh tidy               # clang-tidy via the `tidy` preset
bash .claude/skills/run-lint/lint.sh warnings           # COMPILER warnings — fails on any
bash .claude/skills/run-lint/lint.sh format             # clang-format --dry-run (check only)
bash .claude/skills/run-lint/lint.sh format-file FILE   # clang-format -i on specific files
bash .claude/skills/run-lint/lint.sh tools              # show resolved tool paths
```

Formatting is enforced in two places, both through this script: a `PostToolUse`
hook formats every C++ file on save, and CI's **blocking** `clang-format` job
runs `format` over the whole tracked tree. A repo-wide reformat is a deliberate,
standalone commit (`ARPBOX_ALLOW_FORMAT_FIX=1 ... format-fix`).

`warnings` is a third, separate gate (issue #79) and is also **blocking** in CI.
clang-tidy runs its own check set and never sees compiler diagnostics, so
`-Wswitch-enum` and friends — arriving via `juce_recommended_warning_flags`, and
relied on by name in the codebase — were previously enforced by nothing. The mode
recompiles every ARPBOX translation unit in the `tidy` preset's single-arch tree
and fails on any warning in our own sources. It deletes its own object files
first, self-tests its diagnostic parser, and asserts the warning flags actually
reach the compiler, so it cannot silently pass by inspecting nothing.

## Project layout

| Path              | Contents |
|-------------------|----------|
| `app/`            | JUCE GUI application target (`ARPBOX`): AudioDeviceManager, AudioProcessorPlayer, main window, app lifecycle |
| `engine/`         | Static lib (`arpbox_engine`) — the real-time engine with **zero UI dependencies**: graph, sequencer, generative operators, MIDI |
| `hosting/`        | Plugin scan/instantiate/persist, HostedPluginNode wrapper, editor windows |
| `scanner-helper/` | Separate console binary for out-of-process plugin scans |
| `ui/`             | Component library, design tokens, screens |
| `tests/`          | Unit + contract suites (Catch2 v3 via CTest); `tests/fakes/` hostile-plugin corpus; `tests/golden/` determinism reference MIDI event streams |
| `design/`         | UI visual reference — [`design/arpbox_ui_mockup.html`](design/arpbox_ui_mockup.html) |

CI (`.github/workflows/ci.yml`) configures, builds both arches, runs the test
suites, and codesigns using `ArpBox.entitlements`.

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the full technical
reference and [`docs/INSTRUCTIONS.md`](docs/INSTRUCTIONS.md) for the phased
development plan.
