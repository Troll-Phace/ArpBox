# ARPBOX — Design System

## Design Philosophy

"Retro chassis, modern surface." MPC nostalgia lives in the frame — silkscreen-labeled panels, an amber 7-segment BPM display, rubber pads, chunky skirted knobs, and one red accent rule — while every working surface (piano roll, lane strip, browsers) is clean, flat, modern dark-UI. The retro budget is fixed and closed: nothing on an editor surface gets scanlines, glows, or pixel fonts. Visual reference: `design/arpbox_ui_mockup.html`. The machine-readable mirror of this document is `ui/Tokens.h` — the two must never drift.

---

## Color System

Semantic tokens; two skins share the schema. Values below are the **Midnight** skin (default). 3000 Gray remaps the same tokens to a warm light-gray chassis with dark editor surfaces.

### Base Palette (Midnight)

| Role | Token | Value | Usage |
|------|-------|-------|-------|
| Chassis | `chassis` | `#232326` | Window body panel (subtle noise texture ≤ 5% opacity) |
| Panel | `panelBg` | `#26262A` | Section boxes (SilkPanel interiors) |
| Editor surface | `surface` | `#1B1B1F` | Piano roll body, browser lists |
| Editor surface deep | `surface2` | `#17171A` | Name strips, lane strip background, wells |
| Silkscreen text | `silk` | `#B8B4AA` | Section labels (uppercase, letterspaced) |
| Silkscreen dim | `silkDim` | `#7A776F` | Secondary labels, hints |
| Outline | `line` | `#3A3A40` | Panel borders, control outlines |
| Outline raised | `line2` | `#46464E` | Button/chip borders |
| Text primary | `textPrimary` | `#DCD9D2` | Values, strip contents |
| Text bright | `textBright` | `#E8E5DE` | Logo, seed display, emphasized values |

### Accent / Semantic Colors

| Role | Token | Value | Usage |
|------|-------|-------|-------|
| Primary accent (red) | `accent` | `#E0453A` | Header rule, DICE, record states, current-pattern ring — identity only, never decoration |
| Note fill | `noteFill` | `#FF6D5A` → `#E85643` (vertical gradient) | Committed piano-roll notes |
| Note edge | `noteEdge` | `#C74537` | Committed note border |
| Ghost | `ghostOutline` | `#8F7AE8` | Generated/uncommitted notes (dashed outline), ghost velocity bars, operator seed dots |
| 7-seg amber | `segAmber` | `#FFB02E` | BPM display, numeric amounts, macro values (glow allowed here only) |
| Lane bar | `laneBar` | `#C9A44A` | Lane strip value bars (committed) |
| Success/on LED | `ledOn` | `#55FF22` | Bypass LEDs, limiter LED |
| Meter gradient | `meterLo/Mid/Hi` | `#2C7A42` / `#E8C33A` / `#E0453A` | Level meters (bottom→top) |
| Warning | `warning` | `#E8C33A` | Latency badge, non-blocking alerts |
| Scale row tint | `rowInScale` | `#26262E` | In-scale piano-roll rows |
| Row (natural) | `rowLight` | `#232329` | Out-of-scale natural rows |
| Row (sharp/flat) | `rowDark` | `#1D1D22` | Out-of-scale black-key rows |
| Beat line | `beatLine` | `#3C3C46` | Every 4th column boundary |
| Grid line | `gridLine` | `#28282E` | Step column/row boundaries |
| Playhead | `playhead` | `#FFFFFF` @ 7% fill, 25% left edge | Sweeping play column |

### Contrast requirements
Every text token must hold ≥ 4.5:1 against its designated surface in BOTH skins; validate when adding tokens. `silkDim` is exempt only for redundant hint text.

---

## Typography

| Role | Font | Weight | Size | Tracking / LH |
|------|------|--------|------|---------------|
| Logo | Helvetica Neue / Inter | 800 | 20 px | +0.28 em |
| Silkscreen section label | Helvetica Neue / Inter, UPPERCASE | 600 | 9 px | +0.22 em |
| Silkscreen hint | same, UPPERCASE | 600 | 8 px | +0.15 em |
| Value / strip text | SF Mono / Menlo | 400 | 11 px | tabular |
| Roll row labels / badges | SF Mono / Menlo | 400 | 7–8 px | tabular |
| 7-seg BPM | SF Mono / Menlo (seg-styled) | 400 | 24 px | +2 px |
| Tooltip body | Helvetica Neue / Inter | 400 | 12 px | 1.4 |

No pixel/bitmap fonts anywhere. The 7-seg treatment (amber + inset well + soft glow) is exclusive to the BPM display and numeric readouts in the Generate panel.

---

## Spacing Scale

| Token | Value | Usage |
|-------|-------|-------|
| `space1` | 4 px | Chip padding, badge insets |
| `space2` | 6 px | Grid gaps, slot row gaps |
| `space3` | 10 px | Panel padding |
| `space4` | 14 px | Column gutters, section stacking |
| `space5` | 18 px | Chassis margin |

## Border Radius Scale

| Token | Value | Usage |
|-------|-------|-------|
| `radiusSm` | 3 px | Chips, strips, note blocks, lane bars |
| `radiusMd` | 5–6 px | Panels, operator cards, roll container |
| `radiusLg` | 8 px | Pads, DICE button |
| `radiusXl` | 14 px | Chassis window |

---

## Component Specifications

### SilkPanel (section frame)
- 1 px `line` border, `radiusMd`, `panelBg` fill
- Label chip floats over the top edge: silkscreen style on `chassis` background, `space3` left inset

### SegDisplay (BPM / numeric)
- Inset well: `#160F04` fill, 1 px black border, inner shadow; `segAmber` text with soft glow (the only permitted glow)
- Min width 110 px for BPM; right-aligned

### SkirtKnob
- 52 px (macro) / 20 px (mini) diameter; radial gradient cap, 1 px dark rim, white indicator line with 270° travel (-135°..+135°)
- Interactions: vertical drag, scroll, option-drag fine, cmd-click default, double-click type-in; focus ring 2 px `accent` at 60%

### RubberPad
- Height 38 px strip pads, `radiusLg`, vertical gray gradient, 2 px drop (pressed: 1 px + red glow ≤ 10 px)
- Label bottom-center 7 px mono; states: idle / hit (150 ms decay) / current (`accent` 1 px ring) / FILL (amber label)

### PianoRoll
- Row height 22 px; key column 46 px; 16 columns per bar page
- Committed note: `noteFill` gradient block, `radiusSm`, 18 px tall, velocity tick inset left; badges bottom-right 7 px mono
- Ghost note: transparent fill, 1 px dashed `ghostOutline`, source tag (TUR/DICE/RAT/…) in `ghostOutline`
- Anchor lock: ◆ glyph top-right in white (committed) / amber (step-locked)
- Grid background (rows, lines, tints) cached to an Image layer; notes + playhead on separate layers
- Hover cell: +6% white overlay; marquee: 1 px `accent` at 50% with 8% fill

### LaneStrip
- 52 px tall bars region over `surface2`; bars `laneBar` (ghost-sourced columns use `ghostOutline`), 2 px side insets
- Lane tabs: chip row; active tab `#3A3A44` fill, white text

### OperatorCard
- Row: grip glyph, name strip (`surface2`), seed dot (`ghostOutline`), amount in `segAmber`; bypassed = 45% opacity
- Drag-reorder with 120 ms settle animation; expanded state reveals param grid in a `surface2` sub-panel

### Chip button
- 8 px UPPERCASE, `space1`×8 px padding, `line2` border, gradient fill; lit state: warm dark red fill, white text

---

## Animation Rules

| Animation | Duration / Easing |
|-----------|-------------------|
| Playhead sweep | Continuous, snapshot-driven per vsync (never Timer-estimated) |
| Pad hit decay | 150 ms ease-out |
| DICE roll | 350 ms rotate+scale, ease-in-out |
| Seed history slide | 120 ms ease-out |
| Crossfade-related UI (slot swap spinner) | Match engine crossfade (~50 ms) + settle |
| Card reorder settle | 120 ms ease-out |
| Ghost→committed on COMMIT | 200 ms fill fade-in |

- All animation via `VBlankAttachment`; no `Timer`-driven repaints
- Every animation has a zero-duration path honoring the app-level reduced-motion setting
- Repaint discipline: animations dirty only their own layer/rect

---

## Theme System

- `ui/Tokens.h` defines the semantic schema once; each skin is a table of values (`Skin::midnight`, `Skin::gray3000`)
- Components reference tokens exclusively; skin switch is a live repaint, no component rebuild
- 3000 Gray: chassis `#C9C4BA`, panels `#BFBab0`, silkscreen `#4A473F`, editor surfaces stay dark (`surface` unchanged) — editors are dark in both skins by design

---

## Machine-Readable Tokens

```css
:root {
  /* Chassis & surfaces */
  --chassis: #232326;   --panel-bg: #26262A;
  --surface: #1B1B1F;   --surface-2: #17171A;
  /* Silkscreen & text */
  --silk: #B8B4AA;      --silk-dim: #7A776F;
  --text-primary: #DCD9D2; --text-bright: #E8E5DE;
  --line: #3A3A40;      --line-2: #46464E;
  /* Accents */
  --accent: #E0453A;    --note-fill-hi: #FF6D5A; --note-fill-lo: #E85643;
  --note-edge: #C74537; --ghost: #8F7AE8;
  --seg-amber: #FFB02E; --lane-bar: #C9A44A;
  --led-on: #55FF22;    --warning: #E8C33A;
  /* Roll grid */
  --row-in-scale: #26262E; --row-light: #232329; --row-dark: #1D1D22;
  --beat-line: #3C3C46;    --grid-line: #28282E;
  /* Spacing */
  --space-1: 4px; --space-2: 6px; --space-3: 10px; --space-4: 14px; --space-5: 18px;
  /* Radii */
  --radius-sm: 3px; --radius-md: 6px; --radius-lg: 8px; --radius-xl: 14px;
  /* Motion */
  --dur-hit: 150ms; --dur-dice: 350ms; --dur-slide: 120ms; --dur-commit: 200ms;
}
```

---

## Accessibility

- Contrast: ≥ 4.5:1 for all text tokens against their surfaces, both skins
- Focus indicators: visible 2 px ring on every interactive component; full keyboard traversal of the main window
- Color + shape pairing: ghost notes differ by dash pattern AND color; conditions/badges are textual, never color-only
- Reduced motion: app-level setting disables all decorative animation (zero-duration paths)
- Every parameter: double-click type-in as the pointer-free path; tooltips state concretely what a control will change

---

*This design system evolves with implementation. `ui/Tokens.h` must be updated in the same change as any edit here.*
