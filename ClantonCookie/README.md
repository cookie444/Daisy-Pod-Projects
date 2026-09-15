# ClantonCookie

An auto-swell into an infinite reverb for the **Electrosmith Daisy Pod**, built on libDaisy and DaisySP. Play a note and it blooms in with no attack, then you freeze the tail and play over it. Single notes become pads.

## Controls

| Control | Function |
|---------|----------|
| Knob 1 | SWELL — how slowly each note blooms in, 20 ms to 2 s |
| Knob 2 | SIZE — room size and decay together |
| Button 1 | Press to freeze the tail. Press again to release it |
| Button 2 | Toggle the tape saturation circuit, off by default |
| Encoder turn | Tone — damping in the feedback, dark to bright |
| Encoder press | Bypass toggle — both LEDs go dark |
| LED 1 | Swell amount |
| LED 2 | Blue clean, amber tape on, white frozen |

## Use

A pad machine: your playing disappears into a bloom and comes out the other side
as a reverb. Set it up, in this order:

1. **Start with Knob 2 (SIZE) around noon** and Knob 1 (SWELL) low — a quarter
   turn. Bypass off (encoder press) so the LEDs are lit.
2. Play a note. With SWELL low it arrives almost instantly; turn SWELL up and
   each note fades in slowly instead of attacking.
3. Build a chord — keep playing notes and they ring and stack into the tank.
   Turn Knob 2 up for a bigger, longer room; down for a tighter one.
4. **Freeze it:** press Button 1. The tail holds forever, the dry input stops
   feeding it, and LED 2 goes white. Play over the frozen pad, then press
   Button 1 again to release it and start feeding the tank again.
5. **Warm it up:** press Button 2 and LED 2 goes amber — a tape saturation
   stage (2.2x tanh drive, 7 kHz roll-off) makes the pads sit thicker and
   darker. Press again to go back to clean blue.
6. **Darken or brighten the room:** turn the encoder. Counter-clockwise is
   darker, damped tails; clockwise is brighter and more present.

If the pad gets too loud, back off Knob 2 or Knob 1 — the output is soft-clipped,
not limited, so it can sit at full scale for a while.

## How it works

- The swell is an envelope follower driving a gain. It rises over the swell time and falls four times faster, so every note arrives as a bloom with the pick attack removed.
- The reverb is a Schroeder tank built by hand: four damped combs in parallel into two allpasses, one per channel, with the right channel offset by 23 samples for width.
- Freezing pushes the comb feedback to 0.999 and gates the input, so what is already in the tank rings on and nothing new goes in. Release it and playing resumes.
- Size scales the delay lengths and the decay together, so bigger rooms last longer.
- This DaisySP build has no reverb module, hence the hand-built tank.

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
| `kCombLen[]` | `1557 … 1422` | Comb delay lengths in samples |
| `kMinSwellMs` / `kMaxSwellMs` | `20` / `2000` | Knob 1 range |
| `kSizeMin` / `kSizeMax` | `0.4` / `1.3` | Knob 2 range |
| `kFbMin` / `kFbMax` | `0.60` / `0.995` | Comb feedback range |
| `kFreezeFb` | `0.999` | Feedback while frozen |
| `kTapeDrive` | `2.2` | Saturation amount, obvious but still tape |
| `kTapeHfHz` | `7000` | Where the tape top end rolls off |
| `kTones[]` | `0.85 … 0.12` | Damping per encoder position |
| `kOnset` | `0.02` | Level that counts as a note starting |

## Project Structure

```
ClantonCookie/
├── main.cpp
├── Makefile
├── README.md
└── .gitignore
```
