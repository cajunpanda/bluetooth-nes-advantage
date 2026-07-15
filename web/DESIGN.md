# Design

A control panel that looks like the machine it configures: a grey front-loader
NES rendered as UI. One self-contained `index.html` (no build, no external
assets). Committed color strategy: two-tone **molded grey plastic** carries the
surface, a single **NES red** carries every active state and primary action, and
a dark **CRT screen** inset holds the live readouts (the button tester, the
turbo meters, the device log).

## Theme

- Light molded-grey plastic body with dark screen insets. **Committed** strategy
  (the grey plastic surface and the red accent carry the identity), not the
  restrained-neutral product default. Chosen because the brief is diegetic: the
  page should *be* an NES, so the enclosure is the design, not a backdrop behind
  it.
- Scene sentence: a builder at a lit workbench, stick in hand, wants the panel to
  read like the console's own front face, hard-edged injection-molded plastic,
  not a soft web card.
- Not the cream/paper trap: the light surface is a true cool-warm **grey** (not a
  warm near-white), and it is sold as plastic by molded detailing (raised/inset
  edges, panel seams, the red two-tone stripe, sparing screw-heads), never as a
  paper page.

## Color (OKLCH)

**Molded grey plastic** (the console body, the hero surface):

- `--plastic` 0.83 0.008 95 (front panel) · `--plastic-2` 0.74 0.010 95 (lower
  two-tone / recessed wells) · `--plastic-hi` 0.90 0.006 95 (top highlight edge)
  · `--plastic-lo` 0.58 0.012 95 (molded seam / bottom shadow)
- `--bezel` 0.30 0.006 250 (the near-black controller/bezel graphite)

**CRT screen** (dark readout insets, faint green phosphor cast):

- `--screen` 0.19 0.006 150 · `--screen-2` 0.25 0.008 150 · `--screen-line`
  0.33 0.010 150

**NES red** (the one hot accent: primary actions, pressed buttons, current
selection, the two-tone stripe):

- `--red` 0.55 0.19 27 · `--red-2` 0.49 0.19 27 (hover/active) · `--red-ink`
  0.98 0.02 27 (near-white on a red fill) · `--red-soft` (16% red, glow/tints)

**Ink:**

- On plastic: `--ink` 0.30 0.010 95 · `--muted` 0.46 0.010 95 (both clear 4.5:1
  on `--plastic`/`--plastic-2`)
- On screen: `--ink-screen` 0.92 0.02 150 · `--muted-screen` 0.64 0.02 150

**Semantic** (battery, status): `--ok` 0.70 0.16 150 · `--warn` 0.78 0.15 80 ·
`--danger` reuses `--red`. Pressed controller buttons and the turbo meter glow
red (echoing the real red A/B buttons), so red doubles as the "live" color.

Contrast: dark ink on light grey for all body/labels; near-white on red fills and
on the dark screen; the red accent only appears at large-text / UI weights where
it clears 3:1.

## Typography

One family plus a mono, on a **screen-vs-panel** split (product register: no
display font, no pixel font, the retro is carried by color and molding):

- `--font-sans` = `system-ui` stack: headings, control labels, prose, buttons.
- `--font-mono` = `ui-monospace` stack: the wordmark, every numeric readout (%,
  Hz, mV), the device log and console, the P1/P2 chip, equipment micro-labels.
- Fixed rem scale, tight ratio (~1.15), product register. No `clamp()` display
  sizes. Wordmark is a letter-spaced bold sans set in a red stripe (an NES
  title-bar nod), never a pixel typeface.

Micro-labels (`.label-mono`): mono, uppercase, 0.12em tracking, `--muted`. These
are diegetic equipment labeling (`PLAYER`, `TURBO A`, `POWER`), deliberately not
the tracked-uppercase marketing eyebrow, they sit on controls, not above every
section.

## Layout: front-panel console

- Content max-width ~46rem. Breakpoint at **820px**, same skeleton as the sibling
  GameBoy HiFi panel (rail + main, `display: contents` dissolve on mobile).
- **Desktop (>= 820px):** two-pane. A ~15rem left **control deck** (wordmark on
  the red stripe, vertical Test / Configure / System nav, connection pill, pinned
  footer with the player chip + Disconnect + Apply). Beside it a scrolling
  **main** surface led by the button tester.
- **Mobile (< 820px):** the deck dissolves; its top block becomes a sticky top
  bar (wordmark + status pill + underline tabs), its footer a fixed bottom action
  bar.

## Components

- **Button tester (hero):** a dark CRT-screen panel holding a schematic NES
  Advantage: D-pad cross, oval Select/Start, two large red A/B circles, and the
  Turbo A/B dials. Each element lights red the instant its button is pressed
  (`box-shadow` glow + fill), driven by the live INPUT notify. This is the thing
  a builder came to see; it never lies about hardware state and updates even
  under `prefers-reduced-motion`.
- **P1/P2 switch:** a molded slide switch on the tester showing which player the
  Advantage's physical select switch reports, with the player number in mono. It
  reflects hardware, it is not a user toggle.
- **Turbo meters:** two horizontal segmented LED bars (A, B) on the screen, lit
  red proportional to the measured turbo firing rate, with a mono `Hz` readout.
- **Battery gauge:** a molded cell outline that fills green/amber/red by state of
  charge, with `%` and a charge glyph; reads "absent" cleanly when no cell.
- **Molded panels:** light-grey surfaces with a raised top edge + inset bottom
  shadow so they read injection-molded; sparing corner screw-heads (never on
  every panel). Sentence-case titles, no eyebrows.
- **Red slide actions:** the primary action (Connect, Apply) is a red slide
  styled after the NES POWER switch; secondary actions are ghost buttons with a
  hairline plastic border.
- **Selects / sliders / file drop:** native controls restyled to the plastic
  vocabulary; the OTA drop zone is a "cartridge slot" ("insert firmware .bin").
- **Device console:** dark screen, mono green-cast text, a command line that
  streams the same log as the wired UART console.
- **Status pill / player chip:** mono; red when connected, muted when not.

## Motion

- 150-220 ms, state-conveying only. A short power-on reveal plays once on connect;
  section switches crossfade via View Transitions. Button-tester lights are
  immediate (no easing lag on hardware feedback).
- `prefers-reduced-motion`: entrance + view transitions collapse to instant; the
  live tester and meters keep updating (they are state, not decoration).

## Constraints

- Single self-contained `index.html`; inline CSS/JS; no build step, no external
  requests (CSP-safe, offline, forkable). Served over localhost/HTTPS for Web
  Bluetooth's secure-context requirement.
- Web Bluetooth only (Chrome/Edge desktop/Android). All state comes from the BLE
  config server; the GATT contract is duplicated by design in
  `firmware/main/bt_config.cpp` and this page and must stay in lockstep.
</content>
