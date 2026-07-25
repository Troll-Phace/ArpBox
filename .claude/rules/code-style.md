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
- Language level: C++20. No exceptions across module boundaries; audio-thread code is `noexcept` in practice
- Error handling: `juce::Result` or `std::optional`/`tl::expected`-style returns on the message thread; hard `jassert` + graceful degradation (never crash) in engine code. Plugin-facing calls are wrapped and failure-isolated — a misbehaving plugin must never take the app down
- Naming (JUCE conventions): `PascalCase` types, `camelCase` functions/variables/members (no `m_` prefix), `SCREAMING_SNAKE` only for macros, one class per header where practical
- Module organization: `engine/` has ZERO UI dependencies (no `juce_gui_basics`). `ui/` never touches the audio thread directly — it reads `EngineSnapshot` and pushes commands through the `EngineCommandQueue`. `hosting/` owns all `AudioPluginInstance` lifecycle
- Ownership: `std::unique_ptr` by default; `juce::ReferenceCountedObjectPtr` only where JUCE APIs require it; raw pointers are non-owning observers only
- Comments on all public functions/methods; every audio-thread entry point documented with a `// RT-SAFE:` or `// MESSAGE-THREAD ONLY:` marker

## Real-Time Safety (hard rules — audio thread / processBlock call tree)
- NO allocation (`new`, `malloc`, container growth, `String` construction)
- NO locks (`std::mutex`, `CriticalSection`), NO file/network I/O, NO logging
- NO `rand()`/`std::mt19937` construction — use the preallocated `RngStream` (xoshiro256++)
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
