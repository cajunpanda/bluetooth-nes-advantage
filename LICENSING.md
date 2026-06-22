# Licensing

This is an open hardware and firmware project, so different parts are released
under licenses appropriate to their medium. Each directory below carries its own
`LICENSE` file with the full text; this page is the map.

| Path | Contents | License |
|---|---|---|
| `firmware/` | ESP-IDF firmware | **MIT** ([`LICENSE`](LICENSE)) |
| `web/` | BLE config and OTA web app | **MIT** ([`LICENSE`](LICENSE)) |
| `tools/` | Host-side Python utilities | **MIT** ([`LICENSE`](LICENSE)) |
| `hardware/` | KiCad PCB project, schematic, layout, footprints, 3D models | **CERN-OHL-P-2.0** ([`hardware/LICENSE`](hardware/LICENSE)) |
| `docs/` | Design docs, schematics, protocol notes | **CC-BY-4.0** ([`docs/LICENSE`](docs/LICENSE)) |

SPDX: `MIT`, `CERN-OHL-P-2.0`, `CC-BY-4.0`. Source files carry an
`SPDX-License-Identifier` header so the per-file license is machine-readable.

## Third-party material

- `firmware/components/cjson/` is cJSON by Dave Gamble and contributors,
  **MIT** licensed. It is vendored unmodified and retains its own terms in the
  file headers.

## Why the split

Software licenses are written around copyright over source code and do not map
cleanly onto hardware design files. CERN-OHL-P-2.0 is purpose-built for hardware
and is the permissive member of the CERN-OHL family. CC-BY-4.0 is the
conventional choice for documentation.
