# Barberpole

An endless sweeping phaser for the **Electrosmith Daisy Pod**, built on libDaisy and DaisySP. The notches climb forever (or fall forever) instead of bouncing back and forth like a normal phaser.

## Controls

| Control | Function |
|---------|----------|
| Knob 1 | RATE — sweep speed, 0.05 Hz to 6 Hz |
| Knob 2 | RESONANCE — feedback around the allpass chain, up to 0.92 |
| Button 1 | Hold to freeze the sweep where it is |
| Button 2 | Press to clear the chain, useful if the feedback runs away |
| Encoder turn | Direction: UP → DOWN → BOTH |
| Encoder press | Bypass toggle — both LEDs go dark |
| LED 1 | Tracks the sweep position |
| LED 2 | Direction — cyan up, amber down, violet alternating |

## How it works

- Six first order allpass sections in series, `y = c·x + x1 - c·y1`, with feedback around the whole chain. Each section has flat magnitude and moving phase, which is what makes the notches.
- The coefficient comes from a sawtooth sweep: `c = (1 - tan(pi·f/sr)) / (1 + tan(pi·f/sr))`, with `f` sweeping exponentially from 120 Hz to 2.4 kHz.
- The barberpole trick is that the notches are evenly spaced, so when the saw snaps back to the bottom the jump lands where another notch already was. Your ear follows the one climbing and hears an endless rise.
- BOTH flips direction at the end of each sweep, so up and down alternate.
- This DaisySP build has no `FrequencyShift`, so the sweep is an allpass cascade rather than a shifted feedback loop.

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

## Building and flashing

```bash
make -j8
make program-dfu   # with the Pod in DFU mode: hold BOOT, tap RESET
```

> `dfu-util` prints `Error during download get_status` at the end. This is
> expected — the device has already rebooted out of DFU mode.

## Tuning

| Constant | Default | Meaning |
|----------|---------|---------|
| `kNumStages` | `6` | Allpass sections, more is thicker |
| `kSweepMinHz` / `kSweepMaxHz` | `120` / `2400` | Sweep range |
| `kMinRateHz` / `kMaxRateHz` | `0.05` / `6` | Knob 1 range |
| `kMaxFeedback` | `0.92` | Knob 2 ceiling |
| `kMix` | `0.5` | Dry / wet balance |

## Project Structure

```
barberpole/
├── main.cpp
├── Makefile
├── README.md
└── .gitignore
```
