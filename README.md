# Daisy Pod Projects

Effects for the **Electrosmith Daisy Pod**, built on libDaisy and DaisySP. Each
folder is a standalone firmware project with its own Makefile.

| Project | What it is |
|---------|------------|
| [delay](delay/) | Analog-style delay — darkening repeats, tap tempo, tape wobble |
| [stutter-glitch](stutter-glitch/) | Euclidean-pattern stutter/glitch — CHOP, STUTTER, PITCH, REVERSE |
| [buffer-scramble](buffer-scramble/) | Slice re-sequencer — captures a window of 8 slices and replays it shuffled |
| [specter-resonator](specter-resonator/) | Resonant bank — your playing excites 16 tuned resonators, freeze holds the ring. Builds under `experimental-builds/` |
| [legacy-harmonic-saturator](legacy-harmonic-saturator/) | Dual-engine guitar drive — two models, series in either order or parallel, cabinet sim, 8 presets, tuner |
| [frugal-looper](frugal-looper/) | 60 second stereo looper — overdub, stop, erase, half speed / reverse / double |
| [16Jobs](16Jobs/) | Step filter sequencer — Euclidean cutoff pattern with resonance and five shapes |
| [SuperCut](SuperCut/) | Endless rising or falling allpass sweep with feedback |
| [OddWay](OddWay/) | Vowel filter — three formants, tap-synced sweep, voice size |
| [ClantonCookie](ClantonCookie/) | Auto-swell into a hand-built Schroeder reverb with infinite freeze |
| [vertigo](vertigo/) | Tape wow, flutter and drift, stereo modulated delay |

## Setup

Requires the [Daisy Toolchain](https://github.com/electro-smith/DaisyWiki/wiki/1.-Setting-Up-Your-Development-Environment)
(arm-none-eabi-gcc, make, dfu-util). On Windows, run `make` from Git Bash.

libDaisy and DaisySP are shared by every project and live in `lib/`, which is
gitignored. Fetch them once after cloning:

```bash
mkdir -p lib
git clone https://github.com/electro-smith/libDaisy.git lib/libDaisy
git clone https://github.com/electro-smith/DaisySP.git lib/DaisySP

# Build the libraries once (this takes a few minutes)
make -C lib/libDaisy
make -C lib/DaisySP
```

If you already have them elsewhere, skip the clone and point the build at them:

```bash
make LIBDAISY_DIR=/path/to/libDaisy DAISYSP_DIR=/path/to/DaisySP
```

## Building and flashing

From inside a project folder:

```bash
cd delay            # or stutter-glitch
make -j8
```

Put the Pod into DFU mode: hold **BOOT**, press **RESET**, release **RESET**,
then release **BOOT**. Then:

```bash
make program-dfu
```

> `dfu-util` prints `Error during download get_status` at the end. This is
> expected — the device has already rebooted out of DFU mode. The flash
> succeeded.

## Project Structure

```
Daisy-Pod-Projects/
├── delay/            # Analog-style delay pedal
├── stutter-glitch/   # Euclidean stutter/glitch pedal
├── buffer-scramble/  # Slice re-sequencer
├── specter-resonator/  # Resonant bank, builds under experimental-builds/
├── legacy-harmonic-saturator/  # Dual-engine drive
├── frugal-looper/    # 60 second stereo looper
├── 16Jobs/           # Step filter sequencer
├── SuperCut/         # Endless sweeping phaser
├── OddWay/           # Vowel filter
├── ClantonCookie/    # Auto-swell into infinite reverb
├── vertigo/          # Tape modulation
├── lib/              # libDaisy + DaisySP (gitignored, see Setup)
└── .gitignore
```
