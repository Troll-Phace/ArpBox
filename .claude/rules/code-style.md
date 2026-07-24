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
- clang-format: applied on save (enforced by PostToolUse hook; config in `.clang-format`, JUCE-derived style)
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
- No `using namespace` in headers; `using namespace juce` allowed in .cpp only

## Prohibited Patterns
- `juce::Timer` for anything sequencing-related (message-thread timers jitter; the clock lives in the audio graph)
- Blocking the message thread on plugin instantiation (use async paths; AUv3 requires it)
- Touching `AudioProcessorGraph` topology from the audio thread (edits are message-thread, `UpdateKind::async`)
- Hardcoded colors/fonts/dimensions in `ui/` (use `ui/Tokens.h` — see .claude/rules/design-system.md)
- `getStateInformation` blobs parsed or interpreted — they are opaque; only stored/restored
