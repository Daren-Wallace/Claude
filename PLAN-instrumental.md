# Instrumental Isolator: OBS Vocal-Removal Filter (Implementation Plan)

> **Status: implemented.** The finished plugin is in [`obs-instrumental-isolator/`](obs-instrumental-isolator/README.md).
> Where the build differs from this plan:
> - **Core engine.** Per-band centre statistics (engine A as planned) only reached −2 to −8 dB of vocal removal: instruments panned to opposite sides cancel each other's correlation inside a band. The core is now a 256-tap **adaptive side predictor** (NLMS) at about 190 Hz resolution, which reaches **−20 dB** with instruments within 1.5 dB. The filterbank is now 2/3-octave and handles ambience and bass/treble protection.
> - **No shared `common-dsp` library.** Each plugin stays a self-contained obs-plugintemplate project that builds from its own folder, so the small shared headers are copied instead.
> - **No Mix control.** It duplicated Strength; Strength above 50 % now over-subtracts.
> - **CPU** is about 4 % of one core rather than under 3 %. Most of it is the predictor, which is what delivers the separation.

**Goal:** an OBS audio filter that removes vocals in real time and leaves the instrumental, with **zero added latency**. Separation does not need to be perfect.

---

## 1. What "zero latency" allows, and what it rules out

| Approach | Latency | Verdict |
|---|---|---|
| AI stem separation (Demucs, Spleeter, MDX, etc.) | 0.1 s to several seconds. They analyse whole windows of audio, often including the *future*. | ❌ Breaks the zero-latency rule. It's also heavy on CPU and GPU. |
| FFT/spectral masking (STFT) | At least one FFT window, 20–90 ms | ❌ Breaks the rule. |
| **Causal IIR sub-band processing** (this plan) | **0 samples.** Each output sample depends only on the current and past input. | ✅ Chosen. |

**What zero latency means here:** like the Bass Booster, the output responds on the same sample as the input. The crossover filters shift phase slightly, as any EQ does, but nothing is buffered. Analysis such as correlation or pitch tracking runs on *past* audio only, and steers the filters a few milliseconds late. The worst case is that the first few milliseconds of a new vocal note slip through. That's an acceptable imperfection under the brief.

**The physics this relies on:** in almost all commercial stereo music, the **lead vocal is mixed dead centre** (identical in left and right). Most instruments are panned, stereo-widened or carry stereo reverb. So the vocal can be identified as **the part of each frequency band that is identical in both channels**, and removed without any look-ahead.

**Honest limits, which will be documented:**
- **Mono sources:** a mono mic, phone audio or mono capture has *everything* at centre, so centre removal does nothing. Only the pitch-tracking stage (engine B below) helps, and only partially.
- **Possible leaks:**
  - vocals with wide stereo reverb or doubling (the reverb tail leaks);
  - backing vocals panned to the sides (they stay);
  - centre-panned solo instruments, such as a sax solo (they get removed too).
- **Centre-panned instruments** such as kick, bass and snare are protected deliberately (section 3).

---

## 2. Signal flow

```
                ┌──────────────── dry L/R ──────────────────────────────────────────┐
                │                                                                   ▼
L,R ─► input ─► │ zero-latency crossover filterbank (Linkwitz-Riley 4th order, ~12 bands, sums flat)
                │      │ per band b:  L_b, R_b
                │      ▼
                │  [A] Centre-steered cancellation   L_b' = L_b − s·c_b·M_b ,  R_b' = R_b − s·c_b·M_b
                │      ▲            c_b = "how centred is this band right now" (0..1, causal estimate)
                │      │
                │  [B] Pitch-tracked harmonic comb (optional): notches the vocal's harmonics in the mid signal
                │  [C] Transient protection: briefly lowers s on drum hits so kick/snare punch survives
                │  [D] Bass & air protection: bands below ~120 Hz and above ~8 kHz pass untouched
                │      ▼
                └─► sum bands ─► stereo-width restore ─► level makeup ─► mix ─► output (Instrumental | Vocals-only)
```

### [A] Centre-steered sub-band cancellation (the core)
- **Filterbank:**
  - A tree of 4th-order Linkwitz-Riley crossovers with all-pass compensation. Bands sum back to a flat magnitude, so with Strength = 0 the output sounds identical to the input.
  - About 12 bands, one per ~2/3 octave, covering 120 Hz–8 kHz, which is the vocal range.
  - All IIR, so there's no added latency.
- **Per-band centre estimate,** causal, from smoothed statistics over 10–40 ms:
  - correlation `ρ = ⟨L·R⟩ / √(⟨L²⟩⟨R²⟩)`, where 1 means identical in both channels;
  - balance `β = 1 − |⟨L²⟩−⟨R²⟩| / (⟨L²⟩+⟨R²⟩)`, where 1 means equally loud in both;
  - centre fraction `c = clamp((ρ − ρ₀)/(1 − ρ₀), 0, 1) · β^k`. Here ρ₀ is the **Centre Width** control: it sets how close to dead centre something must be to count as "vocal".
- **Cancellation:** `L' = L − s·c·M`, `R' = R − s·c·M`, with `M = (L+R)/2` and s = **Strength**. This removes only the centred share of each band and leaves the side content (panned instruments, stereo reverb) in true stereo. It's better than the classic "L − R" karaoke trick, which leaves mono, phase-inverted audio with no bass.
- **Smoothing:** attack and release on `c` to prevent "warbling" (musical-noise style artefacts).

### [B] Pitch-tracked harmonic comb (optional, for extra suppression and some help on mono)
- **Pitch detection:** a causal pitch detector (a low-cost YIN or autocorrelation variant) on the band-limited mid signal (80–1000 Hz). It analyses the most recent ~25 ms of past audio and updates every 2–5 ms.
- **Voicing check:** a voicing/confidence measure, so the comb is only active while a confident, steady, voice-range pitch is present. This avoids notching chords and bass lines.
- **Harmonic notches:** a bank of IIR notch filters at f0, 2f0 … N·f0 (up to about 5 kHz), applied to the *mid* component only. Notch frequencies glide smoothly as pitch changes.
- **Latency:** the output path has none; only the pitch estimate trails by a few milliseconds.

### [C] Transient protection
A fast causal onset detector (spectral flux on the band envelopes) lowers cancellation for about 30–80 ms after a drum hit. This keeps centre-panned kick and snare punchy.

### [D] Range protection
- **Below Low Protect (default 120 Hz):** bass and kick pass through untouched.
- **Above High Protect (default 8 kHz):** cymbals and air pass through untouched.

### Output stage
- **Stereo-width restore:** a mild side boost. Removing the centre leaves a "hollow" image, so this optionally widens what remains.
- **Level makeup:** automatic loudness makeup, since removing vocals lowers overall level.
- **Mix:** dry/wet blend.
- **Output Mode:**
  - **Instrumental** (default);
  - **Vocals only**, the inverse (centre extracted), which comes almost for free;
  - **Bypass**, with a click-free crossfade.

---

## 3. Controls (OBS properties UI)

| Group | Control | Range (default) |
|---|---|---|
| — | Preset | Karaoke · Gentle · Aggressive · Live Stream Music Bed · Vocals Only · Custom |
| — | Output Mode | Instrumental / Vocals only (Instrumental) |
| — | Bypass | off |
| Removal | Strength | 0–100 % (85) |
| Removal | Centre Width (how centred counts as vocal) | narrow…wide (medium) |
| Removal | Response speed (smoothing) | 5–100 ms (20) |
| Protection | Protect Bass below | 40–300 Hz (120) |
| Protection | Protect Highs above | 4–16 kHz (8 k) |
| Protection | Transient Protection | 0–100 % (50) |
| Pitch Remover *(checkable)* | Amount / Max harmonics / Voice range (low–high Hz) | 0–100 % (50) / 4–40 (20) / 80–1000 Hz |
| Output | Stereo Width / Auto Makeup / Mix / Output Gain | 0–200 % (110) / on / 100 % / 0 dB |

The same patterns as Bass Booster apply:
- presets switch to *Custom* automatically when a control is changed;
- every control has a tooltip;
- every parameter is smoothed;
- settings move to the audio thread without blocking it;
- multichannel input: process the front L/R pair and pass the other channels through, with the centre channel muted in Instrumental mode (5.1/7.1 sources usually put dialogue and vocals there).

---

## 4. Project structure

This would be a new sibling folder, `obs-instrumental-isolator/`, built on the same official obs-plugintemplate, with the same layout as Bass Booster:

- `src/dsp/` (no libobs dependency):
  - `crossover_bank`
  - `centre_estimator`
  - `pitch_tracker`
  - `harmonic_comb`
  - `transient_detector`
  - `isolator_engine`
  - `params`
  - `param_fields`
- `src/isolator-filter.cpp`: the OBS glue, filter id `instrumental_isolator_filter`.
- `tools/isolator-render`: offline WAV processing plus a separation report.
- `tests/`: Catch2 unit tests plus the libobs end-to-end test.
- `docs/`: USER_GUIDE, DEVELOPMENT, charts, and real OBS screenshots.
- CI jobs added for Linux, Windows, macOS, sanitizers and format checks.

The shared biquad, smoother and envelope code from Bass Booster would move into a small `common-dsp/` library used by both plugins.

---

## 5. Testing: proving it works with numbers

**Test material.** Synthesised mixes with known ground truth:
- a "vocal" (a harmonic tone with vibrato and formants), panned dead centre;
- instruments: a guitar-like tone panned left, keys panned right, a stereo-reverb pad, a centre kick and a centre bass;
- optionally, real CC-licensed multitrack stems, to check on real music.

**Measurements:**

| Test | Pass criterion |
|---|---|
| **Zero latency** | An impulse in produces output on the same sample. Group delay at 1 kHz is under 0.1 ms. |
| **Vocal suppression** | The centred vocal is reduced by at least 15 dB (target 20+) at default settings. |
| **Instrument retention** | Panned instruments change by no more than 1.5 dB. The stereo pad keeps its width. |
| **Bass and kick protection** | Centre kick and bass below Low Protect change by no more than 0.5 dB. |
| **Transparency** | Strength 0 gives a flat magnitude response (filterbank sum). Bypass is bit-exact. |
| **Mono input** | Nothing breaks. The pitch comb gives a measurable 3–6 dB reduction on a steady voiced tone. |
| **Pitch tracker accuracy** | Within ±1 % on synthetic voice across 80–1000 Hz. Silent on chords and noise (no false voicing). |
| **Robustness** | Random parameter and sample-rate stress tests, silence in gives silence out, NaN recovery, chunk-size invariance, no clicks on parameter jumps. |
| **CPU** | Under 3 % of one core for stereo 48 kHz with every stage on. Under 1 % without the pitch comb. |
| **End to end in libobs** | The filter registers and its properties UI is correct. Audio through a real OBS source is measured. Zero leaks. |

The render tool prints a before/after **separation report**: vocal-band energy versus side energy, and a correlation-over-time plot.

---

## 6. Milestones

| # | Deliverable | Done when |
|---|---|---|
| 1 | Shared `common-dsp` library extracted; Bass Booster still passes all of its tests | Green CI |
| 2 | Zero-latency LR filterbank with all-pass compensation | Sums flat; impulse test passes |
| 3 | Centre estimator + steered cancellation + bass/air protection | Suppression ≥ 15 dB and retention ≤ 1.5 dB on the test mixes |
| 4 | Transient protection + stereo-width restore + auto makeup + Vocals-only mode | Kick/snare retention tests pass |
| 5 | Pitch tracker + harmonic comb | Accuracy and mono-reduction tests pass |
| 6 | OBS filter, presets, UI, locale, libobs end-to-end test | Screenshots taken in real OBS |
| 7 | Render tool, docs, charts, CI packaging for 3 OSes | Install packages built by CI |

---

## 7. Possible later addition (not part of this plugin, because it breaks zero latency)

An optional **"Quality mode"** using a small low-latency neural separation model (about 20–50 ms latency, CPU-only, ONNX Runtime). OBS can hide that delay by setting the source's *Sync Offset* to match. It would sit behind a clearly labelled switch, so the default stays truly zero-latency.
