# 16Jobs

A step filter sequencer for the **Electrosmith Daisy Pod**, built on libDaisy and DaisySP. A sixteen step pattern drives a resonant lowpass, so the cutoff moves in time with the music instead of wobbling around. The pattern is Euclidean, same as the stutter pedal: turn the density up and the hits spread as evenly as possible, with the downbeat always landing on one.

## Controls

| Control | Function |
|---------|----------|
| Knob 1 | RANGE — how far the filter opens on a hit, up to 8 kHz |
| Knob 2 | DENSITY — how many of the 16 steps are hits. Zero is a closed, steady filter |
| Button 1 | Tap tempo — averages the last 4 taps. Hold 1 s to reset to 120 BPM |
| Button 2 | LATCH — hold to freeze the cutoff wherever it is |
| Encoder turn | Shape: GATE → RAMP UP → RAMP DOWN → TRIANGLE → RANDOM |
| Encoder press | Bypass toggle — both LEDs go dark |
| LED 1 | Flash per step, bright on a hit |
| LED 2 | Shape colour, goes to full brightness while latched |

## Shapes

| Shape | LED 2 | What the filter does between hits |
|-------|-------|-----------------------------------|
| GATE | cyan | Sits closed, snaps open on each hit |
| RAMP UP | green | Rises across the bar, hits go fully open |
| RAMP DOWN | yellow | Falls across the bar |
| TRIANGLE | violet | Up then down over the bar |
| RANDOM | orange | New random contour every bar |

## How it works

- One bar is sixteen steps at sixteenth notes, so a step is a quarter of the tapped beat.
- Each step gets a value `v`: a hit is 1.0, everything else is `kClosedLevel × contour[step]`, and the cutoff is an exponential map between 250 Hz and the range you set.
- The cutoff glides to its target with a one-pole (~3 ms) so the steps snap without clicking.
- Two `Svf` lowpass filters, one per channel, at a fixed resonance.
- Button edges are read from the held state rather than `RisingEdge`, which stays true for a whole debounce window and would fire about twelve times per press at this block size.

## Setup

Requires the [Daisy Toolchain](https://github.com/electro-smith/DaisyWiki/wiki/1.-Setting-Up-Your-Development-Environment)
(arm-none-eabi-gcc, make, dfu-util). On Windows, run `make` from Git Bash.

This project is part of the [Daisy Pod Projects](../README.md) repo. The
libraries are shared by every project and live one level up in `lib/`, which is
gitignored, so fetch them once after cloning:

```bash
mkdir -p ../lib
git clone https://github.com/electro-smith/libDaisy.git ../lib/libDaisy
git clone https://github.com/electro-smith/DaisySP.git ../lib/DaisySP

# Build the libraries once (this takes a few minutes)
make -C ../lib/libDaisy
make -C ../lib/DaisySP
```

If you already have them elsewhere, skip the clone and point the build at them:

```bash
make LIBDAISY_DIR=/path/to/libDaisy DAISYSP_DIR=/path/to/DaisySP
```

## Building

```bash
make -j8
```

Produces `build/16jobs.bin`.

## Flashing

Put the Pod into DFU mode: hold **BOOT**, press **RESET**, release **RESET**,
then release **BOOT**. Then:

```bash
make program-dfu
```

> `dfu-util` prints `Error during download get_status` at the end. This is
> expected — the device has already rebooted out of DFU mode. The flash
> succeeded.

## Tuning

All the "taste" constants are grouped at the top of `main.cpp`:

| Constant | Default | Meaning |
|----------|---------|---------|
| `kPatternSteps` / `kStepsPerBeat` | `16` / `4` | Sixteenth notes, one bar per pattern |
| `kMinCutoffHz` / `kMaxCutoffHz` | `250` / `8000` | Closed cutoff, and the ceiling of Knob 1 |
| `kClosedLevel` | `0.35` | How far a non-hit step opens |
| `kResonance` | `0.6` | Filter resonance |
| `kGlideCoef` | `0.0015` | Cutoff glide, ~3 ms |
| `kDefaultBeatMs` | `500` | Beat length at power up, 120 BPM |

## Project Structure

```
16Jobs/
├── main.cpp      # All pedal logic (control handling + audio callback)
├── Makefile      # Build config, library paths overridable
├── README.md
└── .gitignore    # Excludes build/ and vendored lib/
```
