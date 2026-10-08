# ensemble

Ensemble chorus for the **Electrosmith Daisy Pod**, built on libDaisy and DaisySP.
Three or four modulated delay taps run off one buffer, each on its own slowly
detuned LFO and panned across the stereo field, with a BBD-style compander for
the "breathing" character. Reference topologies are the Solina String Ensemble
(phase-offset voice bank) and the Roland Juno-6/60 (compander pre/de-emphasis).
Pink LEDs throughout.

## Controls

| Control | Function |
| --- | --- |
| Knob 1 | DEPTH — modulation depth, 0 to 4 ms |
| Knob 2 | RATE — base LFO rate, 0.1 Hz (drift) to 8 Hz (flutter); moving it takes the rate back from tap tempo |
| Button 1 (tap) | TAP TEMPO — locks the LFO to one cycle per beat |
| Button 1 (hold 0.7 s) | Cycle mode: ENSEMBLE → CHORUS → VIBRATO |
| Button 2 | BUMP — a random record / cassette knock, different every press |
| Encoder turn | WIDTH — stereo spread of the voice pans (mono → wide) |
| Encoder press | Bypass toggle — both LEDs go dark |
| LED 1 | Pink, follows the modulation; flashes on a bump |
| LED 2 | Mode colour (pink shades) |

## Modes

| Mode | Voices | LFO phases | Rate scale | LED 2 | Character |
| --- | --- | --- | --- | --- | --- |
| ENSEMBLE | 3 | 0° / 120° / 240° | ×1.0 | soft pink | Lush Solina swirl, slow drift |
| CHORUS | 2 | 0° / 180° | ×1.6 | magenta | Tighter Juno-style, faster |
| VIBRATO | 1 | — | ×1.0 | rose | Wet-only pitch wobble |

## Tap tempo

Tap Button 1 a few times and the LFO locks to one cycle per beat (averaged over
the last four taps). It holds until you turn Knob 2, which hands the rate back
to the knob. Taps are ignored inside a 3 s window reset.

## The bump

Button 2 knocks the tape: for the duration of the bump the whole signal
crossfades to 100% wet and its playback speed gets shoved around, so the sound
itself lurches and warbles. Fresh random values every press:

- **length** (150–600 ms) and **direction** (speed up or down),
- **speed lurch** (2–9 % peak, 1–3 wobbles across the bump, springs back),
- **wow / flutter** (4–20 Hz, 4–36 samples deep), and
- an optional **dropout** (0–30 % gain dip).

The lurch is an accumulated playback-speed offset, so the pitch bends and
returns to where it started; the wow/flutter rides on top. Crossfading to full
wet is what puts *your* signal — not a synthetic effect — through the warp.
There is no added tone.

## How it works

- One circular buffer per channel, read with Hermite interpolation so the pitch
  glides instead of stepping.
- Every voice sits at the same 12 ms centre but on its own phase and rate. The
  rate ratios (1.00 / 1.19 / 1.37 / 1.53) are deliberately irrational so the
  voices drift in and out of phase instead of locking into a periodic warble.
- WIDTH blends each voice's equal-power pan between centre and its full spread;
  a single-voice mode is forced centre so VIBRATO stays stereo.
- The compander pre-emphasises the signal entering the delay and de-emphasises
  the wet return — the cheap, stable stand-in for a NE570 that gives the
  dynamic "suck and swell". A gentle reconstruction low-pass finishes the BBD
  impression.
- The whole output gets a fixed +10 % (`kOutputGain`) so it sits a touch louder
  than unity.

## Setup

Requires the [Daisy Toolchain](https://github.com/electro-smith/DaisyWiki/wiki/1.-Setting-Up-Your-Development-Environment) (arm-none-eabi-gcc, make, dfu-util). On Windows, run `make` from Git Bash.

This project is part of the [Daisy Pod Projects](../README.md) repo. The libraries are shared by every project and live one level up in `lib/`, which is gitignored, so fetch them once after cloning:

```
mkdir -p ../lib
git clone https://github.com/electro-smith/libDaisy.git ../lib/libDaisy
git clone https://github.com/electro-smith/DaisySP.git ../lib/DaisySP

# Build the libraries once (this takes a few minutes)
make -C ../lib/libDaisy
make -C ../lib/DaisySP
```

## Building and flashing

```
make -j8
make program-dfu   # with the Pod in DFU mode: hold BOOT, tap RESET
```

> `dfu-util` prints `Error during download get_status` at the end. This is expected — the device has already rebooted out of DFU mode, and `dfu-util` exits non-zero.

## Tuning

| Constant | Default | Meaning |
| --- | --- | --- |
| `kMaxDelay` | `2048` | Buffer size, ~42 ms at 48 kHz |
| `kBaseDelayMs` | `12` | Centre of the modulation |
| `kMaxDepthMs` | `4` | Knob 1 at full |
| `kMinRateHz` / `kMaxRateHz` | `0.1` / `8` | Knob 2 range |
| `kDetune[4]` | `1.00, 1.19, 1.37, 1.53` | Per-voice rate ratios |
| `kPhase[4]` | `0, 120, 240, 180` | Per-voice LFO phase (deg) |
| `kPan[4]` | `-1, 0, 1, 1` | Per-voice pan |
| `kDryWet` | `0.5` | Default mix |
| `kOutputGain` | `1.10` | Fixed output boost, +10 % |
| `kModeHoldMs` | `700` | Button 1 hold to cycle mode |
| `kMinBeatMs` / `kMaxBeatMs` | `200` / `1500` | Tap tempo clamp |
| `kEmphGain` / `kPreEmphHz` | `0.35` / `1000` | Compander depth and corner |
| `kReconHz` | `12000` | Reconstruction low-pass corner |
| `kDcHz` | `12` | Input DC blocker corner |

## Testing

The `daisy-pedal-test-harness` verifies this pedal against measurable thresholds.
New specs land in the `Modulation` group (see `ensemble-chorus-spec.md`):

| Spec | Stimulus | Measured how | Threshold |
| --- | --- | --- | --- |
| `chorus_rate_hz` | 1 kHz sine, VIBRATO | instantaneous-frequency FFT peak | knob rate ± 5 % |
| `chorus_depth_ms` | same capture | `Δf_peak / (2π · f · r)` | knob depth ± 10 % |
| `chorus_notch_hz` | noise, depth 0, mix 50 % | comb notch spacing = `1/τ₀` | ± 2 % |
| `chorus_width` | mono sine | side-vs-mid energy | side ≥ −6 dB, corr ≤ 0.8 |
| `processing_latency_ms`, `thd_pct`, `noise_floor_dbfs`, `fr_*` | reuse | existing specs | unchanged |

Fault injection proves the rig catches regressions: `chorus_rate_drift_pct`,
`chorus_depth_error_pct`, `chorus_delay_error_pct`, `chorus_mono_sum`,
`bbd_noise_dbfs`, `latency_extra_ms`.

## Project Structure

```
ensemble/
├── main.cpp
├── Makefile
├── README.md
└── .gitignore
```
