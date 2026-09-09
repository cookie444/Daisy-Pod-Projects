# Specter Resonator

A resonant bank for the **Electrosmith Daisy Pod**, built on libDaisy and DaisySP. Your playing excites sixteen tuned resonators instead of going straight to the output, so anything you play comes back as a chord built from the selected scale.

Builds live under `experimental-builds/`, each in its own folder with its own Makefile:

| Build | Notes |
|-------|-------|
| [experimental-builds/1dot1](experimental-builds/1dot1/) | First working revision. 16 partials over three octaves, wet-path compressor, freeze swell, 12 scale presets |

Build numbering is `<major>dot<minor>` — `1dot1` is build 1.1. Flash-ready
binaries land in that folder's `build/` directory after `make`.

## Controls (build 1.1)

| Control | Function |
|---------|----------|
| Knob 1 | DAMPING — decay time of the bank, 0.03 s to 1.0 s |
| Knob 2 | BLEND — dry signal through to pure resonator |
| Button 1 | FREEZE — hold to gate the excitation and hold the ring |
| Button 2 | STRIKE — press for a noise burst, hold to bow with noise |
| Encoder turn | Scale preset (12 of them) |
| Encoder press | Bypass toggle — both LEDs go dark |
| LED 1 | White, brightness follows how hard the bank is ringing |
| LED 2 | Scale colour, full brightness while frozen |

## Building

From inside a build folder:

```bash
cd experimental-builds/1dot1
make -j8
make program-dfu   # with the Pod in DFU mode
```

The libDaisy and DaisySP libraries are shared by the whole repo and live two
levels up in `lib/`. See the [root README](../README.md) to fetch them.
