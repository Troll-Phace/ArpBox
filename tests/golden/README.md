# `tests/golden/` — determinism reference MIDI event streams

**If a golden went red and you are here to fix it: read [Rule zero](#rule-zero)
before you touch anything.**

These files are the on-disk half of the ARPBOX determinism contract
(docs/ARCHITECTURE.md §1.2):

> same `(pattern, seeds, N bars)` ⇒ byte-identical MIDI, **forever** … enforced by
> a golden-MIDI test suite and is a release gate.

Machinery: `tests/support/GoldenMidiFile.h` (serialize / parse / compare /
regenerate) on top of `tests/support/MidiRenderHarness.h` (the headless render).
Tests for the machinery itself: `tests/golden_format.cpp`.

---

## Rule zero

**A golden diff is a FINDING, never something to silently regenerate.**

A red golden means one of exactly two things:

1. **A defect.** The engine changed what it plays and nobody meant it to. Saved
   projects would sound different after an update. Fix the engine.
2. **A deliberate change.** Somebody intentionally altered the generative output.
   Per §5.2, *any change that alters output for existing seeds requires an RNG
   version bump + a migration note + a justified golden update.*

There is no third option, and "the test is annoying" is not a diagnosis.
Regenerating a golden to make a build green destroys the only evidence that the
product still sounds the way it did last release.

Committing a regenerated golden requires an **explicit justification line in the
commit body** (`.claude/rules/git-conventions.md`); `.claude/skills/safe-commit`
checks for staged changes under `tests/golden/*` and asks for it.

---

## Running the gate

```sh
ctest --test-dir build -L determinism --output-on-failure
```

Note `-L` (label), not `-R` (name). `catch_discover_tests(... ADD_TAGS_AS_LABELS)`
turns every Catch2 `[determinism]` tag into a CTest label. Golden cases are also
*named* `determinism/golden: <what>`, so `-R determinism` happens to work too — but
the label is the authoritative selector. CI runs this as its own named step
("Determinism gate (ARCHITECTURE §1.2)") so the gate fails loudly if the label ever
empties out.

---

## Why text, and not `.mid`

Phase 6.4 chose a canonical **text** format over Standard MIDI Files. The reasons
are worth keeping written down, because "just write a .mid" is the obvious idea:

- **SMF stores ticks, not samples.** The determinism contract is about *absolute
  sample positions*. Pinning 1 tick = 1 sample requires a fabricated 3000–6000 BPM
  tempo map that differs per sample rate — an encoding artifact sitting between the
  engine's output and the thing under test.
- **`juce::MidiFile::writeTo` is not a stable byte oracle.** Its running-status and
  variable-length encoding are JUCE implementation details. A JUCE submodule bump
  could turn every golden red with **zero engine change**. A gate that fires on the
  wrong signal trains people to regenerate — which is precisely the failure mode
  Rule zero exists to prevent.
- **A text golden is reviewable.** `git diff` shows exactly which event moved,
  which is what "a diff is a finding" requires. A binary blob shows nothing.

The SMF writer's real home is **Phase 18** (§9 MIDI drag-out), where "exported
`.mid` re-imported equals live-rendered golden" makes it a *product output* tested
**against** these files, rather than the reference itself.

---

## Format (version 1)

```
# arpbox-golden 1
# name: baseline-4bar
# sampleRate: 48000
# bpm: 125
# gridPpq: 0.25
# spanSamples: 368640
# events: 128
# rngVersion: 0
# bakedAtBlockSize: 128
0 90 3C 64
2880 80 3C 00
5760 90 3E 64
```

### Grammar — strict, no tolerance

| Part | Rule |
|---|---|
| Line 1 | exactly `# arpbox-golden <version>`; magic + version, so a future format change is a loud parse error rather than a silent mis-read |
| Header | `# <key>: <value>`, one space after the colon; all nine fields required, exactly once each, no unknown keys, header block before any event line |
| Event | `<decimal int64> SP <2 uppercase hex digits> ( SP <2 uppercase hex digits> )*` |
| Whitespace | single-space separators only. No blank lines, no trailing whitespace, no tabs, no CR |
| Line endings | LF only, enforced on checkout by `.gitattributes` (`tests/golden/** text eol=lf`) |
| File end | must end with a final LF |
| Ordering | sample positions non-decreasing (§5.5 "within a block, events are strictly sample-sorted") |
| Non-empty | a golden with zero events is rejected — it would assert nothing |

`MidiRenderHarness::toByteStream()` deliberately carries no magic and no version,
because it is an in-memory comparison target. On disk you need both, which is why
this format is not simply that byte stream hex-dumped.

### Header fields

| Field | Compared? | Meaning |
|---|---|---|
| `name` | no | scenario name; also the file's base name |
| `sampleRate` | no (advisory) | render rate. A change here moves every absolute position, so the event diff is the honest signal |
| `bpm` | no | scenario tempo, for the reader |
| `gridPpq` | no | step grid in quarter notes, for the reader |
| `spanSamples` | no (advisory) | absolute samples the render covered |
| `events` | **structurally** | cross-checked against the parsed body; a mismatch is a parse error (catches truncation) |
| `rngVersion` | **per scenario** | **IN USE SINCE PHASE 7.** The six Phase-6 files are stamped `0` (no RNG existed in their audible path); Phase 7's six are stamped `1`. `determinism_goldens.cpp`'s inventory case checks each file against a per-scenario expectation table — the blanket `== 0` it used to carry became unsatisfiable the moment a second version existed, in the only two ways available: stamping the new files `0` (a lie about the schema they were produced under) or a spurious failure. Not part of the event comparison. |
| `bakedAtBlockSize` | **NEVER** | see below |

**`bakedAtBlockSize` is non-normative and must never be asserted on.**
`MidiRenderHarness.h` is right that a golden must not encode the buffer size it
happened to be rendered at — buffer-size independence is exactly the property the
suite asserts (Phase 5.3). It is recorded because it costs nothing and tells a
future reader which render produced the file. It is the kind of field a later
reader will wrongly start comparing; `tests/golden_format.cpp` has a test that
fails if anyone does.

### What comparison actually compares

The **parsed event vector**, not the raw file bytes. Consequences:

- Editing a header comment is *not* a false failure.
- Any change to the performance — a moved sample, a changed byte, an added or
  dropped event — *is* a failure.
- Reformatting the file (which the writer would never do) is invisible; changing
  what plays is not.

---

## Writing a golden case

```cpp
const auto render = renderProcessor (sequencer, MidiRenderConfig::bars (4, 125.0));
const auto check  = checkGolden (render, headerFor (render, "baseline-4bar", 125.0, 0.25));
INFO (check.report);
REQUIRE (check.passed);
```

Name the case `determinism/golden: <what>` and tag it `[determinism]`.

`checkGolden` resolves `tests/golden/<name>.txt` through the compiled-in
`ARPBOX_GOLDEN_DIR`, which points at the **source tree**, not the build tree — so
editing a golden takes effect immediately and can never be shadowed by a stale
build-tree copy.

---

## Regenerating — and why it always fails

Regeneration is gated on an **environment variable**, not a CMake option. A
configure-time flag persists in `build/` across sessions and nobody re-reads
`CMakeCache.txt`; "left it on by accident" there would leave the release gate
permanently green and worthless.

```sh
ARPBOX_REGENERATE_GOLDENS=1 ctest --test-dir build -L determinism --output-on-failure
```

**This run writes the new files and STILL FAILS.** That is deliberate and load-
bearing: there is no invocation of this suite that both rewrites a golden and
reports green. The workflow is:

1. `ARPBOX_REGENERATE_GOLDENS=1 ctest … -L determinism` → **RED**, with the diff
   and the governance rule printed.
2. `git diff tests/golden/` → read every changed line and explain it (Rule zero).
3. If it is a defect, fix the engine and `git checkout tests/golden/`.
4. If it is deliberate, keep the change, bump `rngVersion` if output changed for
   existing seeds, and write the justification line for the commit body.
5. `ctest … -L determinism` (no env var) → **GREEN**, or the diff was a real defect.

A **missing** golden behaves the same way: the file is created and the run fails
with *"golden created — review and re-run"*. A golden that no human has read
proves nothing, so it never counts as a pass on the run that produced it.

The writer compiles unconditionally (it is not `#ifdef`'d out) so it cannot
bit-rot; only its *execution* is gated.
