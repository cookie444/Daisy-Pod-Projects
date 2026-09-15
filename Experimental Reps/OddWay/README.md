# OddWay

A beating vowel filter for the **Electrosmith Daisy Pod**, built on libDaisy and DaisySP. Three resonant bandpasses sit where your vocal tract would, so a guitar says "aah", "eee", "ooo". A tap-tempo beat clock re-pronounces the vowel on every beat, so you can turn the encoder to a new vowel and hear it land in time.

## Controls

| Control | Function |
|---------|----------|
| Knob 1 | ARTICULATION — how hard each beat pronounces the vowel. Zero is a steady filter |
| Knob 2 | MIX — dry through to pure formants |
| Button 1 | Tap tempo, one beat per tap. Hold 1 s to reset to 120 BPM |
| Button 2 | Press to pronounce now, so you can talk with your hand |
| Encoder turn | Live vowel — A → E → I → O → U, snaps and pronounces |
| Encoder press | Bypass toggle — both LEDs go dark |
| LED 1 | Pulses with the beat, like 16Jobs |
| LED 2 | Vowel colour — red A, orange E, yellow I, green O, blue U |

## How it works

- Three `Svf` bandpasses per channel at the F1, F2 and F3 centres of the vowel you've picked, summed 1.0 / 0.6 / 0.25 so the low formant dominates like a real voice.
- The beat clock is one beat per tap, exactly like 16Jobs: LED 1 pulses in time with your physical taps.
- Each beat fires a pronounce envelope — a ~120 ms blip that lifts the wet level by up to `1 + articulation × 1.2` — so the vowel is articulated again rather than just sitting there.
- Turning the encoder snaps to the next vowel and pronounces it immediately, which is how you talk in rhythm.
- `RisingEdge` stays true for a whole debounce window, and the callback runs about twelve times per millisecond, so edges are read from the held state instead — one press is one action.

## Setup

Requires the [Daisy Toolchain](https://github.com/electro-smith/DaisyWiki/wiki/1.-Setting-Up-Your-Development-Environment)
(arm-none-eabi-gcc, make, dfu-util). On Windows, run `make` from Git Bash.

This project is part of the [Daisy Pod Projects](../../README.md) repo. The
libraries are shared by every project and live two levels up in `lib/`, which is
gitignored, so fetch them once after cloning:

```bash
mkdir -p ../../lib
git clone https://github.com/electro-smith/libDaisy.git ../../lib/libDaisy
git clone https://github.com/electro-smith/DaisySP.git ../../lib/DaisySP

# Build the libraries once (this takes a few minutes)
make -C ../../lib/libDaisy
make -C ../../lib/DaisySP
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
| `kVowels[][]` | A E I O U | F1, F2, F3 centres in Hz |
| `kFormantQ` | `0.82` | `Svf` resonance |
| `kF1Gain` / `kF2Gain` / `kF3Gain` | `1.0` / `0.6` / `0.25` | Formant balance |
| `kPronounceMs` | `120` | Length of the per-beat articulation blip |

## Project Structure

```
OddWay/
├── main.cpp
├── Makefile
├── README.md
└── .gitignore
```
