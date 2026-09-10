# Frugal Looper

A stereo looper for the **Electrosmith Daisy Pod**, built on libDaisy and DaisySP. Sixty seconds of stereo audio in external SDRAM, with sound-on-sound overdubbing, stop and restart, erase, and half speed / reverse / double speed playback.

## Controls

| Control | Function |
|---------|----------|
| Knob 1 | Loop level — how loud the loop sits under your dry signal |
| Knob 2 | Overdub feedback — how much the previous layers fade each pass |
| Button 1 | Transport: record → play → overdub → play |
| Button 2 | Stop. Press again to clear back to empty. Hold 2 s to erase |
| Encoder turn | Speed and direction: normal, half speed, reverse, double speed |
| Encoder press | Bypass the loop, dry signal only |
| LED 1 | Transport state — red recording, green playing, blue overdubbing, dim stopped, dark empty |
| LED 2 | Pulses at the top of the loop — cyan forwards, amber in reverse |

## How it works

- Two channels of loop audio live in SDRAM. The first record pass sets the loop length; every later pass runs inside it.
- Your dry signal always passes through, so you play over the loop rather than instead of it.
- Overdubbing is sound-on-sound: `buffer = buffer · feedback + input`, so turning Knob 2 down makes old layers decay away as you add new ones.
- Recording only happens at normal speed. Half, double and reverse are playback-only, so changing speed mid-loop never writes over anything.
- The read head is fractional with linear interpolation, which is what makes half speed and reverse smooth rather than steppy.

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

Produces `build/frugal-looper.bin`.

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
| `kMaxLoopSeconds` | `60` | Longest loop. Uses ~22.5 MB of SDRAM as stereo floats |
| `kMaxLevel` | `1.2` | Loop level at full Knob 1 |
| `kMaxFeedback` | `0.98` | Overdub feedback ceiling, just below runaway |
| `kEraseHoldMs` | `2000` | Button 2 hold time to erase |
| `kRates[]` | `1, 0.5, -1, 2` | The four encoder positions |

## Project Structure

```
frugal-looper/
├── main.cpp      # All pedal logic (control handling + audio callback)
├── Makefile      # Build config, library paths overridable
├── README.md
└── .gitignore    # Excludes build/ and vendored lib/
```
