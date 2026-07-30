# JLCPCB production package, PCB 2.1

Regenerate with `kicad-cli` from `../../bt-nes-advantage-pcb.kicad_pcb`; these are outputs, not
design source. Generated 2026-07-30 against a board that is ERC-clean, DRC-clean, and has zero
schematic parity issues.

| File | Use |
|---|---|
| `bt-nes-advantage-pcb-2.1-gerbers.zip` | upload at step 1 (fabrication). Gerbers + merged Excellon drill |
| `bom.csv` | upload at the assembly step. Columns `Comment, Designator, Footprint, LCSC Part #` |
| `cpl.csv` | upload at the assembly step. Columns `Designator, Mid X, Mid Y, Layer, Rotation` |
| `gerbers/` | the unzipped set, plus `-drl_map.gbr` for reference (deliberately **not** in the zip, it is not a fab layer) |

## Board options

| Setting | Value |
|---|---|
| Layers | 2 |
| Dimensions | 78 x 36 mm |
| Thickness | 1.6 mm |
| Min hole | 0.2 mm (via drill) |
| Min track/clearance | within JLCPCB standard 2-layer capability |
| Assembly | **PCBA, both sides** (Standard, not Economic) |
| Assembly qty | minimum 5 |

Both sides is not optional: D4, D5, D6 and R1, R2, R3 are on the bottom so the status LEDs shine
through the transparent jack plug. Economic assembly is top-side only and cannot build this board.

## Parts

17 BOM lines, 33 placed components. Every line carries an LCSC part number.

- **9 Basic** (no per-part setup fee): C45783, C14663, C8218, C25804, C22843, C22935, C2480, C15127, C8545
- **1 Preferred** (no feeder fee): C16581, the TP4056
- **7 Extended** (~$3 each, about $21): C157741, C125102, C125103 (the three LEDs), C237554 (L1),
  C22885 (R11), C701341 (U1), C1518762 (U3)

R11 is 16.2 kohm 1%, and no Basic part exists at that value. It sets the TPS63900 CFG3 output
voltage, so do not substitute it to save the fee.

Watch stock on L1 (C237554, Murata DFE201610P-2R2M). It ran about 4.4k units, the thinnest margin on
the BOM. U3 (TPS63900) is the next thinnest at about 16k.

## Not assembled by JLCPCB

J1, J2 and J3 are excluded from `bom.csv` and `cpl.csv`, and are hand-populated from local stock.

- **J1** CUI PJ-040-SMT-TR barrel jack. Listed as C3094068 but at 0 stock behind a 1200 piece order
  multiple, so it is not orderable through JLCPCB at all. A second-source jack is worth designing in
  before local stock runs out; what matters is the barrel geometry, 2.5 mm outer and 0.65 mm centre pin.
- **J2** JST S8B-XH-A and **J3** JST S2B-PH-K-S. Both through-hole right-angle.

J4 is a Tag-Connect footprint and needs no part. The two mounting holes are NPTH.

## U1 centroid correction (do not lose this on regeneration)

`cpl.csv` is **not** a raw `kicad-cli pcb export pos` dump. That command writes each footprint's
**origin**, and for the ESP32-WROOM-32E the KiCad origin is the centre of the 18.00 x 25.50 mm body,
while JLCPCB anchors its module model on the **pad field**. The WROOM's PCB antenna takes up about
7 mm of body with no pads under it, so the two differ by **3.620 mm** and JLCPCB's placement preview
renders U1 shifted toward the antenna end.

`cpl.csv` therefore uses the **pad bounding-box centre** for every part:

```
U1  origin (144.400, 77.500)  ->  pad centre (144.400, 81.120)   shift 3.620 mm
```

U1 is the only assembled part affected. Every other placed component has its pads centred on its
origin to within 0.000 mm, so the rule is a no-op for them and the other 32 rows are unchanged.
J1, J2 and J3 share the same asymmetry but are hand-populated and excluded from the CPL.

If you regenerate this file, apply the pad-centre rule again rather than shipping the raw export.

## U1 rotation offset (also do not lose this)

JLCPCB's model for **C701341** sits 90 degrees clockwise from KiCad's footprint orientation, so U1
needs a **+270 degree** correction. JLCPCB's `Rotation` column is counter-clockwise positive, so
+270 CCW is the same as the 90 CW the placement preview asks for.

```
CPL rotation = (KiCad footprint rotation + 270) mod 360      # U1 / C701341 only
U1  0  ->  270
```

The offset belongs to JLCPCB's part model, not to the package, so it is keyed by LCSC part rather
than applied to every module-style footprint. It was confirmed against the live placement preview.
No other part needed one: the rest are exported exactly as KiCad orients them.

## Check before confirming the order

JLCPCB's pick-and-place convention does not always match KiCad's, and their placement preview is the
place to catch it. Rotations are exported as KiCad orients parts, with **no** offsets applied by
hand, because the correct offset depends on the specific LCSC part rather than a general rule.

Step through the preview and confirm orientation on the parts where a 180 degree error is both
plausible and destructive:

| Ref | Package | Why |
|---|---|---|
| D7 | D_SMA | polarity; SMA diodes commonly need +180 |
| Q1, Q2 | SOT-23 | pinout; SOT-23 commonly needs +180 |
| U2 | SOIC-8-1EP | pin 1; SOIC commonly needs +90 or +270 |
| U3 | WSON-10-1EP | pin 1; QFN-style commonly needs +90 |
| D4, D5, D6 | LED 0603 | polarity, and they are on the bottom layer |
| U1 | ESP32-WROOM-32E | pin 1 |

Also confirm the three bottom-side resistors R1 to R3 and the three bottom-side LEDs land on
**Bottom**, since a side error is silent in the BOM but obvious in the preview.
