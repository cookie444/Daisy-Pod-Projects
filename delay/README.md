# Daisy Pedal Pod Delay

An analog-style delay pedal for the **Electrosmith Daisy Pod**, built on libDaisy and DaisySP.

Warm repeats that darken progressively as they decay (filter sits in the feedback path, like a real BBD delay), with tap tempo, manual time control, and an optional tape-wobble modulation.

## Controls

| Control | Function |
|---------|----------|
| Knob 1 | Feedback (number of repeats) |
| Knob 2 | Mix (dry / wet blend) |
| Button 1 | Bypass toggle — green LED on means the effect is active |
| Button 2 | Tap tempo — averages the last 4 taps, 3s timeout restarts |
| Encoder turn | Manual delay time, ±5 ms per detent |
| Encoder press | Toggle tape wobble modulation |
| LED 2 | Blinks once per repeat — red = modulation off, blue = modulation on |

Tap tempo and the encoder write to the same value, so you can tap in a rough tempo and then fine-tune it with the encoder.

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
make -j4
```

Produces `build/daisy-audio.bin`.

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
| `kFilterCutoffHz` | `4500` | Feedback lowpass cutoff. Lower = darker repeats |
| `kModRateHz` | `1.1` | Tape wobble LFO rate |
| `kModDepthMs` | `1.5` | Tape wobble depth in ms |
| `kEncoderStepMs` | `5.0` | Delay time change per encoder detent |
| `kMinDelayMs` / `kMaxDelayMs` | `20` / `2000` | Delay time range |
| `kMaxFeedback` | `0.92` | Feedback ceiling, kept just below self-oscillation |

## Project Structure

```
daisy-audio/
├── main.cpp      # All pedal logic (control handling + audio callback)
├── Makefile      # Build config, library paths overridable
├── README.md
└── .gitignore    # Excludes build/ and vendored lib/
```
