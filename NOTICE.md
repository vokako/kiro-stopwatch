# NOTICE — third-party material

This workspace builds against upstream libraries that are **not** committed here;
`scripts/fetch-references.sh` clones them at pinned revisions.

| Component | Upstream | Pinned | Licence |
|---|---|---|---|
| M5Unified | github.com/m5stack/M5Unified | 0.2.22 | MIT |
| M5GFX | github.com/m5stack/M5GFX | 0.2.29 | FreeBSD / BSD-2-Clause (LovyanGFX) |
| ArduinoJson | registry `bblanchon/ArduinoJson` (pinned in `linkfw/platformio.ini`) | 7.4.3 | MIT |
| M5StopWatch-UserDemo (optional) | github.com/m5stack/M5StopWatch-UserDemo | V0.5 | MIT |
| m5stack-stopwatch-simulator (optional) | github.com/ochyai/m5stack-stopwatch-simulator | main | MIT |
| M5PM1 / M5IOE1 (optional) | github.com/m5stack | 1.0.6 / 1.0.8 | MIT |

`patches/M5GFX-sdl-stopwatch-466.patch` is a local change to M5GFX (adds the
466×466 SDL window for `board_M5StopWatch`); it is applied to the fetched clone
and is offered back to upstream terms.

Vendor datasheets and schematics referenced in `references/README.md` are
downloaded from M5Stack and Espressif and are not redistributed here.

## Kiro ghost artwork — read before publishing

`linkfw/src/kiro_body.{c,h}` and `buddy/src/kiro_body.{c,h}` are generated
bitmaps of the **Kiro ghost mascot**, rasterised from `kiro-ghost.svg`. That SVG
came from a sibling project in this workspace and carries no licence statement of
its own; the mascot is Kiro/AWS branding, not original artwork of this project.

Consequences:

- Fine for private or internal use, which is what this repository is set up for.
- Before pushing to a public host, decide deliberately: either obtain permission
  for the mark, or replace the asset. `linkfw/tools/gen_kiro_body.py` regenerates
  the bitmap from any SVG of the same shape, so swapping in your own character is
  a one-command change plus new eye coordinates.
- The generator itself, the animation code (`face.cpp`, `ui/face.js`) and
  everything else in this repository are original work.
