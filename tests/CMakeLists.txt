# ─────────────────────────────────────────────────────────────────────────────
# ARPBOX test harness — Catch2 v3 via CTest (docs/INSTRUCTIONS.md Phase 1.2,
# docs/ARCHITECTURE.md §3.1 "Catch2 v3 via CTest", §12.5 build/test commands).
#
# One test executable (arpbox_tests) links Catch2::Catch2WithMain and the engine
# static library, so future headless engine tests (construct → prepareToPlay →
# processBlock loops) already have their link seam. Each of the five suites is a
# single Catch2 TEST_CASE tagged with its suite name; catch_discover_tests(...
# ADD_TAGS_AS_LABELS ...) maps every Catch2 [tag] to a CTest label, which is how
# the contract suites become independently selectable via `ctest -L <label>`.
#
# Contract suites (ARCHITECTURE §12.5): determinism | midi-conformance |
# hosting-lab | perf-budget, plus unit. These are smoke placeholders proving the
# labeled pipeline; later phases flesh out the real determinism/fuzzer/perf logic.
# ─────────────────────────────────────────────────────────────────────────────

include(FetchContent)

# Catch2 v3 — pinned to a stable v3 tag. v3.7.0+ is required for the
# ADD_TAGS_AS_LABELS option on catch_discover_tests (tags → CTest labels).
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.8.1
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Catch2)

# Catch2 ships its CMake integration helpers (Catch.cmake → catch_discover_tests)
# in extras/. Add it to the module path, then pull in CTest + the helper.
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# ── Test executable ──────────────────────────────────────────────────────────
# One binary carrying all suites. Catch2WithMain supplies main(); the engine lib
# is linked so headless engine tests construct real engine objects.
#
# Phase 2.4 infrastructure tests (ARCHITECTURE §3.4, §11): the SPSC FIFO / command
# / event channels, the snapshot triple buffer + retirement queue, the concurrent
# TSan stress, the headless EngineGraph render smoke, and the zero-allocation
# steady-state guard. support/AllocationCounter.cpp REPLACES the global operator
# new/delete for the allocation guard — it must live in THIS test binary only
# (never in arpbox_engine / the app); see support/AllocationSentinel.h.
add_executable(arpbox_tests
    smoke_unit.cpp
    smoke_midi_conformance.cpp
    smoke_perf_budget.cpp
    # Phase 2.4 infrastructure test suite.
    infra_fifo.cpp
    infra_snapshot.cpp
    infra_tsan_stress.cpp
    graph_smoke.cpp
    infra_alloc_guard.cpp
    support/AllocationCounter.cpp
    support/AllocationSentinel.h
    # Phase 3.3 hosting-lab: hostile fake-plugin corpus + scan/instantiate/fail-path.
    # The [hosting-lab] smoke placeholder (smoke_hosting_lab.cpp) is REPLACED by the
    # real lab below (its own header comment anticipated this).
    fakes/FakePlugins.h
    fakes/FakePlugins.cpp
    fakes/FakePluginFormat.h
    fakes/FakePluginFormat.cpp
    fakes/HostingLabSupport.h
    hosting_scan.cpp
    hosting_instantiate.cpp
    # Phase 4.4 synth-hosting integration + RT tests (HostedPluginNode wrapper, the
    # MIDI-In node, and the fake-synth end-to-end / swap-under-playback graph paths).
    fakes/HostedSynthGraphSupport.h
    hosted_node_unit.cpp
    midi_input_node.cpp
    # Phase 5.1 transport: exact/drift-free PPQ, buffer-size independence, tempo and
    # stop/locate command handling, head-node render ordering, the custom playhead as
    # seen by a hosted node, and the transport path's allocation guard.
    transport_clock.cpp
    # Phase 5.3a: the reusable headless MIDI render harness (absolute sample positions,
    # byte-identical stream comparison, canonical golden byte stream, graph-internal
    # tap) plus its own tests. This is the foundation of the Phase-6 golden-MIDI
    # determinism suite — 6.4 builds on support/MidiRenderHarness.h, not on ad-hoc
    # per-file render loops.
    support/MidiRenderHarness.h
    midi_render_harness.cpp
    # Phase 5.3c/5.3e: the buffer-size-independence sweep (the Phase 5 headline
    # criterion "test pattern plays in perfect time at any buffer size", and the
    # regression guard for the two determinism snaps in SequencerProcessor) plus the
    # sequencer / stop-flush matrix (§5.5 note-off ownership, the sounding-note table,
    # the seeded churn fuzzer and the transport+sequencer allocation guard).
    # support/NoteLifecycleCheck.h is the reusable note-balance checker Phase 8.3's
    # hanging-note fuzzer reuses; support/SequencerRenderRig.h drives Transport +
    # SequencerProcessor in the graph's own command-drain-then-beginBlock order.
    support/NoteLifecycleCheck.h
    support/SequencerRenderRig.h
    transport_timing.cpp
    sequencer_node.cpp
    # Phase 5.3d: success criterion "hosted fake plugin observes correct BPM/PPQ via
    # AudioPlayHead" on the WRAPPED path (HostedPluginNode → inner instance), including
    # the regression guard for the wrapper's one-line setPlayHead forwarding.
    transport_playhead.cpp
    hosting_synth_e2e.cpp
    hosting_swap_stress.cpp
    # Issue #19 in-process-scan crash-recovery stopgap: dead-man's-pedal blacklist,
    # blacklist persistence round-trip, and incremental-resume skip (headless).
    hosting_crash_recovery.cpp
    # Regression guard: the REAL production format registration helper actually
    # registers VST3 + AudioUnit (guards the Phase 3 "0 formats" host-flag bug).
    hosting_formats.cpp
    # Phase 6.4: the golden-MIDI FILE infrastructure — the on-disk half of the
    # determinism contract (ARCHITECTURE §1.2), built and proven BEFORE the first
    # golden is baked. support/GoldenMidiFile.h serializes a MidiRenderResult to the
    # canonical TEXT format in tests/golden/ (see tests/golden/README.md for why text
    # and not .mid), parses it back through a strict, exhaustively-rejecting loader,
    # and compares on the parsed event vector. golden_format.cpp asserts each parser
    # rejection individually and proves the regeneration interlock: a run that
    # rewrites a golden can never also report green.
    # The [determinism] smoke placeholder (smoke_determinism.cpp) is REPLACED by this
    # (its own header comment named 6.4 as its successor). A REQUIRE(true) in a
    # contract suite is itself an anti-vacuity hazard — a broken `-L determinism`
    # selector would still have reported one passing test.
    support/GoldenMidiFile.h
    golden_format.cpp
    # Phase 6.4: the two L1 pattern-core PRIMITIVES, tested against hand-derived
    # values rather than a frozen stream — hence [unit], not [determinism]. These
    # pin the musical semantics that 6.4's golden MIDI then freezes permanently, so
    # they are what says a golden was baked from the RIGHT rhythm/traversal.
    #
    # pattern_euclid: the euclidean GATE-lane necklace, its rotation convention
    # (positive = later, modulo steps, negative included) and the pulse-count sweep
    # that no constant/truncating/overflowing generator survives.
    pattern_euclid.cpp
    # pattern_directions: the 11 §12.3 traversal tables — the nine deterministic
    # orders pinned as literals AND compared all-pairs (a switch falling through to
    # `up` would pass nine separate literal checks), the buildTraversal/
    # traversalPeriod agreement sweep, and property tests for the two seeded modes.
    pattern_directions.cpp
    # Phase 6 task 0c: the regression guards for the two Phase-5-gate determinism
    # findings that Phase 6's PITCH lane / pool made reachable — issue #36 (an
    # already-due note-off must keep its TRUE sample position under a same-pitch
    # retrigger, byte-identical from block 32 to 4096; the PRE-GOLDEN BLOCKER, since a
    # golden baked while it was live would freeze buffer-size-dependent output as the
    # permanent reference) and issue #37 (the one-sample snap-boundary window, whose
    # documented bound this turns into a guard that reddens if the grid coarsens).
    sequencer_retrigger.cpp
    # Issue #48's permanent regression test, scoped to the whole FAILURE CLASS rather
    # than the single instance: a note-off placed from a within-block offset instead
    # of from its own absolute schedule (#36 / #46 / #48 — one root cause, three
    # sites, none of them caught by the suite; with the #48 fix reverted all 189
    # permanent tests still passed). Six scenarios cover every note-off emission
    # path — natural gate end, both same-pitch retrigger branches, the MID-BLOCK
    # switch flush, the block-head PRE-FLUSH sibling, the discontinuity flush, and
    # all of them combined — each swept across the ten block sizes and compared on
    # bytes AND event count AND CC123 placement, with a per-path reachability
    # assertion. The 1/32 grid at 300 BPM (1200-sample steps) is what puts three
    # steps inside one 4096-sample block; pattern_switch.cpp's 6000-sample steps
    # structurally could not, which is the gap this file closes.
    sequencer_offdeterminism.cpp
    # ── Phase 6 task 12: the coverage suite for 6.1-6.3 (pattern model, lanes,
    # polymeter, quantized switching, snapshot lifecycle). Everything the Phase 6.4
    # golden bake depends on — these say a golden was baked from the RIGHT engine.
    #
    # pattern_switch: the quantized pattern switch (§5.2/§6.1) — four modes against
    # four DISTINCT hand-computed absolute-sample literals (the control that catches a
    # quantizer ignoring its enum), the exactly-on-a-bar-line case, the
    # switch-to-self no-op, request-survives-stop/locate, and the §5.5 switch flush.
    pattern_switch.cpp
    # pattern_polymeter: the L1 lane engine against a closed form written INDEPENDENTLY
    # in the test — the gated-step set, the 105-step combined period proven by
    # exhaustive search, the gated-cursor rule (a rest does not consume a pool note),
    # and GATE-as-clock-divider vs every-other-lane-as-value-hold.
    pattern_polymeter.cpp
    # pattern_model_unit: the message-thread half — snapshot builder (lane clamping,
    # gate-prefix table, traversal-set dedup, byte-identical rebuilds), PatternChannel's
    # publish/adopt/retire cycle including the displaced-publish path that a `store`
    # instead of an `exchange` would leak, PatternDocument edits/undo/transactions, and
    # the RetirementQueue capacity bound.
    pattern_model_unit.cpp
    # pattern_channel_threading: §3.4 mechanism 3 under two real threads. TSan proves
    # race-freedom; the destructor-recorded thread id proves the OTHER half of
    # INSTRUCTIONS' criterion — "retired snapshots freed on message thread ONLY".
    pattern_channel_threading.cpp
    # pattern_alloc_guard: §11's zero-allocation contract with snapshot ADOPTS and
    # pattern-switch FLUSHES provably inside the armed region (an alloc guard around a
    # region where nothing adopted proves nothing). Also the standing guard on the
    # outgoing MidiBuffer's 8192-byte warm-up: two flushes in one block must not grow it.
    pattern_alloc_guard.cpp
    # step_purity: issue #53's BEHAVIOURAL enforcement. `evaluateStep` is a free
    # function so no SequencerProcessor member is reachable from it (the compile-time
    # half); this file covers what the type system cannot see — a file-scope /
    # function-local static, a thread_local, a memo — by demanding that an in-order
    # reference sweep come back identical under repeated, reversed, shuffled,
    # interleaved and lookahead-shaped call orders. The retrigger lookahead
    # (`cutoffForSamePitch`, the #46 fix) predicts future steps, so an impure core
    # silently reintroduces the #36/#46/#48 buffer-size-dependent class one level up.
    step_purity.cpp
    # boundary_agreement: issue #54's enforcement. `stepBoundarySample` (the #46 fix,
    # deliberately unclamped) and the step-boundary walk in `processBlock` are TWO
    # WRITINGS of one snap-then-floor convention; they agree today and nothing pinned
    # that they continue to. This sweep puts both derivations of the SAME boundary
    # into the emitted stream — the walk places the note-on, `cutoffForSamePitch`
    # places the previous note-off one sample earlier — so a single-sided edit (the
    # per-pattern grid #54 names) reddens instead of resurrecting #36/#46/#48 one
    # level up.
    boundary_agreement.cpp
    # pattern_determinism: the REPRODUCIBLE half of §1.2, distinct from the
    # buffer-size-independent half transport_timing.cpp covers — two fresh renders of
    # one configuration are byte-identical, which is what catches hidden global state.
    pattern_determinism.cpp
    # ── Phase 6 task 13: THE BAKED GOLDENS — the on-disk half of §1.2's determinism
    # contract, and the release gate INSTRUCTIONS Phase 6.4 asks for. Six reference
    # event streams in tests/golden/ (baseline canary, per-lane LENGTH polymeter,
    # per-lane CLOCK-DIVISION polymeter, LEN tie/retrigger on a fractional clock, the
    # nine deterministic §12.3 direction modes, and the euclidean GATE necklaces),
    # each compared at ALL TEN swept block sizes so buffer-size independence and the
    # frozen reference are checked together rather than as two separate sweeps.
    #
    # Everything above this line pins the SEMANTICS against hand-derived values; this
    # file freezes the resulting BYTES. That order is deliberate: the primitives say a
    # golden was baked from the right engine, and the golden then says the engine has
    # not drifted since. Each case additionally carries a perturbed-render negative
    # control (one lane value changed ⇒ must NOT match), without which a comparator
    # that always returned true would leave all six green.
    #
    # `walk` and `randomNoRepeat` are deliberately NOT baked here — Phase 7.1's
    # versioned RngStream would force an immediate regeneration. See the omission note
    # in the file; they keep their property tests in pattern_directions.cpp.
    determinism_goldens.cpp
    # Issue #22/#24: SynthSlot message-thread coordinator unit tests. The coordinator
    # depends only on the GUI-free app/ISynthEngine.h seam (NOT AudioEngine), so
    # app/SynthSlot.cpp compiles into this GUI-free target linking arpbox_hosting only
    # (no juce_audio_utils / juce_audio_devices). See the app/ include root added below.
    ${CMAKE_SOURCE_DIR}/app/SynthSlot.cpp
    synthslot_coordinator.cpp)

target_compile_features(arpbox_tests PRIVATE cxx_std_20)

# Phase 3.3: the hosting-lab must pump the JUCE message loop to receive the async
# instantiation callbacks (PluginInstantiator delivers via MessageManager::callAsync
# and JUCE's async plugin creation). MessageManager::runDispatchLoopUntil is compiled
# only under JUCE_MODAL_LOOPS_PERMITTED. JUCE modules are recompiled per-target, so
# this enables the pump API in the TEST BINARY ONLY — engine/hosting/app are
# untouched. The bounded pump (fakes/HostingLabSupport.h) uses a hard timeout, so no
# wall-clock sleep acts as a synchronization primitive.
#
# Phase 6.4: ARPBOX_GOLDEN_DIR points support/GoldenMidiFile.h at the SOURCE-tree
# tests/golden/ directory. This is the compiled-target analogue of the
# `-DENGINE_DIR=${CMAKE_SOURCE_DIR}/engine` idiom used by the GUI-free check below.
# Deliberately NOT file(COPY)'d into the build tree: a build-tree copy means editing
# a golden and forgetting to rebuild silently reads a STALE reference and PASSES —
# the worst failure class a determinism gate can have. The golden data files are
# likewise NOT listed in add_executable(): listing them would make CMake re-run on
# every golden edit, and they are read at test time, not compile time.
target_compile_definitions(arpbox_tests PRIVATE
    JUCE_MODAL_LOOPS_PERMITTED=1
    ARPBOX_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

# tests/ as an include root so tests reach support/ headers as
# "support/AllocationSentinel.h" (engine headers come via arpbox_engine's exported
# "engine/..." include root). app/ is added so the SynthSlot coordinator sources +
# tests resolve "SynthSlot.h" / "ISynthEngine.h" (issue #22 testability seam); the
# "hosting/..." include root arrives via arpbox_hosting's PUBLIC export.
target_include_directories(arpbox_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/app)

target_link_libraries(arpbox_tests
    PRIVATE
        Catch2::Catch2WithMain
        arpbox_engine
        # Phase 3.3: the hosting-lab exercises the real hosting/ code path. This
        # pulls in the FULL juce_audio_processors (arpbox_hosting links it), which
        # layers on the juce_audio_processors_headless that arpbox_engine already
        # links — JUCE builds each module once, so no duplicate-symbol/ODR clash.
        arpbox_hosting)

# Register each Catch2 TEST_CASE with CTest and turn its [tags] into CTest labels
# (ADD_TAGS_AS_LABELS). PRE_TEST discovery runs the binary at test time rather
# than post-build, which plays nicely with universal / cross-arch builds.
catch_discover_tests(arpbox_tests
    ADD_TAGS_AS_LABELS
    DISCOVERY_MODE PRE_TEST)

# ── Order-independent GUI-free enforcement (Phase 1 success criterion) ─────────
# "engine/ fails to compile if juce_gui_basics is included (enforced check)."
# The compile-time tripwire in engine/EngineGuiGuard.h is bypassable by include
# order, so it does NOT truthfully satisfy that criterion on its own. This CTest
# test is the authoritative, order-independent enforcement: a CMake script
# (tests/CheckEngineGuiFree.cmake) textually scans every engine/ source/header
# and FAILS (non-zero exit) if any file includes juce_gui_basics / juce_gui_extra
# / juce_graphics, no matter where the include sits. It runs in the default
# `ctest --test-dir build` run and in CI. Not a Catch2 case — pure CMake so it
# needs no compilation and cannot be defeated by TU ordering.
add_test(
    NAME "engine/gui-free: no GUI module includes"
    COMMAND ${CMAKE_COMMAND}
            -DENGINE_DIR=${CMAKE_SOURCE_DIR}/engine
            -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckEngineGuiFree.cmake)
set_tests_properties("engine/gui-free: no GUI module includes"
    PROPERTIES LABELS "unit")
