---
paths:
  - "app/**/*.{h,hpp,cpp,mm}"
  - "engine/**/*.{h,hpp,cpp}"
  - "hosting/**/*.{h,hpp,cpp,mm}"
  - "scanner-helper/**/*.{h,hpp,cpp}"
  - "ui/**/*.{h,hpp,cpp}"
---

# Code Standards — ARPBOX

## C++20 / JUCE Standards
- clang-tidy: clean, no warnings (config in `.clang-tidy`; checks include `bugprone-*`, `performance-*`, `concurrency-*`)
- clang-format: applied on save by the `PostToolUse` hook in `.claude/settings.json`, which calls `.claude/skills/run-lint/lint.sh format-file`; config in `.clang-format` (JUCE-derived style). The formatter is **Homebrew LLVM's** `clang-format` (`/opt/homebrew/opt/llvm/bin/clang-format`, `brew install llvm`) — bare `clang-format` is not on `PATH` on macOS and the Xcode toolchain's copy is an older major. If it is missing the hook now **fails loudly** instead of silently doing nothing (issue #30).
- CI enforces formatting too: the blocking `clang-format` job runs `lint.sh format` (`--dry-run --Werror`) over every tracked source, so the tree cannot drift. Check locally with `bash .claude/skills/run-lint/lint.sh format`; a whole-tree reformat is a deliberate, standalone commit (`ARPBOX_ALLOW_FORMAT_FIX=1 ... format-fix`), never mixed into a feature change.
- Compiler warnings are a **blocking gate**, separate from clang-tidy: `bash .claude/skills/run-lint/lint.sh warnings` recompiles every ARPBOX translation unit and fails on any `warning:` in `app/ engine/ hosting/ scanner-helper/ ui/`. CI runs it as `compiler warnings (blocking)`. clang-tidy never sees compiler diagnostics, so before this existed the whole `juce_recommended_warning_flags` set — including `-Wswitch-enum`, which the code relies on by name — was enforced by nothing (issue #79)
- Switches over an enum list **every enumerator explicitly and carry no `default:`** — cases you intend to ignore go in one grouped no-op arm (`case a: case b: … break;`). A `default:` still trips `-Wswitch-enum` on a new enumerator, but dropping it means `-Wswitch` (inside `-Wall`) flags it too, and it makes the ignore set readable. This is contractual for `ICommandSink::applyCommand` implementors — see `engine/graph/ICommandSink.h`. Where an arithmetic dispatch genuinely beats 30 hand-written arms, say so in a comment at the site (`engine/sequencer/StepLogic.cpp` is the precedent)
- Language level: C++20. No exceptions across module boundaries; audio-thread code is `noexcept` in practice
- Error handling: `juce::Result` or `std::optional`/`tl::expected`-style returns on the message thread; hard `jassert` + graceful degradation (never crash) in engine code. Plugin-facing calls are wrapped and failure-isolated — a misbehaving plugin must never take the app down
- Naming (JUCE conventions): `PascalCase` types, `camelCase` functions/variables/members (no `m_` prefix), `SCREAMING_SNAKE` only for macros, one class per header where practical
- Module organization: `engine/` has ZERO UI dependencies (no `juce_gui_basics`). `ui/` never touches the audio thread directly — it reads `EngineSnapshot` and pushes commands through the `EngineCommandQueue`. `hosting/` owns all `AudioPluginInstance` lifecycle
- Ownership: `std::unique_ptr` by default; `juce::ReferenceCountedObjectPtr` only where JUCE APIs require it; raw pointers are non-owning observers only
- Comments on all public functions/methods; every audio-thread entry point documented with a `// RT-SAFE:` or `// MESSAGE-THREAD ONLY:` marker

## Real-Time Safety (hard rules — audio thread / processBlock call tree)
- NO allocation (`new`, `malloc`, container growth, `String` construction)
- NO locks (`std::mutex`, `CriticalSection`), NO file/network I/O, NO logging
- NO `rand()`/`std::mt19937` construction. Randomness inside the **pure emission core**
  (`evaluateStep` and everything it calls) is drawn as a **per-index hash** —
  `rng::stepHash (masterSeed, domain, stepIndex)` / `rng::subStepHash (…, childIndex)` from
  `engine/generative/Rng.h` — **never** from a running stream. A stream is only pure if it is
  drawn from a FIXED number of times, and PRE chains and variable ratchet counts both break
  that; a cursor there is an issue-#53 violation no matter where it is parked (a file-scope
  `static` included). `RngStream` (xoshiro256++), when Phase 12 introduces it, is for the
  **operator stack** (§5.1 L3): stack-local, seeded per (step, operator), never persistent
  across steps. It is not "preallocated" and there is no long-lived RNG object on the audio thread.
- Cross-thread data moves ONLY via `juce::AbstractFifo` command/event queues or atomic
  pointer swaps of immutable `PatternSnapshot`/`EngineSnapshot` objects
- Retired snapshots are returned to the message thread for deletion — never freed on the audio thread
- Denormal protection (`juce::ScopedNoDenormals`) at every processBlock entry

## Import/Include Organization
1. Matching header first
2. Project headers (`engine/`, `hosting/`, `ui/`)
3. JUCE modules
4. Standard library
- Keep the four groups separated by a blank line, and maintain the order **by hand**: `.clang-format` sets `SortIncludes: false` on purpose — reordering includes is a semantic change, and clang-format's default categories would sort stdlib above JUCE inside a single block, violating the order above
- No `using namespace` in headers; `using namespace juce` allowed in .cpp only

## Prohibited Patterns
- `juce::Timer` for anything sequencing-related (message-thread timers jitter; the clock lives in the audio graph)
- Blocking the message thread on plugin instantiation (use async paths; AUv3 requires it)
- Touching `AudioProcessorGraph` topology from the audio thread (edits are message-thread, `UpdateKind::async`)
- Hardcoded colors/fonts/dimensions in `ui/` (use `ui/Tokens.h` — see .claude/rules/design-system.md)
- `getStateInformation` blobs parsed or interpreted — they are opaque; only stored/restored
