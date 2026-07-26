# Development Instructions — ARPBOX

## Overview

Phased development guide for ARPBOX. Each phase is small enough to fit within a single Claude Code session. The orchestrator (CLAUDE.md) delegates all implementation to subagents. Every phase ends with the mandatory `code-reviewer` gate; deferred findings are logged via `issue-triage`.

### Subagent Roles

| Subagent | Responsibilities |
|----------|-----------------|
| `audio-engine-dev` | Audio graph, device I/O, transport/AudioPlayHead, FIFOs/snapshots, master section, recorder |
| `plugin-host-dev` | VST3/AU scanning, instantiation, HostedPluginNode, editor windows, state persistence, scanner-helper |
| `generative-seq-dev` | Sequencer node, pattern model, step logic, operator stack, seeds, constraints, note pool, MIDI correctness |
| `juce-ui-dev` | Component library, Tokens.h, piano roll, lane strip, panels, interaction |
| `test-engineer` | All testing: contract suites (determinism, MIDI conformance, hosting lab, perf), fakes, sanitizers |
| `code-reviewer` | Mandatory review gate, RT-safety audits, architecture compliance |
| `issue-triage` | GitHub issue creation, labeling, milestone review |

### Key Commands

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
open build/app/ARPBOX_artefacts/ARPBOX.app
```

---

## Phase 1: Project Skeleton & CI

### Objective
Buildable, signed, testable app shell with all tooling wired.

### Prerequisites
- None

### Reference Documents
- docs/ARCHITECTURE.md §3.1, §3.2

### Tasks

#### 1.1 CMake + JUCE application scaffold
**Delegate to**: `audio-engine-dev`

Create the CMake superstructure (JUCE 8 submodule, Ninja, universal binary, macOS 12.0+ target), the `app/` GUI application target with an empty main window, and the `engine/` static library target with zero UI module dependencies. Add `.clang-format` (JUCE-derived) and `.clang-tidy` (bugprone/performance/concurrency checks).

#### 1.2 Test harness
**Delegate to**: `test-engineer`

Wire Catch2 v3 via FetchContent + CTest with labeled contract-suite targets (`determinism`, `midi-conformance`, `hosting-lab`, `perf-budget`, plus `unit`). Add TSan and ASan build presets. One smoke test per suite proves the pipeline.

#### 1.3 Signing, entitlements & CI
**Delegate to**: `plugin-host-dev`

Hardened Runtime configuration with the `com.apple.security.cs.disable-library-validation` entitlement (required for third-party plugin loading — see ARCHITECTURE §3.1). CI workflow: configure, build both arches, run tests, codesign; notarization job stubbed for release branches.

#### 1.4 Repo hygiene
**Delegate to**: `test-engineer`

`.gitignore` (build/, .claude/settings.local.json, .claude/state/), README with build instructions, `design/` folder containing the UI mockup reference.

### Success Criteria
- [ ] `cmake --build build` produces a signed, launchable .app on arm64 and x86_64
- [ ] `ctest --test-dir build` runs and passes all smoke tests
- [ ] `engine/` fails to compile if `juce_gui_basics` is included (enforced check)
- [ ] clang-format and clang-tidy run clean on the scaffold

---

## Phase 2: Audio Device I/O & Graph Passthrough

### Objective
CoreAudio in/out through an AudioProcessorGraph with a working master section.

### Prerequisites
- Phase 1

### Reference Documents
- docs/ARCHITECTURE.md §3.3, §3.4, §7 (master), §11

### Tasks

#### 2.1 Device layer
**Delegate to**: `audio-engine-dev`

`AudioDeviceManager` setup with persisted device/buffer/SR selection, `AudioProcessorPlayer` hosting the root `AudioProcessorGraph` (audio I/O nodes connected). Graceful device-death handling: auto-fallback to default device + event for the UI banner.

#### 2.2 Master section node
**Delegate to**: `audio-engine-dev`

Master processor: output gain, brickwall safety limiter (default ON), stereo peak/RMS meter tap. `ScopedNoDenormals` + NaN scrub at the graph boundary.

#### 2.3 Cross-thread infrastructure
**Delegate to**: `audio-engine-dev`

Implement the three canonical mechanisms (ARCHITECTURE §3.4): `EngineCommandQueue` (AbstractFifo SPSC, POD commands), `EngineSnapshot` triple buffer, discrete event FIFO, plus the snapshot-retirement return queue. These are the only cross-thread channels for the rest of the project.

#### 2.4 Infrastructure tests
**Delegate to**: `test-engineer`

Headless tests: FIFO round-trip under churn, snapshot swap adoption at boundaries, allocation-guard harness (operator-new counter) proving zero steady-state audio-thread allocation.

### Success Criteria
- [ ] Test tone through graph to device out; meter values visible in a debug readout
- [ ] Device unplug mid-playback falls back without crash or hang
- [ ] TSan job clean on FIFO/snapshot stress tests
- [ ] Allocation guard asserts 0 allocations in steady-state processBlock

---

## Phase 3: Plugin Discovery & Instantiation

### Objective
Scan VST3 + AU plugins and instantiate them reliably (in-process scan for now, seam left for Phase 20).

### Prerequisites
- Phase 2

### Reference Documents
- docs/ARCHITECTURE.md §6.1, §6.2

### Tasks

#### 3.1 Format manager & known-plugin list
**Delegate to**: `plugin-host-dev`

`AudioPluginFormatManager` (VST3 + AU), `KnownPluginList` persisted to Application Support XML, default + user search paths, incremental rescan. Structure the scan behind `KnownPluginList::CustomScanner` so Phase 20's out-of-process swap is localized.

#### 3.2 Instantiation service
**Delegate to**: `plugin-host-dev`

Async instantiation path (`createPluginInstanceAsync` uniformly), prepareToPlay-before-insertion discipline, failure isolation with typed error results.

#### 3.3 Fake-plugin corpus
**Delegate to**: `test-engineer`

Create `tests/fakes/`: in-memory AudioProcessor fakes exhibiting hostile behaviors (crash-on-scan marker, allocate-in-processBlock, wrong latency reporting, state-corrupting, bus-lying). Hosting-lab suite: scan/instantiate/fail-path coverage.

### Success Criteria
- [ ] Scan finds installed VST3 + AU plugins; list persists across launches
- [ ] Instantiation failure of a fake leaves the app healthy with a typed error
- [ ] Hosting-lab suite green, including all hostile fakes

---

## Phase 4: Synth Hosting End-to-End

### Objective
Play a hosted synth live from the QWERTY keyboard — first sound.

### Prerequisites
- Phase 3

### Reference Documents
- docs/ARCHITECTURE.md §3.3, §6.3, §9

### Tasks

#### 4.1 HostedPluginNode v1
**Delegate to**: `plugin-host-dev`

The wrapper (ARCHITECTURE §6.3): bus negotiation, soft-bypass crossfade, gain trims, NaN scrub, latency forwarding. Instrument configuration (MIDI in, audio out).

#### 4.2 MIDI input node & QWERTY path
**Delegate to**: `audio-engine-dev`

MIDI In graph node merging hardware devices + the command-FIFO note events (QWERTY/pads from the message thread). Channel filtering. Wire: MIDI In → synth slot → master.

#### 4.3 Synth slot management
**Delegate to**: `plugin-host-dev`

Slot model: load/swap (crossfaded via async graph edit), remove, gain. Minimal debug UI (plain list + load button — real UI comes in Phase 15+).

#### 4.4 Integration tests
**Delegate to**: `test-engineer`

Hosting-lab additions: fake-synth end-to-end (note event → rendered audio non-silence), swap-under-playback stress, latency propagation assertions.

### Success Criteria
- [ ] Typing plays a real installed synth audibly, latency ≤ 1 block above device latency
- [ ] Synth swap mid-note: no click, no hang, no stuck note
- [ ] Graph latency reflects the hosted plugin's reported latency

---

## Phase 5: Transport & Sequencer Node Scaffold

### Objective
Sample-accurate transport driving a sequencer node that emits a fixed test pattern.

### Prerequisites
- Phase 4

### Reference Documents
- docs/ARCHITECTURE.md §3.3, §4, §5.5

### Tasks

#### 5.1 Transport clock
**Delegate to**: `audio-engine-dev`

Double-precision PPQ transport advanced per block; tempo 20–300 BPM; play/stop/position commands via the FIFO; block-boundary tempo changes. Custom `AudioPlayHead` on the player exposing tempo/PPQ/play-state to all hosted plugins.

#### 5.2 Sequencer node shell
**Delegate to**: `generative-seq-dev`

MIDI-only `AudioProcessor` node placed ahead of the synth: consumes transport context, emits a hardcoded 16-step pattern with sample-offset events. Sounding-note table v1 with note-off ownership and stop-flush.

#### 5.3 Timing verification
**Delegate to**: `test-engineer`

Render N bars headless at multiple buffer sizes (32–2048) and assert event sample positions are identical (buffer-size independence). Verify AudioPlayHead values seen by a fake plugin match the transport.

### Success Criteria
- [ ] Test pattern plays the hosted synth in perfect time at any buffer size
- [ ] Hosted fake plugin observes correct BPM/PPQ via AudioPlayHead
- [ ] Stop flushes all notes (table empty assertion)

---

## Phase 6: Pattern Model & Lanes

### Objective
The full deterministic pattern core: document, snapshot, lanes, polymeter, directions, euclid.

### Prerequisites
- Phase 5

### Reference Documents
- docs/ARCHITECTURE.md §4, §5.1 (L1), §12.1, §12.3

### Tasks

#### 6.1 PatternDocument & PatternSnapshot
**Delegate to**: `generative-seq-dev`

Message-thread `PatternDocument` (editable, undoable) → immutable `PatternSnapshot` build → publish/adopt-at-boundary/retire cycle using Phase 2 infrastructure. 16 patterns; quantized pattern switching (instant/beat/bar/pattern-end).

#### 6.2 All 11 lanes + polymeter
**Delegate to**: `generative-seq-dev`

Lane storage per §12.1 with independent length + clock division per lane; GATE/PITCH/OCT/VEL/LEN evaluation in the step tick; LEN>100% tie/legato and same-pitch retrigger policy per §5.5.

#### 6.3 Direction modes & euclidean generator
**Delegate to**: `generative-seq-dev`

All §12.3 traversal modes over the (stub) note pool; euclidean steps/pulses/rotate writer for the GATE lane.

#### 6.4 Core determinism goldens
**Delegate to**: `test-engineer`

First golden-MIDI files: representative patterns (polymetric, tied, each direction mode) rendered to `tests/golden/`; determinism suite compares byte-for-byte.

### Success Criteria
- [ ] Polymetric lanes phase correctly (verified against hand-computed golden)
- [ ] Pattern switch lands exactly on the chosen quantize boundary
- [ ] Determinism suite green at all buffer sizes
- [ ] Retired snapshots freed on message thread only (TSan clean)

---

## Phase 7: Step Logic

### Objective
Probability, trig conditions, ratchets, micro-timing, swing.

### Prerequisites
- Phase 6

### Reference Documents
- docs/ARCHITECTURE.md §5.1 (L2), §12.2

### Tasks

#### 7.1 Probability & trig conditions
**Delegate to**: `generative-seq-dev`

Per-step probability rolls (seeded RngStream — this introduces the RNG infrastructure per §5.2, versioned from day one) and the full condition set (§12.2): A:B loop-aware cycles, 1ST/!1ST, FILL/!FILL (FILL command flag), PRE/!PRE, NEI/!NEI. Conditions gate before probability.

#### 7.2 Ratchets, micro, swing
**Delegate to**: `generative-seq-dev`

Ratchet subdivision (1–8) with velocity ramp + per-ratchet probability; MICRO shifts; swing applied on top of micro at pattern grid level.

#### 7.3 Step-logic test matrix
**Delegate to**: `test-engineer`

Goldens for condition matrices across multi-loop renders (A:B correctness over 8 loops); statistical tests for probability (seeded, exact expected sequences); ratchet timing goldens.

### Success Criteria
- [ ] 3:4 condition fires exactly on loop 3 of every 4 (golden-verified)
- [ ] Probability output is seed-exact and loop-stable
- [ ] Swing + micro compose correctly (golden-verified)

---

## Phase 8: Note Pool & MIDI Correctness

### Objective
THRU/SELF input modes and bulletproof note lifecycle.

### Prerequisites
- Phase 7

### Reference Documents
- docs/ARCHITECTURE.md §5.1 (L0), §5.5, §9

### Tasks

#### 8.1 Note pool
**Delegate to**: `generative-seq-dev`

THRU mode (live-held tracking, sustain latch, hold latch); SELF mode (root + scale + degree stack, 8-slot chord lane); pool-change flush semantics; as-played ordering for direction modes.

#### 8.2 Hanging-note killer & panic
**Delegate to**: `generative-seq-dev`

Complete sounding-note table semantics per §5.5: flush on stop/pattern switch/pool change/plugin swap; panic (CC123 + per-note offs) to synth and MIDI out path.

#### 8.3 MIDI conformance suite
**Delegate to**: `test-engineer`

The hanging-note fuzzer: seeded random churn of transport/pattern/pool/latch events over thousands of blocks, asserting an empty table after every flush point and zero orphan note-ons at end. Overlap/retrigger property tests.

### Success Criteria
- [ ] Fuzzer: zero stuck notes across 10k-event seeded runs
- [ ] Chord latch survives sustain-pedal edge cases (golden scenarios)
- [ ] SELF mode plays correct scale tones with no controller attached

---

## Phase 9: FX Rack

### Objective
Six serial wrapped FX slots with correct dry/wet and live reordering.

### Prerequisites
- Phase 4 (wrapper), Phase 5 (transport for synced FX)

### Reference Documents
- docs/ARCHITECTURE.md §6.3, §7

### Tasks

#### 9.1 FX wrapper features
**Delegate to**: `plugin-host-dev`

Extend HostedPluginNode for FX configuration: per-slot dry/wet with latency-compensated dry path, input/output trims, effect bus negotiation.

#### 9.2 Rack model & chain wiring
**Delegate to**: `plugin-host-dev`

6-slot rack model: load/remove/reorder as async graph edits with crossfades; chain latency accumulation reported to graph + UI event (warning threshold 10 ms per ARCHITECTURE §3.3).

#### 9.3 Rack stress tests
**Delegate to**: `test-engineer`

Hosting-lab: reorder-under-playback stress, dry/wet null test (wet=0 ⇒ bit-exact dry through latency compensation), latency-lying fake handling.

### Success Criteria
- [ ] 6 real FX process in series; reorder mid-playback is click-free
- [ ] Dry/wet at 0% nulls against bypassed (latency-compensated) signal
- [ ] Latency badge event fires when chain exceeds threshold

---

## Phase 10: Plugin Editor Windows

### Objective
Native editor windows plus the generic parameter editor.

### Prerequisites
- Phase 9

### Reference Documents
- docs/ARCHITECTURE.md §6.4

### Tasks

#### 10.1 Editor window host
**Delegate to**: `plugin-host-dev`

Per-plugin `DocumentWindow` with resize handling, remembered position/size per plugin UID, always-on-top toggle, close-on-plugin-removal. (Chrome styling lands with the UI phases; structural now.)

#### 10.2 Generic parameter editor
**Delegate to**: `plugin-host-dev`

Auto-generated parameter view from `AudioProcessorParameter` enumeration (sliders/choices/buttons), usable for every plugin; parameter-touch events exposed (feeds Phase 14's wiggle-to-learn).

#### 10.3 Editor lifecycle tests
**Delegate to**: `test-engineer`

Hosting-lab: open/close storm, editor-open during plugin swap/removal, generic editor against fakes with pathological parameter counts.

**Leak detection mechanism — ASan does NOT provide this on macOS.** LeakSanitizer is unsupported on Darwin: `ASAN_OPTIONS=detect_leaks=1` is rejected and aborts Catch2's test-discovery step, so the `asan` preset finds memory *errors* (use-after-free / out-of-bounds) and never leaks. Use instead:

- **The gate** (enforcing): `leaks --atExit -- ./build/tests/arpbox_tests "<storm test>"`, wired as its own CTest test in the hosting-lab suite. `leaks` exits non-zero when anything leaked, so the gate fails on its own with no human reading output — verified locally (0 leaks ⇒ exit 0; one deliberate `malloc` leak ⇒ exit 1). Works in any build config; the `debug` preset only adds attribution. `leaks` needs task-port access to the child, so confirm it functions on the CI runner when wiring it — if it cannot run there, the gate is a required local/pre-release step and the phase is not met on CI evidence alone.
- **Attribution** (diagnostic only, never the gate): JUCE's `LeakedObjectDetector` via `JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR` names the leaking class. It requires the `debug` preset — `JUCE_CHECK_MEMORY_LEAKS` is defined only under `JUCE_DEBUG`, so in RelWithDebInfo every leak detector in the codebase compiles to nothing — and even then it only *prints*, because `jassertfalse` is a no-op unless a debugger is attached. A leaking test run under it still exits 0. Do not treat it as a pass/fail signal.
- **Manual / soak passes**: Instruments' Leaks instrument (see 21.3).

### Success Criteria
- [ ] Real plugin editors open, resize, remember position, survive removal
- [ ] Generic editor works for every fake and real plugin tested
- [ ] Open/close storm is leak-free under the `leaks --atExit` gate above — and that gate is **validated fails-without/passes-with**: land the test alongside a temporary deliberate editor-window leak, record the non-zero exit, then remove the leak and record green. A leak check that has never been seen to fail does not count as met

---

## Phase 11: Project Persistence

### Objective
Full save/load with plugin state, autosave, and crash recovery.

### Prerequisites
- Phase 10 (all state producers exist for the sound half); Phase 8 (sequencer state)

### Reference Documents
- docs/ARCHITECTURE.md §8

### Tasks

#### 11.1 Serialization layer
**Delegate to**: `generative-seq-dev`

`project.json` schema per §8.1: transport, key/scale, all pattern data, operators (stub list now), seeds, constraints, chain. Schema-versioned with forward migration; round-trip property tests.

#### 11.2 Plugin state & container
**Delegate to**: `plugin-host-dev`

`.arpx` zip container assembly; opaque state blob capture/restore; plugin identity matching + missing-plugin placeholder slots preserving blobs.

#### 11.3 Autosave & crash recovery
**Delegate to**: `plugin-host-dev`

Worker-thread autosave (60 s + significant edits, rotating slots, < 100 ms message-thread stall per §11 budget); recovery offer on launch after unclean exit.

#### 11.4 Persistence torture tests
**Delegate to**: `test-engineer`

kill -9 during autosave, truncated/corrupt zip, missing plugins, forward-migration from a frozen v0 fixture — every path lands somewhere safe.

### Success Criteria
- [ ] Save → quit → load restores sound + sequence exactly (golden A/B render)
- [ ] Missing plugin preserved as placeholder; state restores when plugin returns
- [ ] All torture scenarios recover without data loss beyond last autosave

---

## Phase 12: Seed System & Generative Core v1

### Objective
The generative foundation: seeds, history, first four operators, constraints, locks, commit. First "wow" build.

### Prerequisites
- Phase 11

### Reference Documents
- docs/ARCHITECTURE.md §5.1 (L3), §5.2, §12.4

### Tasks

#### 12.1 Seed engine & history
**Delegate to**: `generative-seq-dev`

Seed composition per §5.2 (master ⊕ operator ⊕ optional bar counter), LOOP LOCK, DICE command, 64-deep seed history ring with back/forward, A/B slots, quantized apply. All serialized.

#### 12.2 IStepOperator framework + first operators
**Delegate to**: `generative-seq-dev`

The `IStepOperator` interface and stack executor (ordered, lane-masked, bypassable, RT-safe); implement DICE, EUCLID MORPH, HUMANIZE, SPARSE/DENSE. Provenance bitmask plumbed through to the snapshot.

#### 12.3 Constraint gate & anchor locks
**Delegate to**: `generative-seq-dev`

Scale mask (snap/mute), range clamp (fold/clamp), note budget, repeat suppressor; per-step and per-lane anchor locks immune to operators and commits; COMMIT/UNCOMMIT.

#### 12.4 Generative determinism suite
**Delegate to**: `test-engineer`

Goldens: each operator at fixed seeds/amounts; LOOP LOCK on (loop-identical) and off (bar-varied but seed-exact); anchor immunity tests; commit-then-uncommit round trip; constraint edge cases (empty pool, budget 0, one-note range).

### Success Criteria
- [ ] DICE → back → forward reproduces exact prior outputs
- [ ] LOOP LOCK on: byte-identical MIDI every loop
- [ ] Anchored steps unchanged under amount=100 operator storms
- [ ] All generative goldens green; RNG version stamped in schema

---

## Phase 13: Full Operator Set

### Objective
The remaining seven operators.

### Prerequisites
- Phase 12

### Reference Documents
- docs/ARCHITECTURE.md §5.1 (L3), §12.4

### Tasks

#### 13.1 TURING & DRIFT
**Delegate to**: `generative-seq-dev`

Shift-register Turing machine on the pitch lane (loop↔mutate continuum); brownian DRIFT with rate + gravity on pitch/vel/micro.

#### 13.2 MARKOV
**Delegate to**: `generative-seq-dev`

Order-1/2 transition tables: message-thread build from style presets and learn-from-pattern; immutable snapshot delivery; RT-safe sampling.

#### 13.3 RATCHETIZER, STRUM, VOICE-LEAD, EVOLVE
**Delegate to**: `generative-seq-dev`

Beat-weighted ratchet promotion; chord-roll STRUM (span/curve); VOICE-LEAD movement minimization in chord mode; EVOLVE (every N bars, mutate M% of unlocked steps, prints to working buffer, respects anchors).

#### 13.4 Operator goldens & interaction matrix
**Delegate to**: `test-engineer`

Goldens per operator + stacked-interaction scenarios (TURING→MARKOV ordering differences, EVOLVE under LOOP LOCK, full 8-stack storm within perf budget).

### Success Criteria
- [ ] All 11 operators golden-verified individually and stacked
- [ ] MARKOV table rebuild causes zero audio-thread disturbance (TSan + allocation guard)
- [ ] Full stack stays within the §11 processBlock budget

---

## Phase 14: Macros & Mod Matrix

### Objective
Performance macros and modulation routing to arp params, plugin params, and CC.

### Prerequisites
- Phase 13

### Reference Documents
- docs/ARCHITECTURE.md §5.3

### Tasks

#### 14.1 Macro layer
**Delegate to**: `generative-seq-dev`

CHAOS/DENSITY/MOTION/SHAPE with the curated default mappings (§5.3), user-remappable, MIDI-learnable; macros as mod sources; MOD A/B lanes as per-step sources.

#### 14.2 Plugin parameter targets
**Delegate to**: `plugin-host-dev`

Mod-matrix targets on hosted parameters via the wrapper's parameter attachment; wiggle-to-learn flow using Phase 10's parameter-touch events; block-rate smoothing of modulated values.

#### 14.3 Matrix tests
**Delegate to**: `test-engineer`

Mod routing determinism goldens (CC output streams); learn-flow tests against fakes; macro serialization round-trip.

### Success Criteria
- [ ] CHAOS at 0 ⇒ pure sequenced output; sweep is glitch-free
- [ ] A macro modulates a real hosted filter cutoff audibly and smoothly
- [ ] CC out streams are golden-verified

---

## Phase 15: UI Foundation

### Objective
Design tokens, component library, and the main window shell.

### Prerequisites
- Phase 12 (engine snapshot carries what the UI needs); UI phases can overlap 13–14

### Reference Documents
- docs/DESIGN_SYSTEM.md (all); docs/ARCHITECTURE.md §10; design/arpbox_ui_mockup.html

### Tasks

#### 15.1 Tokens & LookAndFeel
**Delegate to**: `juce-ui-dev`

`ui/Tokens.h` mirroring DESIGN_SYSTEM tokens; skin system (Midnight + 3000 Gray semantic schema); base LookAndFeel; reduced-motion setting.

#### 15.2 Component library
**Delegate to**: `juce-ui-dev`

`SegDisplay`, `SkirtKnob`, `RubberPad`, `SilkPanel`, chip buttons, strip displays — each with focus/keyboard/type-in/fine-drag behaviors per design-system rules, smoke-tested.

#### 15.3 Window shell & header
**Delegate to**: `juce-ui-dev`

Three-column layout scaffold + header (transport controls, 7-seg BPM with tap, swing, key/scale, clock source, CPU meter, project name) wired to the command queue and snapshot.

### Success Criteria
- [ ] Both skins render the shell; zero hardcoded visual literals (grep-verified)
- [ ] Transport/BPM controls drive the engine; header reflects snapshot at 60 fps
- [ ] All components keyboard-accessible with visible focus

---

## Phase 16: Piano Roll Editor

### Objective
The primary editor: punch-in piano roll with ghost notes and the lane strip.

### Prerequisites
- Phase 15

### Reference Documents
- docs/ARCHITECTURE.md §10.3; docs/DESIGN_SYSTEM.md (piano roll spec)

### Tasks

#### 16.1 Roll rendering
**Delegate to**: `juce-ui-dev`

Pitch rows × step columns from the snapshot: scale tinting, FOLD mode, THRU-mode degree relabeling, cached grid layer + independent note/playhead layers, solid vs ghost note rendering with source tags, per-note badges (prob/cond/ratchet/anchor).

#### 16.2 Edit model
**Delegate to**: `juce-ui-dev`

Click punch-in, click-delete, drag move, edge-drag length, option-vertical velocity, marquee + copy/paste, shift-click anchor, right-click popover (prob/cond/ratchet) — all as PatternDocument edits with undo.

#### 16.3 Lane strip
**Delegate to**: `juce-ui-dev`

Collapsible lane strip (VEL/LEN/RATCH/MICRO/PROB/COND/MOD A/MOD B): per-column bars, vertical drag + horizontal paint, COND cycle/popover.

#### 16.4 Edit-model tests
**Delegate to**: `test-engineer`

Interaction tests on the edit model (simulated mouse streams → document assertions); paint-to-image smoke tests both skins; undo/redo property tests.

### Success Criteria
- [ ] Punch in a melody, hear it on the hosted synth immediately
- [ ] Ghost vs committed rendering matches spec in both skins
- [ ] Note-layer edits never repaint the cached grid layer (repaint-count instrumented)
- [ ] Full undo/redo across all edit gestures

---

## Phase 17: Generate Panel, Sound Column & Pads

### Objective
The rest of the main window: generative controls, FX rack UI, master, pad strip.

### Prerequisites
- Phase 16

### Reference Documents
- docs/ARCHITECTURE.md §10.2; docs/DESIGN_SYSTEM.md

### Tasks

#### 17.1 Generate panel
**Delegate to**: `juce-ui-dev`

DICE button (roll animation), seed display + history nav, A/B, COMMIT, LOOP LOCK, four macro knobs, reorderable OperatorCard stack (amount/seed-dot/bypass, expandable params), constraints strip. X-RAY toggle wiring provenance badges.

#### 17.2 Sound column
**Delegate to**: `juce-ui-dev`

Synth slot strip (name/preset/edit/generic/swap/gain), 6 FX slot rows (name, bypass LED, dry/wet mini-knob, drag reorder), master (volume, limiter LED, meters, REC arm, DRAG MIDI chip).

#### 17.3 Pad strip & FILL
**Delegate to**: `juce-ui-dev`

16-pad strip: pattern select (quantized switch feedback), pad 16 FILL momentary, velocity via click-Y, QWERTY mapping.

#### 17.4 Full-window review pass
**Delegate to**: `test-engineer`

Smoke both skins at min/max sizes; keyboard-only walkthrough of every control; snapshot-starvation behavior (engine stopped) renders gracefully.

### Success Criteria
- [ ] Complete workflow UI-only: load synth+FX, punch notes, dice, lock, commit
- [ ] Operator drag-reorder audibly reorders processing at the next boundary
- [ ] FILL pad drives FILL/!FILL conditions live

---

## Phase 18: Export & Recorder

### Objective
The DAW bridge: MIDI drag-out, offline bounce, live recorder.

### Prerequisites
- Phase 17

### Reference Documents
- docs/ARCHITECTURE.md §7 (recorder), §9

### Tasks

#### 18.1 Offline render service
**Delegate to**: `generative-seq-dev`

Offline engine pass rendering a pattern/chain to a MIDI event list using the exact real-time code path (determinism contract); n-bar unroll for LOOP-LOCK-off patterns.

#### 18.2 MIDI drag-out & clock out
**Delegate to**: `audio-engine-dev`

`.mid` file build + `performExternalDragDropOfFiles` from the roll and the DRAG MIDI chip; MIDI output mirror (devices + virtual "ARPBOX Out"); MIDI clock out.

#### 18.3 Recorder & bounce
**Delegate to**: `audio-engine-dev`

Master-tap recorder to WAV/AIFF via ThreadedWriter (arm + play, project bin, drag-out); offline audio bounce of pattern/chain through the full graph.

#### 18.4 Export verification
**Delegate to**: `test-engineer`

Exported .mid re-imported equals live-rendered golden; bounce nulls against a captured live pass (within limiter tolerance); recorder file integrity under stop/start churn.

### Success Criteria
- [ ] Dragged .mid into a DAW plays exactly what ARPBOX played
- [ ] LOOP-LOCK-off export unrolls N bars with per-bar variation intact
- [ ] Recorder produces gapless files; clock out locks external gear

---

## Phase 19: Preset System & Browser

### Objective
All five preset types with a browser overlay.

### Prerequisites
- Phase 18

### Reference Documents
- docs/ARCHITECTURE.md §8.2

### Tasks

#### 19.1 Preset infrastructure
**Delegate to**: `generative-seq-dev`

Pattern + Style preset capture/apply (Style = operators + constraints + macros, no steps); single-file zip format shared with §8.1 machinery.

#### 19.2 Sound presets
**Delegate to**: `plugin-host-dev`

FX Chain and Kit presets (plugin refs + state blobs); missing-plugin behavior consistent with project load.

#### 19.3 Browser UI
**Delegate to**: `juce-ui-dev`

Overlay browser: type-filtered lists, save/load/rename/delete, drag-in/out of preset files, worker-thread indexing.

#### 19.4 Preset tests
**Delegate to**: `test-engineer`

Round-trips per type; Style-applied-to-different-pattern goldens; cross-version load of frozen fixtures.

### Success Criteria
- [ ] A Style preset transplants generative character onto another pattern deterministically
- [ ] Kit preset restores an entire sound on a blank project
- [ ] Browser stays responsive while indexing a large preset folder

---

## Phase 20: Out-of-Process Scanner Hardening

### Objective
Crash-proof plugin scanning via the scanner-helper child process.

### Prerequisites
- Phase 19 (feature-complete before hardening)

### Reference Documents
- docs/ARCHITECTURE.md §6.1

### Tasks

#### 20.1 scanner-helper binary
**Delegate to**: `plugin-host-dev`

Console target speaking the ChildProcessWorker protocol: scan requests in, PluginDescription results out, per-file timeout, clean crash behavior.

#### 20.2 Coordinator integration
**Delegate to**: `plugin-host-dev`

Swap the Phase 3 CustomScanner seam to ChildProcessCoordinator supervision: relaunch on crash, dead-man's-pedal quarantine, blocklist management UI in the plugin manager window.

#### 20.3 Hostile scan testing
**Delegate to**: `test-engineer`

Crash-on-scan and hang-on-scan fixtures (real subprocess); assert app stays responsive, offender lands on the blocklist, rescans skip it, user override works.

### Success Criteria
- [ ] A crashing plugin binary cannot take down or hang the app during scan
- [ ] Quarantined plugins listed with user-facing override
- [ ] Full rescan of 100+ plugins keeps UI responsive

---

## Phase 21: Performance & RT-Safety Validation

### Objective
Prove every §11 budget and RT invariant under load.

### Prerequisites
- Phase 20

### Reference Documents
- docs/ARCHITECTURE.md §11

### Tasks

#### 21.1 Perf harness completion
**Delegate to**: `test-engineer`

perf-budget suite: synthetic heavy graph (synth fake + 6 FX fakes + full 8-operator stack) measured across buffer sizes; CI-asserted thresholds; UI repaint instrumentation report.

#### 21.2 Hotspot remediation
**Delegate to**: `audio-engine-dev`

Profile (Instruments) and fix engine/graph hotspots surfaced by 21.1; coordinate with `generative-seq-dev` for operator hotspots and `juce-ui-dev` for paint hotspots (re-delegate per domain).

#### 21.3 Sanitizer soak
**Delegate to**: `test-engineer`

Extended TSan/ASan soak runs of the full suites + a scripted app session; zero findings tolerated (or logged critical). **Leaks are a separate mechanism**: TSan covers races and ASan on macOS covers memory *errors* only — LeakSanitizer does not exist on Darwin (see Phase 10.3). Leak coverage for this phase is `leaks --atExit` over the suites plus an Instruments Leaks pass across the 24-hour session; do not report "ASan clean" as evidence of leak-freedom.

### Success Criteria
- [ ] All §11 budgets green in CI
- [ ] Zero TSan findings (races) and zero ASan findings (memory errors) across soak runs
- [ ] Zero leaks across the suites under `leaks --atExit`, using the same validated gate as Phase 10.3
- [ ] 24-hour playback soak: no drift, no stuck notes, and a flat allocation trend with no leaks reported by Instruments' Leaks instrument

---

## Phase 22: Packaging & Beta

### Objective
Notarized DMG, docs, and a beta-ready build.

### Prerequisites
- Phase 21

### Reference Documents
- docs/ARCHITECTURE.md §3.1

### Tasks

#### 22.1 Release pipeline
**Delegate to**: `plugin-host-dev`

Release CI: universal build, codesign with entitlements, notarize, staple, DMG with layout; version stamping; clean-machine validation checklist (Gatekeeper pass, plugin loading confirmed).

#### 22.2 First-run & docs
**Delegate to**: `juce-ui-dev`

First-run flow (audio device pick, initial scan prompt), tooltip pass (every generative control states what it will change), README/quick-start.

#### 22.3 Compatibility lab
**Delegate to**: `test-engineer`

Scripted load/save/reload/editor/param-storm across the top-50 real synths/FX available; file compatibility issues via `issue-triage` (this phase generates the beta-blocker milestone sweep).

#### 22.4 Final review
**Delegate to**: `code-reviewer`

Full-codebase architecture compliance pass; verify every phase's success criteria still hold; final `milestone-review` for beta-blockers.

### Success Criteria
- [ ] Notarized DMG installs and runs clean on a fresh machine, loading third-party plugins
- [ ] Compatibility lab issues triaged into milestones with a beta-blocker batch identified
- [ ] All contract suites green on the release build

---

## Checklist Summary

### Foundation
- [ ] Phase 1: Project Skeleton & CI
- [ ] Phase 2: Audio Device I/O & Graph Passthrough
- [ ] Phase 3: Plugin Discovery & Instantiation
- [ ] Phase 4: Synth Hosting End-to-End

### Sequencer Core
- [ ] Phase 5: Transport & Sequencer Node Scaffold
- [ ] Phase 6: Pattern Model & Lanes
- [x] Phase 7: Step Logic
- [ ] Phase 8: Note Pool & MIDI Correctness

### Sound Section
- [ ] Phase 9: FX Rack
- [ ] Phase 10: Plugin Editor Windows
- [ ] Phase 11: Project Persistence

### Generative Engine
- [ ] Phase 12: Seed System & Generative Core v1
- [ ] Phase 13: Full Operator Set
- [ ] Phase 14: Macros & Mod Matrix

### UI
- [ ] Phase 15: UI Foundation
- [ ] Phase 16: Piano Roll Editor
- [ ] Phase 17: Generate Panel, Sound Column & Pads

### Integration & Release
- [ ] Phase 18: Export & Recorder
- [ ] Phase 19: Preset System & Browser
- [ ] Phase 20: Out-of-Process Scanner Hardening
- [ ] Phase 21: Performance & RT-Safety Validation
- [ ] Phase 22: Packaging & Beta
