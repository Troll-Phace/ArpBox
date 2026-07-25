# ARPBOX — Architecture Reference

Standalone macOS generative arp workstation: hosts a synth plugin (VST3/AUv2) and an FX rack of effect plugins, driven by a deeply programmable seed-based generative sequencer, presented in a "retro chassis, modern surface" UI. This document is the single technical source of truth. Section numbers (§N) are stable — delegation prompts reference them.

## Table of Contents

1. [Project Philosophy](#1-project-philosophy)
2. [Complete Feature Set](#2-complete-feature-set)
3. [Technical Architecture](#3-technical-architecture)
4. [Data Flow](#4-data-flow)
5. [Sequencer & Generative Engine](#5-sequencer--generative-engine)
6. [Plugin Hosting Subsystem](#6-plugin-hosting-subsystem)
7. [FX Rack & Sound Section](#7-fx-rack--sound-section)
8. [Data Model & File Formats](#8-data-model--file-formats)
9. [MIDI, I/O & Export](#9-midi-io--export)
10. [UI Architecture](#10-ui-architecture)
11. [Performance Budgets](#11-performance-budgets)
12. [Quick Reference Tables](#12-quick-reference-tables)

---

## 1. Project Philosophy

### 1.1 Constrain first, randomize hard
Every random outcome passes an always-on constraint gate (scale, range, note budget, repeat suppression). Because output can never leave the musical space, randomness controls can be cranked without producing garbage. This inversion is the product thesis and shapes the entire generative pipeline (§5).

### 1.2 Always reversible, always reproducible
Everything random is seeded (xoshiro256++ streams, versioned). A 64-deep seed history makes every previous roll recoverable; LOOP LOCK freezes a roll into a stable riff; anchor locks pin material against regeneration; commit/uncommit moves the baseline deliberately. The determinism contract — same (pattern, seeds, N bars) ⇒ byte-identical MIDI, forever — is enforced by a golden-MIDI test suite and is a release gate.

### 1.3 The engine is sample-accurate and UI-free
The sequencer is a MIDI-emitting node inside the audio graph, ticked on the audio thread with sample-offset events. No message-thread timers sequence anything. `engine/` compiles with zero UI dependencies (enabling headless tests today; a plugin version or multi-track version tomorrow).

### 1.4 Plugins are hostile until proven otherwise
Third-party binaries crash, lie about latency, and corrupt state. Scanning is out-of-process, instantiation is failure-isolated per slot, state blobs are opaque and never discarded (missing-plugin placeholders), and autosave/crash recovery is a headline feature.

### 1.5 Retro chassis, modern surface
MPC nostalgia lives in the frame (silkscreen panels, 7-seg BPM, rubber pads, red accent); working surfaces (piano roll, lane strip) are clean modern dark-UI. The retro budget is fixed and enumerated in docs/DESIGN_SYSTEM.md.

---

## 2. Complete Feature Set

### 2.1 MVP (v1)

**Sound**
- One synth slot hosting a VST3 or AUv2 instrument; crossfaded swap; gain trim; editor + generic editor
- 6-slot serial FX rack: per-slot soft-bypass, latency-compensated dry/wet, drag reorder, slot presets
- Master: output gain, safety limiter (default on), stereo metering, WAV/AIFF recorder

**Sequencer core**
- Up to 64 steps; 11 parameter lanes (§12.1) with independent per-lane length + clock division (polymeter)
- Direction modes (§12.3); euclidean gate generation; swing; per-pattern grid (1/32..1/4, triplet/dotted)
- Per-step probability + Elektron-style trig conditions (§12.2); ratchets 1–8 with velocity ramp + probability; micro-timing
- 16 patterns/project, quantized switching (instant/beat/bar/pattern-end), chain mode with repeat counts
- THRU mode (arp held/latched notes) and SELF mode (root+scale pool, 8-slot chord lane)

**Generative engine**
- Ordered operator stack (≤8) of non-destructive mutators (§12.4): DICE, DRIFT, TURING, MARKOV, EUCLID MORPH, RATCHETIZER, HUMANIZE, STRUM, VOICE-LEAD, SPARSE/DENSE, EVOLVE
- Seed system: master DICE, per-operator seeds, 64-deep history with back/forward, LOOP LOCK, A/B slots
- Anchor locks (step + lane), COMMIT/UNCOMMIT, quantized apply
- Constraint gate: scale mask (snap/mute), range clamp (fold/clamp), note budget, repeat suppressor
- 4 macros (CHAOS/DENSITY/MOTION/SHAPE), user-remappable, MIDI-learnable, mod-matrix sources
- X-RAY provenance view (which layer/operator produced each note)

**Integration**
- MIDI in (devices + virtual "ARPBOX In" + QWERTY/pads); MIDI out mirror (devices + virtual "ARPBOX Out"); MIDI clock out
- MIDI drag-out (.mid of pattern as currently sounding, n-bar unroll); offline bounce
- Project format `.arpx` (§8); autosave + crash recovery; preset taxonomy (§8.2)

### 2.2 Post-MVP
- AUv3 hosting (behind flag until stable); CLAP; Ableton Link; MIDI clock follow
- Multi-track (2–4 arp engines + synth slots); plugin version of ARPBOX; user operator scripting
- Windows port

---

## 3. Technical Architecture

### 3.1 Stack
- C++20, JUCE 8 (submodule), CMake + Ninja, universal binary (arm64 + x86_64), macOS 12.0+
- Catch2 v3 via CTest; clang-format + clang-tidy; TSan/ASan CI jobs
- Hardened Runtime + `com.apple.security.cs.disable-library-validation` entitlement (required to load third-party plugins); Developer ID signing + notarization in CI from Phase 1

### 3.2 Module layout

```
arpbox/
├── app/                    # JUCE GUI application target; owns AudioDeviceManager,
│                           #   AudioProcessorPlayer, window, app lifecycle
├── engine/                 # static lib: RT engine, ZERO UI deps
│   ├── graph/              #   graph assembly, node wiring, transport/AudioPlayHead,
│   │                       #   master section, recorder, snapshot/FIFO infrastructure
│   ├── sequencer/          #   clock, pattern model, lanes, step logic
│   ├── generative/         #   IStepOperator stack, seeds, constraints, macros
│   └── midi/               #   note pool, voice tracking, sounding-note table, MIDI I/O
├── hosting/                # plugin scan/instantiate/persist, HostedPluginNode,
│                           #   editor windows (message thread)
├── scanner-helper/         # separate console binary: out-of-process scans
├── ui/                     # component library, Tokens.h, screens
├── tests/                  #   unit + contract suites; fakes/ (hostile plugin corpus);
│                           #   golden/ (determinism reference MIDI event streams)
└── design/                 # arpbox_ui_mockup.html (visual reference)
```

Ownership boundaries: `audio-engine-dev` owns `engine/graph` + app audio wiring; `generative-seq-dev` owns `engine/sequencer|generative|midi`; `plugin-host-dev` owns `hosting/` + `scanner-helper/`; `juce-ui-dev` owns `ui/`.

### 3.3 Audio graph topology

```
                     ┌────────────────────────── ROOT AudioProcessorGraph ────┐
 HW MIDI in ─┐       │                                                        │
 QWERTY/pads ┼──► [MIDI In Node] ──► [ARP ENGINE node] ──► [SYNTH plugin]     │
 (UI thread  │       (merge +          (MIDI-only:            │ (hosted,      │
  via FIFO)  ┘        channel           consumes chord,       ▼  wrapped)     │
                      filter)           emits arp MIDI)    [FX slot 1 wrap]   │
                                            │                 ▼               │
                                            │              [FX slot 2..6]     │
                                            ▼                 ▼               │
                                     [MIDI Out node]       [Master: gain,     │
                                     (optional mirror       limiter, meter,   │
                                      to gear/virtual)      WAV recorder]     │
                                                              ▼               │
                                                           [Audio Out]        │
└─────────────────────────────────────────────────────────────────────────────┘
   AudioDeviceManager (CoreAudio) → AudioProcessorPlayer (custom AudioPlayHead)
```

- The arp engine is an `AudioProcessor` graph node handling MIDI only; it emits `MidiBuffer` events with sample-accurate offsets
- Every hosted plugin sits inside a `HostedPluginNode` wrapper (§6.3)
- Graph topology edits are message-thread only, `UpdateKind::async`, crossfaded
- The custom `AudioPlayHead` exposes tempo/PPQ/play-state to hosted plugins (synced LFOs and delays depend on it)
- Serial chain ⇒ plugin latencies accumulate; total reported on the graph and surfaced in the UI (warning badge > 10 ms)

### 3.4 Threading model

| Thread | Owns | Never does |
|---|---|---|
| Audio (CoreAudio callback) | Transport clock, arp engine tick, operator evaluation, graph processing | Allocate, lock, touch editors, file I/O, logging |
| Message | UI, plugin editors, project I/O, graph topology edits, Markov/style precompute | Block on audio |
| Workers | Scanner-child supervision, autosave serialization, WAV disk writer, preset indexing | Touch engine state directly |

Cross-thread mechanisms (the only three that exist — do not invent a fourth):

1. **UI → engine**: `EngineCommandQueue` — lock-free SPSC FIFO (`juce::AbstractFifo`) of small POD commands (set param, queue pattern switch, reroll seed at next bar…). Drained at the top of each `processBlock`.
2. **Engine → UI**: `EngineSnapshot` triple buffer (playhead, per-step activity, meters, current seed, voice count) written per block, read at 60 fps; plus a discrete event FIFO (step fired, pattern switched, latency changed) for UI animation triggers.
3. **Pattern data**: immutable `PatternSnapshot` objects built on the message thread, published by atomic pointer swap, adopted by the audio thread at a quantize boundary; retired snapshots returned via FIFO for message-thread deletion.

---

## 4. Data Flow

Primary loop (one audio block):

```
processBlock(buffer, midi):
 1. Drain EngineCommandQueue (apply POD commands; stage quantized changes)
 2. Advance transport (PPQ position from sample count + tempo map);
    update AudioPlayHead state for this block
 3. If a staged change (pattern switch / seed / commit) hits its quantize
    boundary inside this block → adopt new PatternSnapshot at that sample offset
 4. Arp engine tick: for each step boundary in block:
      note pool → pattern core lanes → step logic (prob/cond) →
      operator stack (IStepOperator chain, RngStream) → constraint gate →
      emit note events (sample offsets, provenance bitmask);
      update sounding-note table (schedule note-offs)
 5. Graph processes: synth ← MIDI; FX chain; master (gain → limiter → meter tap
    → recorder tap)
 6. Write EngineSnapshot slot; push discrete events
```

Message-thread edit flow: UI edit → mutate the authoritative `PatternDocument` (message-thread model) → rebuild immutable `PatternSnapshot` → publish (step 3 above picks it up) → undo stack records the document delta.

---

## 5. Sequencer & Generative Engine

Design priorities in order: **(1) always musical, (2) always reversible, (3) always explainable.**

### 5.1 Four-layer pipeline

```
[L0 NOTE POOL] → [L1 PATTERN CORE] → [L2 STEP LOGIC] → [L3 OPERATOR STACK] → [CONSTRAINTS] → MIDI
```

**L0 — Note pool.** THRU mode: live-held/latched notes (sustain-pedal + hold-button latch). SELF mode: pool from root + scale + degree stack, with an 8-slot chord lane (chord per N bars). Voice-tracked; the engine owns all note lifecycle downstream.

**L1 — Pattern core.** 11 lanes (§12.1) over ≤64 steps; each lane has independent length + clock division ⇒ polymeter. Direction modes (§12.3) govern pool traversal. Euclidean generator (steps/pulses/rotate) writes the GATE lane. Deterministic: the core is the user's committed material and is never modified by operators until COMMIT.

**L2 — Step logic.** Per-step probability 0–100% and trig conditions (§12.2). Conditions gate first, then probability rolls. Ratchets (1–8) with per-ratchet velocity ramp and ratchet probability.

**L3 — Operator stack.** Ordered, reorderable, ≤8 non-destructive operators (§12.4). Each has: `amount` (0–100 continuous), independent seed, lane targeting mask, bypass. Interface:

```cpp
struct StepContext;   // step index, bar counter, pool view, lane values, provenance
class IStepOperator {
public:
    virtual void prepare (const PrepareInfo&) = 0;              // message thread
    virtual void process (StepContext&, RngStream&) noexcept = 0; // audio thread, RT-SAFE
    virtual void serialize (juce::var&) const = 0;
    virtual ~IStepOperator() = default;
};
```

Heavy precompute (MARKOV transition tables, style analysis) runs on the message thread and is delivered as immutable snapshots via pointer swap — `process()` only reads.

**Constraint gate (always on).** Scale mask (snap-nearest or mute, per-pattern), range clamp (fold or clamp), note budget (max notes/beat), repeat suppressor (max consecutive identical pitches).

### 5.2 Seeds & reversibility

- Effective stream seed = `splitmix64(masterSeed ⊕ operatorSeed ⊕ (loopLock ? 0 : barCounter))`
- **LOOP LOCK on** ⇒ identical output every loop (a roll becomes a riff). Off ⇒ re-rolls each loop
- **DICE** = new master seed, applied at the quantize boundary. **Seed history**: ring of last 64 master seeds with back/forward navigation
- **A/B slots** park two full generative states (seeds + operator params + macros)
- **Anchor locks**: per-step and per-lane; locked material is immune to all operators AND commit overwrites
- **COMMIT** renders current audible output into the pattern core (one-level UNCOMMIT); UI document edits have unlimited undo separately
- **Quantized apply** (instant/beat/bar, default bar) for seed changes, pattern switches, commits
- RNG streams are versioned in the schema; any change that alters output for existing seeds requires a version bump + migration note + justified golden update

### 5.3 Macros & mod matrix

CHAOS (scales all operator amounts), DENSITY (SPARSE/DENSE + ratchet promotion + euclid pulses), MOTION (DRIFT rate + TURING mutate + EVOLVE rate), SHAPE (octave spread + direction morph + velocity arc). Macros are mod-matrix sources; targets: internal arp params, hosted plugin parameters (via `AudioProcessorParameter`, wiggle-to-learn), MIDI CC out. MOD A/B lanes are per-step mod sources.

### 5.4 Provenance (X-RAY)

Every emitted note carries a provenance bitmask (core, each operator slot, constraint-snapped, ratchet-child). Rides the step event into the `EngineSnapshot`; UI renders badges. Also the debug view during development.

### 5.5 MIDI correctness invariants

- Engine owns every note-off; sounding-note table tracks all live notes
- Flush (all-notes-off + sustain-off) on: transport stop, pattern switch, pool change, plugin swap. Table must be empty after flush (asserted in tests)
- Overlap policy: LEN>100% ⇒ tie/legato; same-pitch retrigger ⇒ note-off then note-on with 1-sample gap
- Tempo changes and pattern switches land on block boundaries; within a block, events are strictly sample-sorted
- Panic: CC123 + per-note offs to synth and MIDI out

---

## 6. Plugin Hosting Subsystem

### 6.1 Discovery & scanning
- `AudioPluginFormatManager` with `VST3PluginFormat` + `AudioUnitPluginFormat`; `KnownPluginList` persisted as XML in `~/Library/Application Support/ARPBOX/`
- Search paths: `~/Library/Audio/Plug-Ins/VST3`, `/Library/Audio/Plug-Ins/VST3`, user-added paths; AU via the system component registry
- **Out-of-process scanning** (required by beta): `scanner-helper` console binary supervised via `ChildProcessCoordinator`/`ChildProcessWorker`, wired through `KnownPluginList::CustomScanner` (AudioPluginHost pattern). Dead-man's-pedal file quarantines crash-on-scan plugins to a user-overridable blocklist. In-process scanning is an explicitly temporary early-phase state
- Rescan is incremental (`pluginNeedsRescanning`); a full rescan is a user action

### 6.2 Instantiation lifecycle
1. Message thread; `createPluginInstanceAsync` (mandatory for AUv3, used uniformly)
2. On success: `prepareToPlay(currentSR, currentBlockSize)`, then state blob applied (if restoring), then insertion into the graph (async update), then crossfade in
3. On failure: slot shows error state; previous plugin (if any) keeps running; failure never propagates

### 6.3 HostedPluginNode wrapper
Every hosted plugin is wrapped. Responsibilities: crossfaded soft-bypass; per-slot dry/wet with a latency-compensated dry path (FX only); input/output gain trims; `setBusesLayout` negotiation (mono→stereo adaptation, refuse exotic layouts gracefully); output NaN/Inf scrub + denormal guard; latency change forwarding (triggers graph latency recompute + UI event); parameter enumeration/attachment for the mod matrix; state capture/restore.

### 6.4 Editor windows
Per-plugin `DocumentWindow` (native NSWindow) with ARPBOX-styled chrome; remembered position/size per plugin UID; always-on-top toggle. Third-party editors are NEVER embedded in the main panel. A generic editor (auto-generated from parameters, retro-styled) exists for every plugin and hosts the wiggle-to-learn flow.

### 6.5 State & identity
State = opaque `getStateInformation` blob, base64 in the project file. Identity = format + UID (+ name fallback for VST3↔AU sibling matching, user-confirmed). Missing plugin on load ⇒ placeholder slot preserving the blob; silent restore when the plugin reappears. Never discard user state.

---

## 7. FX Rack & Sound Section

- **Synth slot**: one instrument plugin; LCD-strip UI (name/preset), open editor / generic / swap (crossfaded) / gain / MIDI channel
- **FX rack**: 6 serial slots, each a `HostedPluginNode`; drag reorder = async graph rebuild + crossfade; per-slot save/load
- **Chain presets** (rack + states) and **Kit presets** (synth + rack + states)
- **Master**: gain → brickwall safety limiter (default ON) → meter tap → recorder tap. Recorder: arm + play captures master to WAV/AIFF via `ThreadedWriter` on a disk thread; files land in the project bin with drag-out

---

## 8. Data Model & File Formats

### 8.1 Project file `.arpx` (zip container)

```
project.arpx
├── project.json        # schema-versioned; everything below
├── plugins/
│   ├── synth.state     # opaque blob
│   └── fx-{1..6}.state
└── meta.json           # appVersion, schemaVersion, created/modified
```

`project.json` top-level schema (human-readable, diffable; forward-migration on load):

| Key | Type | Contents |
|---|---|---|
| `schemaVersion` | int | Bumped on breaking model changes; loaders migrate forward |
| `rngVersion` | int | RNG stream algorithm version (determinism contract) |
| `transport` | obj | bpm (20–300), swingPct, grid, quantizeApply |
| `keyScale` | obj | root (0–11), scaleId or custom mask (12 bools) |
| `mode` | enum | `thru` \| `self` |
| `patterns[16]` | arr | Per pattern: lanes (per-lane: length, division, steps[]), directionMode, euclid {steps,pulses,rotate}, chordLane[8], operators[≤8] {typeId, amount, seed, laneMask, bypass, params{}}, masterSeed, loopLock, anchors (step/lane masks), macros[4], constraints {scale, rangeLo, rangeHi, foldMode, noteBudget, maxRepeats}, abSlots |
| `chain` | arr | [{patternIndex, repeats}] |
| `rack` | obj | slots[6]: {pluginRef {format, uid, name}, bypass, dryWet, gainIn, gainOut} + synth slot ref |
| `modMatrix` | arr | [{source, targetType (arpParam\|pluginParam\|midiCC), targetId, depth}] |
| `master` | obj | gain, limiterOn, recorderFormat |
| `midi` | obj | inputs[], outputMirror {enabled, device}, clockOut |

Lane step values are stored as dense arrays of the lane's native type (§12.1 ranges). Autosave: every 60 s + on significant edits to a rotating slot; crash recovery offered on next launch.

### 8.2 Preset taxonomy (single-file zips, same scheme)

| Preset | Contains |
|---|---|
| Pattern | lanes + step data |
| Style | operator stack + constraints + macros (no steps) — the shareable "generative character" |
| FX Chain | rack config + plugin states |
| Kit | synth + rack + states |
| Project | everything |

---

## 9. MIDI, I/O & Export

- **Audio**: CoreAudio via `AudioDeviceManager`; device/buffer/SR picker; device-death → auto-fallback to default + banner
- **MIDI in**: all/selected devices; virtual "ARPBOX In" port; QWERTY + on-screen pads (message thread → command FIFO → merged in MIDI In node)
- **MIDI out**: optional mirror of generated MIDI to hardware and/or virtual "ARPBOX Out" (sequences external gear/DAWs); MIDI clock out. Clock follow + Ableton Link are post-MVP
- **MIDI drag-out**: drag from the roll ⇒ `.mid` of the pattern as currently sounding (current seeds, operators applied); n-bar unroll when LOOP LOCK is off. Rendered by an offline engine pass (same code path as real-time — determinism contract applies)
- **Audio export**: recorder (live capture) + offline bounce of pattern/chain

---

## 10. UI Architecture

### 10.1 Contract with the engine
UI reads: `EngineSnapshot` (triple buffer, 60 fps via `VBlankAttachment`) + discrete event FIFO. UI writes: `EngineCommandQueue` POD commands only. UI edits go to the message-thread `PatternDocument`, which rebuilds and publishes `PatternSnapshot`s. The UI NEVER touches audio-thread state directly.

### 10.2 Layout (single window, min 1280×800)
Header (transport, 7-seg BPM, swing, key/scale, clock, CPU, project) over three columns: **left = Sound** (synth slot, FX rack, master/recorder), **center = piano roll** + lane strip + 16-pad strip, **right = Generate** (DICE, seed history, A/B/COMMIT/LOOP LOCK, 4 macros, operator stack cards, constraints strip). Visual reference: `design/arpbox_ui_mockup.html`. Full spec: docs/DESIGN_SYSTEM.md.

### 10.3 Piano roll (primary editor)
- A *view* over the pattern core: a note block = (gate + pitch/oct + len) on its column; remaining lanes edit in the collapsible lane strip and appear as per-note badges (prob %, condition tag, ratchet ticks, anchor pin)
- Scale-aware rows (in-scale tinted; FOLD mode collapses to pool degrees). THRU mode relabels rows as chord-relative degrees; SELF mode shows absolute pitches
- **Solid vs ghost**: committed notes solid; operator-generated uncommitted notes dashed/ghost with source tag. Core product concept — preserved in every note-rendering view
- Interactions: click punch-in, click-note delete, drag move, edge-drag length, option-vertical velocity, marquee + copy/paste, right-click popover (prob/cond/ratchet), shift-click anchor lock
- Rendering: grid background cached to an Image layer; note layer + playhead repaint independently

### 10.4 Component library
`SegDisplay`, `SkirtKnob`, `RubberPad`, `PianoRoll`, `LaneStrip`, `OperatorCard`, `SilkPanel` in `ui/components/`; all visuals from `ui/Tokens.h` (mirror of docs/DESIGN_SYSTEM.md); two skins (Midnight, 3000 Gray) share one semantic token schema.

### 10.5 Secondary windows
Plugin editors (§6.4), plugin manager (scan UI, blocklist), settings (audio/MIDI devices), preset browser (overlay).

---

## 11. Performance Budgets

| Metric | Target |
|--------|--------|
| processBlock (synthetic heavy graph: synth + 6 FX fakes + full operator stack) | < 3 ms @ 128 samples / 48 kHz |
| Audio-thread allocations in steady state | 0 (asserted by operator-new guard) |
| UI frame budget (full-window repaint worst case) | < 8 ms; steady-state dirty-rect repaints < 2 ms |
| App cold start (no scan) | < 2 s to interactive |
| Plugin scan (out-of-process, 100 plugins) | Non-blocking UI; progress surfaced |
| Project save (autosave path) | < 100 ms message-thread stall, serialization on worker |
| Project load (10 plugins) | < 5 s with progressive slot fill |
| End-to-end MIDI-in → audio-out added latency (excl. plugin latency) | ≤ 1 block |

---

## 12. Quick Reference Tables

### 12.1 Parameter lanes

| Lane | Range | Notes |
|---|---|---|
| GATE | on/off | Trig lane; euclidean generator writes it |
| PITCH | −24..+24 degrees | Pool index offset (THRU) or absolute degree (SELF) |
| OCT | −4..+4 | |
| VEL | 1–127 | |
| LEN | 1–400 % of step | >100% ⇒ tie/legato |
| RATCHET | 1–8 | Per-ratchet velocity ramp + probability |
| MICRO | −50..+50 % step | Swing applies on top |
| PROB | 0–100 % | |
| COND | enum (§12.2) | |
| MOD A / MOD B | 0–127 | Mod-matrix sources |

### 12.2 Trig conditions
`A:B` cycles (1:2, 2:2, 1:4, 2:4, 3:4, 1:8 … pattern-loop-aware), `1ST`, `!1ST`, `FILL`, `!FILL` (FILL = held pad 16), `PRE`, `!PRE` (previous step's result), `NEI`, `!NEI` (neighbor mod lane's result). Conditions gate before probability rolls.

### 12.3 Direction modes
up, down, up-down (incl. endpoints), up-down (excl.), converge, diverge, outside-in, as-played, walk (±1 brownian), random-no-repeat, spiral.

### 12.4 Operators

| Operator | Core param(s) | Notes |
|---|---|---|
| DICE | amount = fraction of steps re-rolled | Lane-targeted, range-bounded |
| DRIFT | rate, gravity | Brownian walk on pitch/vel/micro |
| TURING | loop↔mutate (0–100) | Shift register on pitch lane |
| MARKOV | order (1–2), style/learned table | Table precomputed message-thread |
| EUCLID MORPH | density | Continuous pulse/rotate interpolation on GATE |
| RATCHETIZER | amount, beat weighting | Promotes steps to ratchets |
| HUMANIZE | timing ms, vel units | Gaussian jitter (absolute units) |
| STRUM | span, curve | Chord-mode roll spread |
| VOICE-LEAD | strength | Minimize movement between chords |
| SPARSE/DENSE | bias, beat protection | Rest injector / gap filler |
| EVOLVE | rate (bars), mutation % | Prints to working buffer; respects anchors |

### 12.5 Key commands (build/test)

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
ctest --test-dir build -L determinism --output-on-failure   # contract suites:
                        # determinism | midi-conformance | hosting-lab | perf-budget
```

Suites are selected with `-L` (LABEL), not `-R` (name):
`catch_discover_tests(... ADD_TAGS_AS_LABELS)` maps every Catch2 `[tag]` to a CTest
label, which is what makes the suites independently selectable. Test cases are
additionally named `<suite>/<unit>: <behavior>` (e.g. `determinism/golden: …`), so
`-R determinism` also happens to match — but the label is the authoritative selector
and the one CI uses.

---

*This document evolves with implementation. Section numbers are stable; append new subsections rather than renumbering.*
