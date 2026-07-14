# Product

## Register

product

## Users

Owners of the **Bluetooth NES Advantage** mod, a hardware+firmware kit that turns
the classic NES Advantage arcade stick into a wireless controller for the Switch,
Switch 2, and BLE hosts, plus the maker who builds and sells the kit. Mostly
retro-gaming hobbyists. They open this page at a **desk or bench**, on a desktop
or laptop, with the stick in front of them, usually to do one focused thing:
change how it behaves or confirm the hardware works.

Primary jobs on any visit:

1. **Test the stick.** Put it in config mode, watch every button, the D-pad, the
   P1/P2 switch, and the turbo dials light up live so a buyer or builder can
   confirm the hardware end to end without a console.
2. **Configure it.** Pick the transport (Classic Switch Pro / BLE), the button
   profile, and the directional mode; forget a paired host; reboot.
3. **Update firmware** over the air, and read the live device log.

## Product Purpose

The single Web Bluetooth control panel for the mod's BLE config server
(`firmware/main/bt_config.cpp`). It is the main interface a buyer touches after
the solder work is done, so it doubles as the product's face. Success = a builder
connects on the first try, immediately sees the controller respond button for
button, and configures it without a manual.

## Brand Personality

An 8-bit console you climb inside. Three words: **molded, tactile, diegetic.** It
should feel like the inside of a grey front-loader NES, the two-tone plastic, the
red racing stripe, the chunky POWER/RESET slides, the cartridge slot, honest
mono readouts on a dark screen. The heritage is worn as **industrial design**,
not decoration: the console *is* the chrome.

## Anti-references

- Kitsch retro: pixel/8-bit display fonts, chiptune skeuomorphism, scanline
  filters slapped on everything, `Press Start` cliches.
- Generic dark-neon "gamer" RGB dashboards.
- SaaS-cream / warm-paper backgrounds, glassmorphism, gradient-and-glow decor.
- Marketing scaffolding: tracked-uppercase eyebrows, hero-metric templates, 01 /
  02 / 03 section numbers, over-glossy buttons.
- Nintendo trade dress or logos. The look is *evocative of* an era's industrial
  design, never a counterfeit of a brand.

## Design Principles

1. **The controller is the hero.** The live button tester is the thing a builder
   came to see; it reads at a glance and never lies about hardware state.
2. **Read like the console's own panel.** Molded grey surfaces, a single red
   accent, hard edges, mono readouts on a dark "screen." No decoration that
   isn't signal.
3. **Follow the hardware.** The P1/P2 indicator, turbo meter, and battery all
   mirror what the stick physically reports; nothing is invented UI state.
4. **Robust over pretty when they conflict.** Web Bluetooth on Linux is flaky, so
   the connection self-heals (retry + auto-reconnect) rather than looking nice
   and failing.
5. **One self-contained file.** No build, no external requests; served as-is over
   localhost/HTTPS and stays forkable.

## Accessibility & Inclusion

- Light molded-grey theme (used bench-lit at a desk). Contrast verified: body and
  labels hit >= 4.5:1 on grey; the red accent is used at weights that clear 3:1
  for large text / UI, near-white text on red fills.
- Full `:focus-visible` on every control; native form controls (select, range,
  file, text) for assistive-tech support.
- `prefers-reduced-motion` honored: the button-tester still updates (it's state,
  not decoration), but entrance and view-transition motion collapse to instant.
- Not color-only: pressed buttons carry a filled/labeled state, the P1/P2 switch
  shows the player number as text, connection state is labeled, turbo shows a
  numeric Hz readout beside the meter.
</content>
</invoke>
