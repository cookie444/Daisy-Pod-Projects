# OddWay

A vowel filter for the **Electrosmith Daisy Pod**, built on libDaisy and DaisySP. Three resonant bandpasses sit where your vocal tract would, so a guitar starts to say "aah", "eee", "ooo". Sweep it with the tap tempo or step it with a button and it talks.

## Controls

| Control | Function |
|---------|----------|
| Knob 1 | VOWEL — position along A → E → I → O → U, interpolated |
| Knob 2 | MIX — dry through to pure formants |
| Button 1 | Tap tempo sets the sweep rate (one sweep per bar). Hold 1 s to stop the sweep |
| Button 2 | Press to step to the next vowel |
| Encoder turn | Voice size — transposes the whole stack: -12, -5, 0, +5, +12 semitones |
| Encoder press | Bypass toggle — both LEDs go dark |
| LED 1 | Output level |
| LED 2 | Vowel colour — red A, orange E, yellow I, green O, blue U |

## How it works

- Three `Svf` bandpasses per channel at the F1, F2 and F3 centres of the current vowel, summed 1.0 / 0.6 / 0.25 so the low formant dominates like a real voice.
- The vowel knob is an interpolation, so you get every point between A and U rather than five fixed settings.
- Voice size multiplies all three centres by `2^(semitones/12)`, which is what turns the same vowel into a child, an adult or a giant.
- Sweeping runs the vowel position linearly across the range, one pass per four beats.
- `RisingEdge` stays true for a whole debounce window, and the callback runs about twelve times per millisecond, so edges are read from the held state instead — one press is one action.

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
| `kVowels[][]` | A E I O U | F1, F2, F3 centres in Hz |
| `kFormantQ` | `0.82` | `Svf` resonance |
| `kF1Gain` / `kF2Gain` / `kF3Gain` | `1.0` / `0.6` / `0.25` | Formant balance |
| `kSweepBeats` | `4` | Beats per full vowel sweep |
| `kSizes[]` | `-12 … 12` | Voice size steps |

## Project Structure

```
OddWay/
├── main.cpp
├── Makefile
├── README.md
└── .gitignore
```
