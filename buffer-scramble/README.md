# Buffer Scramble

A slice re-sequencer for the **Electrosmith Daisy Pod**, built on libDaisy and DaisySP.

Everything you play is captured into a window of eight beat-synced slices. While one window records, the previous one plays back — but its slices come out in a shuffled order. At SCRAMBLE 0 you hear the phrase exactly as you played it, one window late; turn it up and the phrase gets cut up and rearranged, more the higher you go. Hold the trigger and the current window freezes into a loop you can play over.

## Controls

| Control | Function |
|---------|----------|
| Knob 1 | DIV — slice length as a division of the beat: 1, 1/1.5, 1/2, 1/3, 1/4, 1/6, 1/8, 1/12, 1/16, 1/24, 1/32, 1/48 |
| Knob 2 | SCRAMBLE — how many random swaps are applied to the slice order (0 = in order) |
| Button 1 | Tap tempo — averages the last 4 taps, 3s timeout restarts. Hold 1s to reset to 120 BPM |
| Button 2 | Press for a fresh shuffle, hold to freeze the current loop |
| Encoder turn | Mode: SCRAMBLE → REVERSE → PINGPONG → DRIFT |
| Encoder press | Bypass toggle — both LEDs go dark |
| LED 1 | White flash on every slice |
| LED 2 | Mode colour, pulses when a new window is picked up |

## Modes

| Mode | LED 2 | What it does |
|------|-------|--------------|
| SCRAMBLE | cyan | Plays the shuffled order forward |
| REVERSE | green | Reads the shuffled order backwards |
| PINGPONG | orange | Alternates direction every window |
| DRIFT | violet | Rotates the order one slot later every window, so the phrase slowly turns |

## How it works

- One circular buffer per channel in external SDRAM. The read head sits exactly one window behind the write head, so it is continuous rather than a stop-and-start sampler, and a tempo change re-slices the audio on the next sample instead of waiting for a new capture.
- One window is eight slices, so the latency you hear is eight slices — at 1/8 notes and 120 BPM that is 0.5 s, which lands like a slapback.
- The order starts as 0–7 and takes `SCRAMBLE × 12` random transpositions. It is rebuilt every time a window is picked up, so each pass is a slightly different arrangement.
- Slice edges get a ~1 ms fade, so jumping between non-adjacent slices doesn't click.
- Holding button 2 stops the write head, which parks the window in place and loops it — that is the freeze.

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

Produces `build/buffer-scramble.bin`.

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
| `kWindowSlices` | `8` | Slices per window. Also the latency you hear |
| `kMaxSliceSamples` | `24576` | Longest slice, ~0.51 s at 48 kHz |
| `kFadeSamples` | `48` | Slice edge fade, ~1 ms |
| `kMaxSwaps` | `12` | Random transpositions at full SCRAMBLE |
| `kDivisions[]` | `1 … 48` | The 12 grid steps Knob 1 selects between |
| `kDefaultBeatMs` | `500` | Beat length at power up, 120 BPM |
| `kMinBeatMs` / `kMaxBeatMs` | `200` / `1000` | Tap tempo range |

## Project Structure

```
buffer-scramble/
├── main.cpp      # All pedal logic (control handling + audio callback)
├── Makefile      # Build config, library paths overridable
├── README.md
└── .gitignore    # Excludes build/ and vendored lib/
```
