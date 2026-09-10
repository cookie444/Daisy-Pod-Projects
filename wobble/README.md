# Wobble

Tape style modulation for the **Electrosmith Daisy Pod**, built on libDaisy and DaisySP. A short modulated delay gives you wow, flutter and drift, with the two channels offset so it spreads across the stereo field.

## Controls

| Control | Function |
|---------|----------|
| Knob 1 | DEPTH — up to 6 ms of wobble |
| Knob 2 | RATE — 0.1 Hz (wow) to 8 Hz (flutter) |
| Button 1 | Tap tempo syncs the wobble to one cycle per beat. Hold 1 s to go back to the knob |
| Button 2 | Hold to brake the wobble to a stop |
| Encoder turn | Waveform: SINE → TRIANGLE → RANDOM → S&H |
| Encoder press | Bypass toggle — both LEDs go dark |
| LED 1 | Brightness follows the modulation |
| LED 2 | Waveform colour |

## Waveforms

| Waveform | LED 2 | Character |
|----------|-------|-----------|
| SINE | cyan | Smooth wow |
| TRIANGLE | green | Symmetrical flutter |
| RANDOM | orange | Tape drift, glides between random levels |
| S&H | pink | Stepped random, the damage tape actually does |

## How it works

- One modulated delay per channel around a 10 ms centre, read with Hermite interpolation so the pitch slides instead of stepping.
- The two channels run a half cycle apart, plus a 2.5 ms static offset, which is where the width comes from.
- RANDOM and S&H replace the oscillator with a random walk: a new target each cycle, either glided towards or jumped to.
- Braking ramps the rate multiplier to zero, so the wobble coasts to a stop rather than cutting out.
- Tapping sets the rate to one cycle per beat; holding button 1 hands it back to the knob.

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
| `kBaseDelayMs` | `10` | Centre of the modulation |
| `kMaxDepthMs` | `6` | Knob 1 at full |
| `kStereoOffsetMs` | `2.5` | Static spread between channels |
| `kMinRateHz` / `kMaxRateHz` | `0.1` / `8` | Knob 2 range |
| `kMix` | `0.5` | Dry / wet balance |
| `kBrakeCoef` | `0.0005` | How fast the brake brings it to a stop |

## Project Structure

```
wobble/
├── main.cpp
├── Makefile
├── README.md
└── .gitignore
```
