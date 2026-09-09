# Stochastic Stutter

A probability-driven glitch pedal for the **Electrosmith Daisy Pod**, built on libDaisy and DaisySP.

Audio is captured continuously into a ~1 s buffer while the slice clock runs against a 16-step Euclidean rhythm. The CHAOS knob sets how many of those 16 steps fire, and they are spread as evenly as possible, so it grooves instead of spraying random chops: 4/16 is a steady pattern, 13/16 is a dense rolling stutter, and the downbeat never moves. Slices are always a division of the beat (120 BPM until you tap), so everything lands on the grid.

## Controls

| Control | Function |
|---------|----------|
| Knob 1 | DIV — slice length as a division of the beat: 1, 1/1.5, 1/2, 1/3, 1/4, 1/6, 1/8, 1/12, 1/16, 1/24, 1/32, 1/48 |
| Knob 2 | CHAOS — how many of the 16 steps fire (0 = clean, full = every slice) |
| Button 1 | Tap tempo — averages the last 4 taps, 3s timeout restarts. Hold 1s to reset to 120 BPM |
| Button 2 | Trigger — press for one burst, hold for continuous stutter |
| Encoder turn | Mode: CHOP → STUTTER → PITCH → REVERSE |
| Encoder press | Bypass toggle — the only off switch, both LEDs go dark |
| LED 1 | White flash on every slice, bright on pattern hits |
| LED 2 | Mode colour, brightness follows the wet crossfade (orange / cyan / violet / green) |

## Modes

| Mode | What it does |
|------|--------------|
| CHOP | Euclidean gate — mutes the audio on the pattern hits |
| STUTTER | Repeats the captured slice 2–4 times at the original pitch |
| PITCH | Same, but each repeat walks up a whole tone, stopping an octave up |
| REVERSE | Repeats the slice backwards, occasionally flipping forward as CHAOS rises |

Holding button 2 always stutters, in any mode, so you can play the glitch by hand and leave CHAOS low.

## How it works

- Two buffers per channel in external SDRAM: `rec_*` is written continuously, `frz_*` is a frozen copy of the slice.
- The freeze happens at the moment a sequence starts, so the repeat is of the slice that just ended and keeps running even while the live audio overwrites `rec_*`.
- Repeats are one per slice: the read pointer loops within the frozen slice until the next boundary, then moves to the next repeat and possibly a new rate. That keeps the rhythm on the grid even at 2x.
- Dry and wet are crossfaded with a ~1 ms one-pole, so slice edges don't click.
- The wet path runs through a 6 kHz one-pole and a -1 dB trim, so repeats sit behind the dry signal instead of stabbing through it.
- The right channel reads the frozen slice a few ms later than the left, which widens the repeats without a second delay line.
- The pattern is only rebuilt when CHAOS actually changes, so the groove stays put while the knob is untouched.

## Setup

Requires the [Daisy Toolchain](https://github.com/electro-smith/DaisyWiki/wiki/1.-Setting-Up-Your-Development-Environment) (arm-none-eabi-gcc, make, dfu-util). On Windows, run `make` from Git Bash.

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

If you already have them elsewhere, skip the clone and point the build at them instead:

```bash
make LIBDAISY_DIR=/path/to/libDaisy DAISYSP_DIR=/path/to/DaisySP
```

## Building

```bash
make -j8
```

Produces `build/stochastic-stutter.bin`.

## Flashing

Put the Pod into DFU mode: hold **BOOT**, press **RESET**, release **RESET**, then release **BOOT**. Then:

```bash
make program-dfu
```

> `dfu-util` prints `Error during download get_status` at the end. This is expected — the device has already rebooted out of DFU mode. The flash succeeded.

## Tuning

All the "taste" constants are grouped at the top of `main.cpp`:

| Constant | Default | Meaning |
|----------|---------|---------|
| `kBufferSamples` | `49152` | Record/freeze buffer per channel, ~1.02 s at 48 kHz. Sets the max slice length |
| `kDivisions[]` | `1 … 48` | The 12 grid steps Knob 1 selects between |
| `kPatternSteps` | `16` | Length of the Euclidean trigger pattern |
| `kPitchStepSemis` | `2` | Semitones the PITCH mode climbs per repeat |
| `kWetToneHz` / `kWetGain` | `6000` / `0.9` | Repeats are darker and slightly under the dry level |
| `kFadeCoef` | `0.02` | Dry/wet crossfade rate, ~1 ms |
| `kWetSpreadMs` | `7` | L/R offset on repeats, for stereo width |
| `kDefaultBeatMs` | `500` | Beat length at power up, 120 BPM |
| `kMinBeatMs` / `kMaxBeatMs` | `200` / `1000` | Tap tempo range |

## Project Structure

```
stochastic-stutter/
├── main.cpp      # All pedal logic (control handling + audio callback)
├── Makefile      # Build config, library paths overridable
├── README.md
└── .gitignore    # Excludes build/ and vendored lib/
```
