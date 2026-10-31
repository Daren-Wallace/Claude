# Bass Booster — OBS Audio Filter Plugin: Implementation Plan

**Goal:** an adjustable OBS Studio audio filter that shows up under
*Audio Mixer → (source) ⋮ → Filters → + → "Bass Booster"* and puts bass back into
an input that currently has none.

---

## 0. Diagnose first (≈15 minutes, and it may change the design)

"No bass at all" usually means the low frequencies were **removed before OBS got
the audio**. That matters because an EQ can only turn up frequencies that are
still in the signal. If they've been stripped out completely, turning them up
just amplifies the noise floor. So the plugin needs two engines (section 2):
an **EQ boost** for bass that's weak, and **bass synthesis** for bass that's gone.

Common causes to check before building anything:

| Cause | How to check | Fix |
|---|---|---|
| Bluetooth headset in "Hands-Free"/HFP mode (telephone quality, 8–16 kHz mono, no lows) | Windows Sound settings shows the device as "Headset (Hands-Free)" | Pick the "Headphones (Stereo/A2DP)" device in OBS instead |
| Windows "Audio Enhancements" or the mic's own low-cut | Sound Control Panel → device → Properties → Enhancements / Advanced | Turn off enhancements; flip the hardware low-cut switch |
| Another OBS filter thinning the sound (Noise Suppression, a high-pass Compressor sidechain, etc.) | Disable the source's filters one at a time | Reorder or remove them |
| Low source sample rate (16 kHz VoIP or Discord capture) | Check the app's audio settings | Raise the source quality |

**Quick stop-gap while the plugin gets built:** OBS 29+ ships a **3-Band
Equalizer** filter, and the **VST 2.x Plug-in** filter can host free EQs such
as TDR Nova or ReaEQ. Both can boost weak bass. Neither can *synthesize*
missing bass, and that's the reason to build a dedicated plugin.

---

## 1. Technology & project setup

- **Language:** C++17 for the DSP core, with a thin C-ABI wrapper around libobs.
- **Scaffold:** the official [`obsproject/obs-plugintemplate`](https://github.com/obsproject/obs-plugintemplate)
  (CMake presets, GitHub Actions CI for Windows/macOS/Linux, installers).
- **Target:** OBS Studio 30+ (libobs API), 64-bit.
- **Layout:**

```
obs-bass-booster/
├── CMakeLists.txt / CMakePresets.json      (from template)
├── src/
│   ├── plugin-main.cpp        OBS_DECLARE_MODULE, obs_register_source
│   ├── bass-filter.cpp        obs_source_info callbacks + properties UI
│   └── dsp/                   NO libobs dependency → unit-testable
│       ├── biquad.hpp         RBJ cookbook filters (TDF-II transposed)
│       ├── bass_engine.hpp/.cpp  full processing chain
│       ├── harmonic_synth.hpp    psychoacoustic harmonic generator
│       ├── subharmonic.hpp       octave-down generator
│       ├── limiter.hpp           peak limiter / soft clipper
│       └── smoother.hpp          parameter ramping (no zipper noise)
├── data/locale/en-US.ini      UI strings
├── tests/                     Catch2 unit tests + WAV render tool
├── tools/render_wav.cpp       offline CLI: process a WAV for A/B listening
└── docs/                      README, user guide, screenshots
```

---

## 2. DSP design (per channel, real-time, zero added latency)

```
in → Input Trim → [A] Low-Shelf Boost → [B] Punch Peak → [C] Harmonic Synth (parallel mix)
   → [D] Sub-Octave Synth (parallel mix) → [E] Rumble HPF → [F] Limiter → Dry/Wet → Output Gain → out
```

| Stage | What it does | Controls (range, default) |
|---|---|---|
| **A. Low-shelf** | Boosts everything below a corner frequency. The main "more bass" control | Frequency 40–300 Hz (120), Gain 0 to +24 dB (+8), Slope/Q 0.3–1.5 (0.7) |
| **B. Punch peak** | Narrow bell boost for kick/body "thump" | Freq 40–200 Hz (80), Gain −6 to +12 dB (+3), Q 0.5–4 (1.0), on/off |
| **C. Harmonic synth** ("Restore") | Band-passes the lowest audio that survived (≈100–300 Hz) and runs it through a waveshaper that creates harmonics. Your ear fills in the missing fundamental (the MaxxBass-style "missing fundamental" effect), so even small speakers and phones "hear" bass | Amount 0–100 % (0), Drive 0–100 % (40), Band 80–400 Hz |
| **D. Sub-octave synth** | Detects the period of the low band (zero-crossing flip-flop divider with envelope following, like the dbx 120 / "subharmonic synth") and generates *real* new content one octave down. This is the only way to create bass that literally isn't in the input | Amount 0–100 % (0), Range 50–120 Hz, Tightness (envelope speed) |
| **E. Rumble HPF** | 2nd-order high-pass that keeps infrasonic/DC energy from eating headroom | 10–40 Hz (25), always on |
| **F. Limiter** | Bass boosts add a lot of peak level. A fast peak limiter with a soft-clip fallback stops OBS from clipping (red meter) | Ceiling −12 to 0 dBFS (−1), on/off |
| **Mix / Output** | Dry/wet blend plus makeup or trim gain | Mix 0–100 % (100), Output −24 to +12 dB (0) |

**Engineering rules**
- Filter coefficients use the RBJ Audio-EQ-Cookbook formulas. They're recalculated in `update()` and handed to the audio thread through a lock-free double buffer (atomic swap), so the audio thread never blocks.
- Every gain or frequency change ramps over about 20 ms (one-pole smoother) to avoid clicks and zipper noise.
- Filter state is kept per channel (up to `MAX_AUDIO_CHANNELS` = 8, which covers 5.1/7.1) and reset when the sample rate or channel count changes.
- Denormal protection: flush-to-zero plus tiny DC offset injection. NaN/Inf guard: reset state and pass dry audio.
- Sample rate comes from `audio_output_get_sample_rate(obs_get_audio())`, and coefficients are rebuilt if it changes.
- CPU budget: under 0.5 % of one core per stereo instance at 48 kHz.

---

## 3. OBS integration

```cpp
struct obs_source_info bass_booster = {
    .id             = "bass_booster_filter",
    .type           = OBS_SOURCE_TYPE_FILTER,
    .output_flags   = OBS_SOURCE_AUDIO,
    .get_name       = bb_get_name,        // "Bass Booster" (localized)
    .create         = bb_create,          // allocate BassEngine, read settings
    .destroy        = bb_destroy,
    .update         = bb_update,          // settings → engine params
    .get_defaults   = bb_defaults,
    .get_properties = bb_properties,      // the UI
    .filter_audio   = bb_filter_audio,    // obs_audio_data* (planar float) in-place
};
```

- `filter_audio` processes `audio->data[ch]` (planar `float*`) for `audio->frames` samples **in place** and returns the same pointer.
- **Properties UI** (built with `obs_properties_add_*`):
  - **Preset** dropdown: *Subtle Warmth · Voice Body · Music Boost · Restore Missing Bass (Phone/Bluetooth) · Extreme · Custom*. Picking a preset fills in the sliders, and touching any slider switches the preset to *Custom* (via `obs_property_set_modified_callback`).
  - Groups (`obs_properties_add_group`, collapsible): **Bass Boost** (A + B), **Bass Restoration** (C + D), **Protection** (E + F), **Output**.
  - dB and Hz suffixes on the sliders (`obs_property_float_set_suffix`), with tooltips on every control.
  - A **Bypass** checkbox for instant A/B comparison. The filter's eye icon in OBS also works.
- Settings are saved in the scene collection automatically, so they persist across restarts.

---

## 4. Testing

**Unit tests (Catch2, run in CI on all 3 OSes):**
1. **Frequency response:** feed sine waves at 30, 60, 120, 1 k and 5 kHz and measure the RMS gain. Expect +8 dB ±0.5 at 60 Hz with the defaults, 0 dB ±0.2 at 1 kHz, and a cut at 15 Hz from the HPF.
2. **Restoration:** feed a 300 Hz high-passed signal (no energy below 300 Hz). With the sub-octave synth on, check that new energy shows up an octave below the band's fundamental (FFT bin check). With the harmonic synth on, check that 2nd and 3rd harmonics show up.
3. **Stability sweep:** every parameter at min, max and random values across 44.1, 48 and 96 kHz, with 1–8 channels. Output must stay finite with no NaN/Inf.
4. **Limiter:** feed a +12 dB-hot signal and check the peak never goes over the ceiling.
5. **Smoothing:** step the gain from 0 to +24 dB mid-buffer and check the sample-to-sample jump stays under a set threshold (no clicks).
6. **Bypass / 0 % mix:** output matches the input bit-for-bit.
7. **Edge buffers:** frames = 0, 1, and odd sizes. Silence in gives silence out.
8. **Benchmark:** 10 minutes of stereo 48 kHz audio must process in under 0.5 % real-time CPU.

**Integration / manual:**
- `tools/render_wav` renders before/after WAVs from your actual problem audio for listening tests, plus spectrum plots.
- Load the plugin in OBS on Windows, macOS and Linux. Add it to a Mic/Aux and a Desktop Audio source, change settings while live, save and reload the scene collection, record a test clip, and check that the mixer meters don't clip.

---

## 5. Build, packaging, install

- GitHub Actions (from the template) builds on every push and produces:
  - **Windows:** `.zip` and installer. Files go in `C:\ProgramData\obs-studio\plugins\bass-booster\bin\64bit\bass-booster.dll` and `...\data\`.
  - **macOS:** universal (arm64 + x86_64) `.pkg` → `~/Library/Application Support/obs-studio/plugins/bass-booster.plugin`.
  - **Linux:** `.deb` and a tarball → `~/.config/obs-studio/plugins/bass-booster/`.
- Tagged releases (`v1.0.0`) publish the artifacts automatically.

---

## 6. Documentation

- **README:** what it does, install steps per OS, screenshots of the filter UI.
- **User guide:** what each knob does, recommended settings by scenario (voice vs. music vs. "phone-quality" input), and the diagnosis checklist from section 0.
- **Developer notes:** DSP block diagram, how to build, how to run the tests, how to add a preset.
- `CHANGELOG.md`, license (GPL-2.0-or-later, matching libobs).

---

## 7. Milestones

| # | Deliverable | Done when |
|---|---|---|
| 1 | Template scaffold builds; empty pass-through filter appears in the OBS Filters list | Visible in OBS and audio passes through unchanged |
| 2 | Low-shelf + punch + HPF + output gain, with smoothing | Frequency-response tests pass and you can hear bass added |
| 3 | Limiter + dry/wet + bypass | No clipping at max boost |
| 4 | Harmonic synth + sub-octave synth | Restoration tests pass; your real problem audio sounds full |
| 5 | Presets, grouped UI, tooltips, localization | UI is polished and presets work |
| 6 | CI on 3 OSes, installers, docs, v1.0.0 release | One-click install from the GitHub Release |
