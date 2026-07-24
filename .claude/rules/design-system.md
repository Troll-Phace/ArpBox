---
paths:
  - "ui/**/*.{h,hpp,cpp}"
---

# Design System Rules — ARPBOX UI

- ALL colors, spacing, radii, font sizes, and animation durations come from `ui/Tokens.h` — NEVER hardcode a `juce::Colour(0x...)`, pixel value, or ms duration in component code
- Token values and component specs are defined in docs/DESIGN_SYSTEM.md; `ui/Tokens.h` is its machine-readable mirror. If a needed token doesn't exist, add it to BOTH (doc first, then header) — don't inline a literal
- Two skins (Midnight, 3000 Gray) share one token schema: components reference semantic tokens (`panelBg`, `noteFill`, `ghostOutline`), never skin-specific values
- The retro budget is fixed: 7-seg BPM display, silkscreen labels, rubber pads, skirted knobs, red accent rule, red DICE button. Editor surfaces (piano roll, lane strip, browsers) are modern flat dark-UI — no scanlines, glows, or pixel fonts on working surfaces
- Custom components live in the `ui/components/` library (`SegDisplay`, `SkirtKnob`, `RubberPad`, `PianoRoll`, `LaneStrip`, `OperatorCard`, `SilkPanel`) — compose these; don't fork one-off variants
- Every interactive component: keyboard focusable, visible focus indicator, double-click type-in, option-drag fine adjust, cmd-click reset to default
- Text contrast ≥ 4.5:1 against its surface in BOTH skins (validate when adding tokens)
- Animations use `juce::VBlankAttachment`-driven repaints at 60 fps; respect the app-level reduced-motion setting (all animations must have a zero-duration path)
- Repaints are dirty-rect scoped: the piano roll's grid background is cached to an Image layer; note-layer and playhead repaints never trigger full-component repaints
- Ghost (generated, uncommitted) notes always render visually distinct from committed notes — dashed `ghostOutline` vs solid `noteFill` — in every view that shows notes
