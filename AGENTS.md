# AGENTS.md — working notes for AI agents

Read this first if you're an agent picking up this repo. It covers how to build,
flash, debug and extend these Daisy Pod effects, the conventions to match, and
the specific traps that have already cost real time here.

(For humans, the top-level `README.md` is the normal entry point. This file is
the agent-facing companion.)

---

## What this repo is

Firmware for the **Electrosmith Daisy Pod**, one standalone effect per folder,
built on **libDaisy** and **DaisySP**. Each folder has its own `main.cpp` and
`Makefile`; the libraries are shared and live in `lib/` (gitignored).

Current projects: `delay`, `stutter-glitch`, `buffer-scramble`,
`specter-resonator`, `legacy-harmonic-saturator`, `frugal-looper`, `16Jobs`,
`SuperCut`, `ClantonCookie`, `vertigo`, `OddWay`, `ensemble`, `oracle`.

---

## Build & flash

`make` needs a Unix shell. On Windows, drive it through Git Bash:

```bash
cd <project>
make -j8
make program-dfu     # Pod must be in DFU (see below)
```

**Entering DFU:** hold **BOOT**, tap **RESET**, release **RESET**, release
**BOOT**. `dfu-util -l` should then show `0483:df11`.

- `dfu-util` prints `Error during download get_status` and exits non-zero **even
  on success** — the device has already rebooted out of DFU. Success looks like
  `Download done` + `File downloaded successfully`. Don't “fix” this.
- There is **no way to flash without a human physically entering DFU**. Batch
  your changes to minimise flash round-trips.
- After flashing, the DFU device disappears; wait ~6 s before opening the USB
  serial port.

If you are on Windows with no toolchain, the setup used here was: winget install
`ezwinports.make`, `Git.Git`, `Arm.GnuArmEmbeddedToolchain` (14.2.Rel1), plus
`dfu-util` binaries extracted to a scratch dir. See "Environment" below.

---

## Two mistakes that cost the most time — don't repeat them

### 1. `fonepole` coefficient is `1 - exp(...)`, not `exp(...)`

`fonepole(out, in, coeff)` computes `out += coeff * (in - out)`, so `coeff` is a
*smoothing alpha* (small = slow). For a one-pole corner `fc`:

```cpp
float coef = 1.0f - expf(-2.0f * kPi * fc / sr);   // CORRECT
// float coef = expf(-2.0f * kPi * fc / sr);       // WRONG -> near all-pass (silence for a DC blocker)
```

The wrong form turns a "12 Hz low-pass" into ~49 kHz, so a DC blocker
`in - lp(in)` subtracts the signal from itself and the output goes to **digital
zero**. This silently muted `ensemble` across several builds. If a pedal outputs
silence with a clean build, check every `fonepole` coefficient first.

### 2. PowerShell `Copy-Item -Recurse` nests when the destination exists

`Copy-Item -Recurse src dst` places `src` *inside* an existing `dst`
(`dst\src`), so you build stale source and flash the wrong thing. Copy files
individually when the destination exists:

```powershell
foreach ($f in "main.cpp","README.md","Makefile",".gitignore") { Copy-Item -Force "$src\$f" "$dst\$f" }
```

Always `Select-String`/`grep` the destination for the symbol you just added
*before* building. A phantom "my change doesn't work" was caused by exactly this.

---

## Debugging by USB serial (the most useful technique)

libDaisy has USB-CDC logging via `pod.seed`. Add temporary instrumentation:

```cpp
#define FOO_DEBUG 1
static volatile float dbg_in = 0.0f, dbg_out = 0.0f;   // ISR writes, main reads

// in AudioCallback, after producing output:
#if FOO_DEBUG
{ float mx = 0; for (size_t i = 0; i < size; i++) { float a = fabsf(out[i]); if (a > mx) mx = a; } dbg_out = mx; }
#endif

int main(void) {
    pod.Init();
#if FOO_DEBUG
    pod.seed.StartLog(false);                  // inits the internal USB CDC
    pod.seed.PrintLine("FOO DIAG boot");
#endif
    pod.SetAudioBlockSize(4);
    /* ... */
    pod.StartAdc();
    pod.StartAudio(AudioCallback);
#if FOO_DEBUG
    uint32_t t0 = System::GetNow();
    while (1) { uint32_t now = System::GetNow();
        if (now - t0 >= 200) { t0 = now;
            pod.seed.PrintLine("in=%d out=%d", (int)(dbg_in*1000.0f), (int)(dbg_out*1000.0f)); } }
#else
    while (1) {}
#endif
}
```

The Pod then enumerates as a CDC device (`VID_0483&PID_5740`), usually `COM5`.
Read it with `System.IO.Ports.SerialPort` at 115200.

**Capture-timing trap:** fixed-duration captures miss the user's playing because
they read your message *after* the capture tool call. Wait for activity before
recording (match a nonzero input line, then record a burst), or just ask them
what they hear — that's usually faster. Strip all diagnostics before the final
commit.

---

## Hardware / Pod facts

- 2 knobs (`pod.knob1/2.Process()` → 0..1), 2 buttons (`pod.button1/2.Pressed()`),
  1 encoder (`pod.encoder.Increment()` / `.Pressed()`), 2 RGB LEDs
  (`pod.led1/2.Set(r,g,b)` + `pod.UpdateLeds()`).
- Call `pod.ProcessAnalogControls(); pod.ProcessDigitalControls();` at the top of
  `AudioCallback`.
- Edge detection: compare `Pressed()` to a previous-value static. `RisingEdge()`
  stays true for a whole debounce window and misfires.
- **Knob positions are physical** — a fresh boot reads wherever the knob sits, so
  a code "default" is overridden immediately. Never rely on a default cutoff.
- `DelayLine<float, N>` reads with `ReadHermite(d)` where `d` is in samples.
  Writing before reading is fine; clamp `d` to `[1, N-2]`.
- `pod.seed` is public (`DaisySeed`): `usb_handle`, `StartLog`, `PrintLine`,
  `AudioSampleRate()`, `AudioBlockSize()`.

### The audio input is weak
A guitar into the Pod peaks around **0.02–0.10** (-40 to -20 dBFS), not ~0.5.
So an *effect* outputs what comes in and can sound "very quiet"/"no audio"; the
fix is input gain, not a DSP bug. A *synth* generates full-level output and masks
it, but its gate thresholds must be low or its follower gained.

---

## Conventions (match these)

```
<project>/
├── main.cpp
├── Makefile      # TARGET = <project>; LIBDAISY_DIR ?= ../lib/libDaisy; DAISYSP_DIR ?= ../lib/DaisySP
├── README.md     # Controls / How it works / Setup / Build+flash / Tuning / Project Structure
└── .gitignore    # build/, lib/, IDE + OS files
```

- `main.cpp` style: file header, `static DaisyPod pod;`, a `// Tuning` block of
  `static constexpr` constants, state globals, small static helpers,
  `AudioCallback`, `main`. Mirror the existing pedals.
- `Makefile` is copy-paste from another project, changing only `TARGET`.
- Keep the root `README.md` project table and tree updated.
- Bigger builds may add a `*-spec.md` design doc (see `ensemble-chorus-spec.md`).

### Adding a new pedal
1. Write `main.cpp`, `Makefile`, `.gitignore`, `README.md`.
2. Copy the files individually into `Daisy-Pod-Projects/<name>/` (see trap #2).
3. Build; fix; rebuild.
4. Flash (human enters DFU) and iterate with the user's ears.
5. When good: strip diagnostics, rebuild, flash clean, add to the root README,
   commit and push.

### C++ notes
- Builds with `-std=gnu++14`, no exceptions/RTTI. Avoid C++17-isms.
- `using namespace daisysp;` collides with DaisySP symbols. Don't name a helper
  `SoftClip` (DaisySP has one) — pick something else (`SoftSat`, etc.). Same care
  with `Chorus`, `Flanger`, `Phaser`, `Tremolo`, `Fonepole`.
- Prefer `constexpr` tuning constants at file scope.

---

## Environment (the Windows dev box this was built on)

Paths, if you're on this machine:

- Repo: `C:\Users\Brendan Koch\Documents\Default Project\Daisy-Pod-Projects`
- Git Bash (for `make`): `C:\Program Files\Git\bin\bash.exe`
- `arm-none-eabi-gcc`: `C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin`
- `make`: winget `ezwinports.make`
- `dfu-util`: scratch `.tools\dfu-util-0.11-binaries\win64\dfu-util-static.exe`

Every shell call must refresh PATH from the registry (winget tools aren't in the
running session):

```powershell
$env:Path = "$([Environment]::GetEnvironmentVariable('Path','Machine'));$([Environment]::GetEnvironmentVariable('Path','User'))"
```

The DFU device needed a driver on this box (problem code 28). It was fixed once
with a self-signed WinUSB INF + trusted cert via `pnputil`; flashing now needs no
admin. Git identity is set repo-local (`Brendan Koch <brendan.koch8@gmail.com>`);
pushes are over HTTPS with cached credentials — set `GIT_TERMINAL_PROMPT=0` in
non-interactive shells and check `$LASTEXITCODE` (git writes progress to stderr,
which PowerShell renders noisily).

---

## Testing

`daisy-pedal-test-harness` (separate repo) is a Python hardware-in-the-loop rig:
it plays stimuli into the Pod, measures latency/THD/frequency-response/decay and
asserts thresholds, emitting JUnit XML and an HTML report. It runs against a
simulated reference (`sim`), real hardware (`audio`) or saved WAVs. New specs go
in `config/rig.yaml` with a matching `_spec_*` method; fault injection proves the
rig catches regressions. Not set up in this repo; `ensemble-chorus-spec.md`
sketches the intended spec set.

---

## Working with the user

- **Confirm which pedal** when they refer to one by colour or nickname — a colour
  LED belongs to a specific firmware, and "pink" already meant a preset LED on
  one pedal rather than another pedal's LEDs.
- You cannot hear the device. Inspect states over serial, or ask a specific,
  multiple-choice question about what they hear.
- A flash always needs them to enter DFU; collect edits and flash once.
