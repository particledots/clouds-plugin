# Unofficial Clouds — AU / VST3 port (work in progress)

A plugin wrapper around the Mutable Instruments **Clouds** DSP, taken from the open-source release
(`pichenettes/eurorack`, `clouds/dsp` + `clouds/resources.*`, plus `stmlib`). This is the stock Clouds firmware, so it can be compared
against a real Clouds. Not a Mutable Instruments product. Don't contact Emilie Gillet or Mutable Instruments about it.

## Easiest: let GitHub build it
`.github/workflows/build.yml` builds the AU and VST3 on a macOS runner (universal arm64 + x86_64, macOS 11+).
Download `Clouds-macOS` from the latest run's Artifacts (Actions tab). `INSTALL.txt` inside covers the install.

## Build it yourself (macOS)
```
brew install cmake
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```
It shows up in Logic as "particledots: Unofficial Clouds". The AU is declared as a *MIDI-controlled effect* (`aumf`),
so MIDI note-ons can fire the module's TRIG input.

Headless DSP test (any OS): `cmake --build build --target engine_test && build/engine_test`

## Parameters (mapped from the panel)

| Plugin | Module |
|---|---|
| Position, Size, Density, Texture | the four main knobs |
| Pitch | semitones, ±48 (on the module: the pitch knob plus V/oct) |
| Dry/Wet, Stereo Spread, Feedback, Reverb | the module's single **BLEND** knob has four roles (selected by its button); here each role has its own knob |
| Freeze | the freeze button / gate |
| Trigger (button) and MIDI note-on | the TRIG input: seeds a grain in Granular, restarts the loop / window in the other modes |
| Pitch Follows MIDI | optional: the last MIDI note transposes Pitch (middle C = no change) |
| Mode | Granular, Pitch shifter / time stretch, Looping delay, Spectral madness |
| Quality | stereo 16-bit, mono 16-bit, stereo 8-bit (low-fi), mono 8-bit (low-fi), in the module's order |

Switching Mode (between Spectral and the others) or Quality clears the buffer and unfreezes, exactly as on the module.

## How it works / honest limits

* The module DSP is called as the firmware does: 32 kHz (16 kHz in the low-fi qualities), 32-frame blocks, 16-bit samples, the module's
  118 784-byte + 65 408-byte memory blocks (so the recording buffer is the module's length, about one second). A windowed-sinc resampler
  converts to and from the host rate. The dry signal also goes through this path, as on the module, so nothing above about 16 kHz survives.
* Each knob is smoothed per block with the firmware's own coefficients, so fast knob moves feel as slow as on the hardware.
* The firmware runs a background `Prepare()` step in its main loop (buffer rebuilds, the spectral mode's FFT, the time-stretch correlator).
  The wrapper runs it 16 times per 32-frame block.
* The module's CV inputs, calibration, VU meter, sample-memory save/load and the pot/UI code are left out.
* **Verified here (Linux, October 2026):** the DSP and wrapper compile with g++ and run at 44.1 / 48 / 96 kHz in all four modes and all four qualities, with
  finite, bounded, non-silent output; Freeze keeps sounding after the input stops; switching mode and quality while running does not crash
  (`engine_test`). The plugin sources compile against the JUCE 8.0.6 headers without warnings.
* **Not verified:** the macOS/AU build (until the GitHub run passes), loading in Logic on either Mac, MIDI trigger behaviour in a real host,
  and **how closely it sounds like a real Clouds**. Nobody has done an A/B against a module yet.
* The UI is a basic custom panel. It is functional rather than polished.

## Where the source came from, and two fixes

* `third_party/clouds/` is `clouds/dsp/*` and `clouds/resources.*` from `pichenettes/eurorack`, commit `f4cfa7697095a28d292bff758beb51deb49b5a86`.
* `third_party/stmlib/` is the subset of `pichenettes/stmlib` at commit `e3bd7c9cc00e4364166f9905c0509b6ffd0535ec` that Clouds needs.
* Changes to upstream:
  1. The hardware-only `#include "clouds/drivers/debug_pin.h"` line was removed from `granular_processor.cc`.
  2. `window.h`: `Window::Start()` now also sets `done_ = false`. In that commit it didn't, which left the **Pitch shifter / time stretch**
     mode permanently silent (its windows were never marked as playing). The Parasites and SuperParasites forks both have the line.
     I don't know why that commit lacks it, and I haven't compared the rest of the file against older upstream history.
* The engine allocates the processor from zeroed memory, because some of it (for example the silence flag) is never initialised by the code
  itself: on the module it lives in zeroed global memory.

## Licensing

`third_party/` is MIT-licensed code by Emilie Gillet (STM32F projects in the upstream repo are MIT; copyright notices are kept in the file headers;
see `third_party/stmlib/LICENSE`). Keep those notices if you redistribute. The hardware design is CC-BY-SA; none of it is used here.
The wrapper code in `src/` has no licence assigned yet. JUCE is used under its own licence terms (AGPLv3 / commercial); check them before distributing builds.
