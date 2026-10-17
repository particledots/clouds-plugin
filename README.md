# Unofficial Clouds — AU / VST3 port (work in progress)

A plugin wrapper around the Mutable Instruments **Clouds** DSP, taken from the open-source release
(`pichenettes/eurorack`, `clouds/dsp` + `clouds/resources.*`, plus `stmlib`). This is the stock Clouds firmware, so it can be compared
against a real Clouds. Not a Mutable Instruments product. Don't contact Emilie Gillet or Mutable Instruments about it.

## Limiter switch (version 0.3.0)
A new **LIMITER** button (parameter `limiter`) sits at the end of the button row. **On (the default) is exactly the 0.2.0 sound**: the soft limiter after Output Level keeps loud sounds from going past 0 dBFS.
**Off** removes it: the output is then the engine plus the Output Level gain and nothing else, so loud wet sounds can go past 0 dBFS (by tens of dB at high Output Level settings). Only two safety things
remain: NaN becomes silence and values are held inside +/-16 so a runaway can never reach the host as infinity. Use Output Level, or a limiter after this plug-in, to keep it in range.
Switching glides over about 10 ms, so there is no click. Presets and projects saved before 0.3.0 have no switch and load with the limiter **on**, i.e. they sound as they did in 0.2.0.
Why it exists: the limiter is an instant curve (no attack, no release, no look-ahead), so on very loud sounds it acts like gentle distortion, which can be heard; the switch lets you turn that off and manage the level yourself.

## Output Level (version 0.2.0)
**What was wrong.** Loading the plug-in made the sound quieter than before it was loaded. With Dry/Wet at 0 (nothing but the input) I measured the output at **-9.04 dB** relative to the input in
all four modes (Granular, Pitch shifter / time stretch, Looping delay, Spectral madness), at 44.1, 48 and 96 kHz host rates and in all four qualities. (This is the stock Clouds code; an earlier note only had the
same measurement from the SuperParasites family.) The cause is in the firmware code, not the wrapper: its dry/wet crossfade table starts at 0.707 (-3 dB) and its output conversion halves the signal
(x 0.5, -6 dB) before the 16-bit output. The module's analogue output stage presumably makes that up; a plug-in has none.

**The fix.** A new **OUTPUT LEVEL** knob (parameter `output_level`, -24 to +24 dB, default **+9.03 dB** = 20 x log10 (2 x sqrt 2)) sits after the engine, followed by a **soft limiter**.
With the default, a fully dry signal comes out at the level it went in. The limiter passes everything below -0.9 dBFS unchanged and bends louder peaks towards 0 dBFS, which they
never exceed; it is an instant curve (no delay, no look-ahead), a safety net against clipping rather than a mastering limiter. The gain glides (about 10 ms) so moving or automating the knob does not click.
The engine and firmware code are untouched. The stage lives in `src/OutputStage.h` and is called from `processBlock`.

**What it does not do.**
* Loud dry signals are bent slightly by the firmware's own soft limit, before this stage: Looping delay, dry, input -6 dBFS: -0.06 dB; -1.9 dBFS: -0.16 dB; -0.4 dBFS: -0.22 dB.
* The firmware's output is 16-bit and is 9 dB lower than the input before this gain is applied, so the gain also raises the firmware's own 16-bit rounding noise by 9 dB (it does not add any). I have not measured or listened for it in quiet passages.
* **Presets and projects saved before 0.2.0 have no Output Level**, so they load at the +9.03 dB default and come out about 9 dB louder than they did. That is the fix, but if you
  had turned the channel up to compensate, turn it back down.

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
| Output Level (version 0.2.0) | not on the module: makes up the firmware's output attenuation (-24 to +24 dB, default +9.03 dB), followed by a soft limiter; see the Output Level section |
| Limiter (version 0.3.0) | not on the module: soft limiter after Output Level, on by default; off lets loud sounds go past 0 dBFS |
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

## Verified and not verified (Output Level, version 0.2.0)

**Limiter switch (version 0.3.0): built on GitHub Actions (macOS runner), every step passed, run 39075614416, commit `7796aca`** (all earlier tests, the extended level tests and both mutation checks, the universal arm64 + x86_64 AU and VST3 build, signing, packaging). I can see each step's pass / fail, not its log text. Not yet loaded in Logic and not listened to.

**Limiter switch (version 0.3.0) - measured and tested here (Linux, g++, October 2026), not listened to**
* `tests/level_test.cpp`: 10355 checks, 0 failed (the 0.2.0 checks plus the switch): with the limiter on, a loud signal never passes 1.0; off, the same signal comes out at exactly the Output Level gain (within 0.0001) and
  its peak is past 1.5; a quiet signal is identical on and off; NaN and infinity never get through when off; switching glides (no step above 2 % of the whole move) and settles exactly, and switching back on gives exactly the 0.2.0 value.
* `tests/proc_level_test.cpp`: 25 checks, 0 failed, on the real processor class: the parameter exists and is on by default; a preset saved without it loads with the limiter on; with Output Level at +24 dB and a loud wet input
  (four modes) the peak is 1.0000 with the limiter on and well past 1.0 with it off, so the switch really reaches the sound.
* Mutation checks: `tests/mutation_check.sh` now breaks the stage in 7 ways (the earlier 4 plus switch ignored one way and the other, and the off-guard removed) and `tests/wiring_mutation_check.sh` in 6 ways (the earlier 3 plus
  switch not read one way and the other, and default wrong); every broken copy was caught.
* Not tested: how the switch sounds or looks in Logic, whether the button fits the panel as intended, Logic's handling of the new parameter in an existing project (the plug-in code is unchanged).

**Built on GitHub Actions (macOS runner): every step passed, run 39057592273, commit `3c043ae`** (engine test, the four level steps, the universal arm64 + x86_64 AU and VST3 build, signing, packaging). I can see each step's pass / fail, not its log text. Not yet loaded in Logic and not listened to.
**Measured and tested here (Linux, g++, October 2026), not listened to**
* `tests/level_test.cpp`: 10,347 checks, 0 failed. Fully dry (Dry/Wet = 0) at the default Output Level, engine + stage output equals the input level within 0.1 dB in each of the four modes, at 44.1 / 48 / 96 kHz host, all four
  qualities, input -18 dBFS (and -6 dBFS at 48 kHz): 64 runs, worst deviation 0.063 dB. The limiter: identity up to its threshold, odd-symmetric, monotonic, slope 1 at the threshold,
  never above 1.0 for any input including infinity, NaN becomes 0. The gain is exact to 0.001 dB at -24, -6, 0, +9.03 and +12 dB; a step from +9.03 to -24 dB glides with no jump. No clipping: with a 0.95-amplitude input, all modes
  and qualities, Dry/Wet 0 / 0.5 / 1, Output Level at the default and at +24 dB (96 runs) no sample exceeds 1.0; without the limiter the engine's wet peaks at the default gain would reach about 2.5 x full scale.
* `tests/proc_level_test.cpp`: 18 checks, 0 failed, on the real plug-in processor class (the same `PluginProcessor.cpp` as the AU / VST3, compiled against JUCE 8.0.6, run headless): the parameter exists and its default shows as "+9.03 dB";
  all four modes come out within 0.010 dB of the input at the defaults; setting Output Level to -24 / -9.03 / 0 / +6 dB moves the output by exactly that much (within 0.02 dB; at 0 dB the output is -9.04 dB, the unfixed level);
  loud input at +24 dB in every mode stays at or below 1.0; a preset saved without an Output Level loads at +9.03 dB (and keeps its other settings), also into an instance whose knob had been turned to 0 dB.
* **Mutation checks (the tests fail when the level is broken).** `tests/mutation_check.sh` breaks `OutputStage.h` in four ways (gain removed, default 8.0 dB instead of 9.03, wrong dB-to-gain factor, limiter removed) and the level test failed for every one
  (13, 9, 13 and 4,020 failed checks), while passing on the unbroken code. `tests/wiring_mutation_check.sh` breaks `processBlock` in three ways (stage not called, level parameter not read, parameter default 0 dB) and the whole-plug-in test failed each
  time (7, 7 and 6 failed checks); the source is restored afterwards. Both are workflow steps.
* The existing suite still passes with the new CMake targets (`engine_test`); the engine and firmware code are not touched.
* The editor source (the fifth knob in the second row) compiles against JUCE 8.0.6 with the rest of the plug-in. **How it looks is not checked: I have not seen the editor.**

**Not verified:** how it sounds; that the level you read on Logic's meters / faders now matches; the 16-bit noise floor at +9 dB in quiet passages; the knob row in Logic; behaviour when automated; any match with a real Clouds' output level.

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
