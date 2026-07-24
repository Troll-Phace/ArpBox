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

### Sanitizer builds

ASan and TSan configurations are provided as CMake presets (see
`CMakePresets.json`). Use them to build the sanitized targets, e.g.:

```
cmake --preset asan
cmake --preset tsan
```

## Project layout

| Path              | Contents |
|-------------------|----------|
| `app/`            | JUCE GUI application target (`ARPBOX`): AudioDeviceManager, AudioProcessorPlayer, main window, app lifecycle |
| `engine/`         | Static lib (`arpbox_engine`) — the real-time engine with **zero UI dependencies**: graph, sequencer, generative operators, MIDI |
| `hosting/`        | Plugin scan/instantiate/persist, HostedPluginNode wrapper, editor windows |
| `scanner-helper/` | Separate console binary for out-of-process plugin scans |
| `ui/`             | Component library, design tokens, screens |
| `tests/`          | Unit + contract suites (Catch2 v3 via CTest); `tests/fakes/` hostile-plugin corpus; `tests/golden/` determinism reference MIDI |
| `design/`         | UI visual reference — [`design/arpbox_ui_mockup.html`](design/arpbox_ui_mockup.html) |

CI (`.github/workflows/ci.yml`) configures, builds both arches, runs the test
suites, and codesigns using `ArpBox.entitlements`.

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the full technical
reference and [`docs/INSTRUCTIONS.md`](docs/INSTRUCTIONS.md) for the phased
development plan.
