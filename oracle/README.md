# oracle

Pitch-tracking mono synth for the **Electrosmith Daisy Pod**, built on libDaisy
and DaisySP. It listens to what you play, works out the fundamental, and sings it
back with band-limited oscillators through a resonant filter that sweeps on every
note. Monophonic — one voice, tracked from the input.

## Controls

| Control | Function |
| --- | --- |
| Knob 1 | CUTOFF — filter base frequency, 80 Hz to 6 kHz |
| Knob 2 | RESONANCE — filter Q, 0.5 to 8 |
| Button 1 | OCTAVE — cycle −1 / 0 / +1 |
| Button 2 | Hold to FREEZE — drone the last tracked note |
| Encoder turn | WAVEFORM — SAW → SQUARE → SUB → SAW+SUB |
| Encoder press | Bypass toggle — both LEDs go dark |
| LED 1 | Gate brightness, hue = detected pitch class (a tuner, roughly) |
| LED 2 | Waveform colour (amber / cyan / violet / pink) |

## How it works

- **Pitch tracking.** The input is low-passed and decimated 4× to 12 kHz, then a
  YIN-style detector runs every 128 decimated samples (~10 ms) over a 512-sample
  window: difference function → cumulative-mean-normalised difference → first
  dip below threshold → parabolic-interpolated lag → frequency. That is the
  fundamental, and it is smoothed before it reaches the oscillator.
- **Gate / trigger.** A fast-attack, slow-release follower on the input
  amplitude opens the gate (with hysteresis) and fires the note trigger.
- **Oscillators.** A phase accumulator drives PolyBLEP-corrected saw and square
  waves plus a half-rate sub. The pitch slews a few ms toward the target, so
  legato lines glide instead of clicking.
- **Filter.** A TPT state-variable low-pass, resonance from Knob 2. On each note
  trigger the cutoff is swept up by the filter envelope and decays back, which is
  where the "synth" comes from.
- **Envelopes.** A short-attack ASR controls the VCA (release when the input
  falls away); a fast AD sweeps the filter.
- **Freeze.** Hold Button 2 to latch the last note and open the VCA — an
  instant drone.

## Response time

Note-on is driven by the input attack, not by the pitch detector, so the VCA opens
within ~2 ms of the pick and the envelope-swept filter fires immediately. The
oscillator runs continuously and **snaps** to the first confidently-tracked pitch
after each new note, then re-detects every ~2.7 ms. Lock therefore costs about
one period of the note plus a couple of milliseconds — near-instant on high
notes, and ~12–15 ms on the low E, which is the physical floor for resolving a
12 ms period.

## Setup

Requires the [Daisy Toolchain](https://github.com/electro-smith/DaisyWiki/wiki/1.-Setting-Up-Your-Development-Environment) (arm-none-eabi-gcc, make, dfu-util). On Windows, run `make` from Git Bash.

Part of the [Daisy Pod Projects](../README.md) repo. The libraries live one level
up in `lib/`, which is gitignored, so fetch them once after cloning:

```
mkdir -p ../lib
git clone https://github.com/electro-smith/libDaisy.git ../lib/libDaisy
git clone https://github.com/electro-smith/DaisySP.git ../lib/DaisySP
make -C ../lib/libDaisy
make -C ../lib/DaisySP
```

## Building and flashing

```
make -j8
make program-dfu   # with the Pod in DFU mode: hold BOOT, tap RESET
```

> `dfu-util` prints `Error during download get_status` at the end. This is
> expected — the device has already rebooted out of DFU mode.

## Tuning

| Constant | Default | Meaning |
| --- | --- | --- |
| `kDecim` / `kDecimSr` | `4` / `12000` | Decimation for pitch detection |
| `kPitchWindow` | `384` | Analysis window, decimated samples (~32 ms) |
| `kLagMin` / `kLagMax` | `12` / `200` | ~1000 Hz to ~60 Hz |
| `kHop` | `32` | Decimated samples between detections (~2.7 ms) |
| `kYinThresh` | `0.18` | Voiced/unvoiced aperiodicity threshold |
| `kGateOn` / `kGateOff` | `0.020` / `0.010` | Gate hysteresis (on the 3× gained follower) |
| `kInGain` | `3.0` | Input gain into the gate follower |
| `kVcaAttack` / `kVcaRelease` | `0.0120` / `0.00020` | VCA envelope (~1.7 ms / ~100 ms) |
| `kEnvRelease` | `0.0010` | Gate follower release (~21 ms) |
| `kFiltDecay` | `0.00015` | Filter sweep decay (~140 ms) |
| `kPitchSlew` | `0.0150` | Portamento (~1.4 ms) |
| `kBaseCutMin` / `kBaseCutMax` | `400` / `14000` | Cutoff range |
| `kKeyTrackRef` / `kKeyTrackAmt` | `110` / `0.4` | Key tracking (A2 reference) |
| `kSweepAmount` | `5.0` | Filter envelope sweep depth |
| `kOutputLevel` | `0.8` | Output level |

## Testing ideas

For the `daisy-pedal-test-harness`: the natural specs are **tracking accuracy**
(play a sine at a known frequency, measure the fundamental of the output), **gate
latency** (click → note-on delay), **octave switching** (output fundamental ratio)
and **freeze hold** (note pitch stable with input removed). Faults worth
injecting: `pitch_offset_cents`, `gate_threshold_drift`, `octave_wrong`.

## Project Structure

```
oracle/
├── main.cpp
├── Makefile
├── README.md
└── .gitignore
```
