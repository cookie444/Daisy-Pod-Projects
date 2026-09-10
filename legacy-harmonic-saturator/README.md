# Legacy Harmonic Saturator

A dual-engine guitar drive for the **Electrosmith Daisy Pod**, built on libDaisy
and DaisySP.

Two independent drive models (A and B) that you can run in series in either
order or in parallel, with a cabinet emulation on the output. Built to sound
good with a guitar plugged straight in: input trim for instrument level, a gate,
per-model EQ, and aliasing-free clipping.

## Controls

| Control | Function |
|---------|----------|
| Knob 1 | DRIVE — pre-gain into both engines |
| Knob 2 | LEVEL — output, unity at about noon |
| Encoder turn | Model A |
| Hold encoder + turn | Model B (LED 1 switches to B's colour while held) |
| Encoder tap | Bypass |
| Button 1 tap | Routing: A→B → B→A → parallel sum → parallel split |
| Button 1 hold | Cabinet sim on / off |
| Button 2 tap | Gate: off → low → high |
| Button 2 hold | Pickup load compensation on / off |
| LED 1 | Model colour, brightness follows level, shifts red when clipping |
| LED 2 | Routing colour, flashes white on any change |

Defaults are A = SOFT, B = BOOST, routing = B→A, i.e. a clean boost into an
overdrive, which is the classic pairing. Flip to A→B (button 1) for drive into
boost, or to a parallel route for two models side by side.

## Models

| # | LED 1 | Model | Character |
|---|-------|-------|-----------|
| 0 | Ice blue | BOOST | Clean preamp, no clipping. Stack it or use it as a platform |
| 1 | Amber | SOFT | Transparent overdrive, small mid push |
| 2 | Orange | CRUNCH | Op-amp style hard clip, tight and biting |
| 3 | Magenta | FUZZ | Scooped, dark, cavernous, huge sustain |
| 4 | Green | HIGH GAIN | Asymmetric, tight low end, strong presence |

## Signal path

```
in → DC cut → load comp → gate ─┬─ A → B ─┐
                                ├─ B → A ─┼→ cab sim → LEVEL → limiter → out
                                ├─ A + B ─┤
                                └─ A|B split @ 220 Hz ─┘
```

All four routings run at once and are crossfaded over ~14 ms, so switching
order or jumping into parallel never clicks. Only the active one(s) cost CPU.

Each engine is: pre-highpass → pre-peak → gain → clipper → clipper → DC cut →
post-lowpass → post-peak → makeup.

## Why it sounds the way it does

- **ADAA clipping.** Every waveshaper is paired with its antiderivative and
  integrated across each sample step instead of evaluated at the sample. Measured
  against naive clipping at 48 kHz, this removes **32–62 dB** of inharmonic
  aliasing on the clipping models — that is the difference between "fizz" and
  "grit". No oversampler needed, so it stays cheap.
- **Cabinet emulation.** Six biquads approximating a closed-back 1x12 (80 Hz
  highpass, 110 Hz thump, 500 Hz cone dip, 2.6 kHz presence, steep top roll
  off). This is the single biggest reason a driven guitar sounds like a record
  instead of a fuzz pedal into a desk. Hold button 1 to bypass it if you are
  going into a real cab or your own IR loader.
- **Load compensation.** The Pod's input is line level, not the 1 MΩ an amp
  presents, so a guitar plugged straight in loses treble. A +4.5 dB shelf at
  2.2 kHz puts some of it back. Hold button 2 to turn it off if you have a
  buffer pedal in front.
- **Headroom.** Inter-stage gain only opens up as DRIVE comes up, so no model is
  a surprise clean boost at zero. A soft limiter catches anything past 0.7.

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

If you already have them elsewhere, point the build at them:

```bash
make LIBDAISY_DIR=/path/to/libDaisy DAISYSP_DIR=/path/to/DaisySP
```

> On Windows, pass those paths with forward slashes and avoid parentheses in
> them — GNU Make eats backslashes and chokes on `(` `)`.

## Building

```bash
make -j8
```

Produces `build/legacy-harmonic-saturator.bin`.

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

Constants are grouped at the top of `main.cpp`:

| Constant | Default | Meaning |
|----------|---------|---------|
| `kLevelMax` | `3.0` | Output gain with LEVEL at full. Unity lands near noon |
| `kDriveCurve` | `2.0` | DRIVE knob taper. Higher = more of the range is clean |
| `kCompExp` | `0.15` | How hard output fights the DRIVE knob. Raising it past ~0.4 makes more drive quieter |
| `kSeriesIn` | `0.5` | Trim into the second engine of a series pair |
| `kParGain` / `kSplitGain` | `0.55` / `0.85` | Parallel sum and crossover trims |
| `kSplitHz` | `220` | Parallel crossover frequency |
| `kRouteFade` | `0.0015` | Routing crossfade, ~14 ms |
| `kGateThresh[]` | `0` / `0.0012` / `0.004` | Gate thresholds on the input |

Models themselves are the `kModels[]` table: pre-highpass, pre-peak, two
clippers with an inter-stage gain, post-lowpass, post-peak, max gain in dB, and
makeup.

## Project structure

```
legacy-harmonic-saturator/
├── main.cpp      # All pedal logic (control handling + audio callback)
├── Makefile      # Build config, library paths overridable
├── README.md
└── .gitignore    # Excludes build/ and vendored lib/
```

## Next steps

- Tuner mode on a long press of both buttons
- Real IR convolution instead of the biquad cabinet (SDRAM is mostly unused)
- 96 kHz operation (`SetAudioSampleRate` before `pod.Init()`) for more headroom
  above the top roll off
- Per-model tone control via a hidden menu on button 1 + knob 2
