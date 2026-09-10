# Specter Resonator — build 1.2 (experimental)

A resonant bank for the **Electrosmith Daisy Pod**, built on libDaisy and DaisySP.

Your playing excites 24 tuned resonators instead of going straight to the output, so anything you play comes back as a chord built from the selected scale. Hold the freeze button and the current ring is held indefinitely while your live signal drops away — a drone you can then strike, bow, or play over.

## Controls

| Control | Function |
|---------|----------|
| Knob 1 | DAMPING — decay time of the bank, 0.03 s to 1.0 s |
| Knob 2 | BLEND — dry signal through to pure resonator |
| Button 1 | Tap to step the key (12 of them, A through G#). Hold past ~150 ms to FREEZE: gates the excitation and holds the ring. LED 1 blips on a key change |
| Button 2 | STRIKE — press for a noise burst into the bank, hold to bow it with noise |
| Encoder turn | Scale preset (12 of them) |
| Encoder press | Bypass toggle — both LEDs go dark |
| LED 1 | White, brightness follows how hard the bank is ringing |
| LED 2 | Scale colour, goes to full brightness while frozen |

## Key and scale

Two separate things, and both are on the pod:

- **Encoder turn** picks the tuning (the chord the bank is voiced in).
- **Button 1 tap** steps the key — 12 of them, A through G#, in semitones. LED 1 blips when it changes. The scale shape stays put and the whole bank transposes.

So you can keep, say, minor pentatonic and walk it through whatever key the song is in. If you are playing with sharps that don't ring, either step the key to match, or use the chromatic tuning.

## Scale presets

Turning the encoder walks the bank through these tunings:

| LED 2 | Scale |
|-------|-------|
| cyan | Major pentatonic |
| green | Minor pentatonic |
| yellow | Ionian (major) |
| blue | Aeolian (minor) |
| orange | Dorian |
| violet | Phrygian |
| pink | Lydian |
| teal | Mixolydian |
| red | Locrian |
| white | Whole tone |
| magenta | Harmonic minor |
| lime | Chromatic — one partial per semitone over two octaves, so every note including sharps lands on something |

## How it works

- Each partial is a two-pole resonator, `y = g·x + 2r·cos(w)·y[n-1] - r²·y[n-2]`, with its radius `r` set from the damping time and its gain `g = (1-r²)·sin(w)` so every resonance peaks at unity.
- Upper partials get a slightly shorter decay (`t60 · (f0/f)^0.35`) so the chord settles the way a real body does.
- A narrow resonance only catches a sliver of a broadband signal, so the bank runs into makeup gain that grows with `sqrt(T60)`. Without it the wet side is roughly 40 dB down. It is driven from the knob, not the freeze time, so freezing doesn't jump the level.
- The partials are spread over a range that follows the density of the tuning — `kNumModes * 12 / notes_per_octave` semitones, capped at `kMaxSpanSemis`. A 12-note tuning ends up one partial per semitone, a 5-note pentatonic reaches much further up. Everything is snapped to the nearest scale degree, so every string finds something to excite rather than only the low ones. Each partial is detuned a few cents so exact harmonic alignments don't become hot spots.
- Key is just a semitone offset added to every partial, so changing it transposes the whole bank without reshaping it.
- The wet path runs through a compressor: loud rings are held near `kAgcTarget`, quiet ones are lifted by `kAgcGain`, which is what keeps one note from jumping out of the mix.
- The partials are spread across the stereo field, low on the left, and the sum runs through a tanh soft clip.
- Coefficients are only recalculated when damping, freeze or scale changes, so the audio loop stays cheap.

## Setup

Requires the [Daisy Toolchain](https://github.com/electro-smith/DaisyWiki/wiki/1.-Setting-Up-Your-Development-Environment)
(arm-none-eabi-gcc, make, dfu-util). On Windows, run `make` from Git Bash.

This project is part of the [Daisy Pod Projects](../../../README.md) repo. The
libraries are shared by every project and live three levels up in `lib/`, which is
gitignored, so fetch them once after cloning:

```bash
mkdir -p ../../../lib
git clone https://github.com/electro-smith/libDaisy.git ../../../lib/libDaisy
git clone https://github.com/electro-smith/DaisySP.git ../../../lib/DaisySP

# Build the libraries once (this takes a few minutes)
make -C ../../../lib/libDaisy
make -C ../../../lib/DaisySP
```

If you already have them elsewhere, skip the clone and point the build at them:

```bash
make LIBDAISY_DIR=/path/to/libDaisy DAISYSP_DIR=/path/to/DaisySP
```

## Building

```bash
make -j8
```

Produces `build/specter-resonator-1dot2.bin`.

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
| `kNumModes` | `24` | Resonators in the bank |
| `kBaseFreqHz` | `110` | A2, the lowest partial |
| `kMinT60` / `kMaxT60` | `0.03` / `1.0` | Knob 1 range, in seconds |
| `kFreezeT60` | `60` | Decay while button 1 is held |
| `kMakeupBase` / `kMaxMakeup` | `24` / `80` | Broadband makeup gain, and its ceiling |
| `kRefT60` | `0.3` | Decay at which makeup sits at `kMakeupBase` |
| `kStrikeDecay` | `0.0005` | Noise burst length, ~40 ms |
| `kStrikeLevel` | `0.015` | Strike loudness after the `1/(2(1-r))` boost |
| `kBowLevel` | `0.003` | Noise drive while button 2 is held |
| `kFreezeGain` | `3.0` | Lift on a frozen ring, swelled in over ~100 ms |
| `kMaxSpanSemis` | `42` | Ceiling on the range the modes are spread across |

## Project Structure

```
1dot2/
├── main.cpp      # All pedal logic (control handling + audio callback)
├── Makefile      # Build config, library paths overridable
├── README.md
└── .gitignore    # Excludes build/ and vendored lib/
```
