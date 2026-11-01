# Instrumental Isolator: Developer Notes

## Layout

```
obs-instrumental-isolator/
├── CMakeLists.txt, CMakePresets.json, buildspec.json, cmake/   official obs-plugintemplate build system
├── src/
│   ├── plugin-main.c          module entry point
│   ├── isolator-filter.cpp/.h obs_source_info, properties UI, lock-free UI→audio hand-off, speaker layouts
│   └── dsp/                   pure C++17, no libobs dependency
│       ├── predictor.hpp        SidePredictor: 256-tap NLMS predicting M from S
│       ├── crossover.hpp        LR4 BandSplitter, Horner BandCombiner, AllpassChain, band layouts
│       ├── analysis.hpp         PitchTracker (YIN), HarmonicComb, TransientDetector, PeakLimiter
│       ├── isolator_engine.*    the complete chain
│       ├── params.*             parameter struct, ranges, sanitize(), presets
│       ├── param_fields.hpp     settings key ↔ field table (shared by plugin and tool)
│       └── biquad.hpp, utils.hpp
├── data/locale/en-US.ini
├── tests/                     Catch2 unit tests + libobs end-to-end test
├── tools/                     isolator-render, WAV I/O, testmix.hpp (ground-truth test mix)
└── scripts/make_plots.py      regenerates docs/images/separation.png
```

## Signal flow

```
L,R ─► M = (L+R)/2, S = (L−R)/2
        ├─► SidePredictor:  P = w*S (panned instruments in M),  R = M − P (vocal + ambience)
        ├─► 2/3-octave LR4 split of R, S, P
        │     Pd_b = max(0, <S_b²> − <P_b²>)          ambience lower bound
        │     g_b  = (<R_b²> − α·Pd_b) / <R_b²>        vocal share of the residual
        │     r_b  = clamp(g_b · oversub) · scale · protect_b · transient · (1 − mono)
        │     vocal = Σ comp(r_b · R_b)                (Horner combiner)
        ├─► M_res = AP(M) − vocal  ─►  harmonic comb at the pitch tracked on `vocal`
        ▼
Instrumental: L = M_inst + w·AP(S),  R = M_inst − w·AP(S)      Vocals: L = R = vocal + comb part
        ─► makeup ─► gain ─► limiter (with makeup) ─► bypass crossfade
```

## Design decisions, and why

| Decision | Reason |
|---|---|
| **No FFT and no neural network** | Both need analysis windows: 20 ms to seconds of latency. The brief requires zero latency. |
| **NLMS predictor instead of per-band statistics** | The first prototype estimated centre content from per-band mid/side statistics. With two instruments panned to opposite sides in the same third-octave band, their correlations cancel, and it could only reach −2 to −8 dB of vocal removal. A 256-tap FIR (about 190 Hz resolution) converges to the per-frequency Wiener solution S_MS(f)/S_SS(f) and handles opposite pans at different frequencies: **−20 dB** vocal, with instruments within 1.5 dB. It's Widrow's adaptive noise canceller with S as the reference. |
| **Ambience bound `Pd ≥ <S²> − <P²>`** | Every panned source has \|side\| ≤ \|mid\|, so side power beyond what the prediction explains must be decorrelated ambience. A correlation-based estimate failed for the same opposite-pan reason. |
| **Horner combiner** | Exact phase compensation with N−1 all-passes instead of N²/2, valid while weights change every sample because the all-passes are LTI. Verified against the explicit form to 10⁻¹⁰. |
| **Protection uses band edges, not centres** | Using band centres let the LR4 skirts drag 82 Hz bass into the 100–159 Hz band's removal (−1.6 dB). With edges it's −0.2 dB. |
| **Energy-based transient detector** | A peak-follower version fired constantly on the vocal's spiky waveform, relaxing removal all the time: −10 dB instead of −20 dB. |
| **Mono detection** | Centre removal on mono or dual-mono input would delete everything. When the side signal is below −40 dB, centre removal fades out and the predictor freezes. |
| **Global 16-sample update grid** | Block-rate updates are aligned to absolute sample position, not to each call, so output is bit-identical for any host buffer size. |
| **Float NLMS with 4 accumulators** | 1.8 % instead of 2.8 % CPU for the predictor, with no measurable change in separation. |
| **Pitch tracker on the extracted vocal, at 8 kHz** | Tracking the centre estimate rather than the whole mix avoids locking onto instruments. Decimating keeps it at about 0.5 % CPU. |

## Threading

Same pattern as Bass Booster: `update()` stores parameters under a mutex and sets an atomic flag. The audio thread only ever `try_lock()`s. Sample rate, channel count and speaker layout are re-checked on every buffer.

## Tests

`ctest` runs two suites:
- `-L unit`: 47 Catch2 test cases, with separation measured on `tools/testmix.hpp`. Each stem sits on unique frequencies, so per-stem level changes are exact.
- `-L integration`: `tests/obs_integration_test.cpp`, which needs libobs and runs under `xvfb-run`.

The pass criteria in `test_engine.cpp` come straight from the plan:
- vocal reduced by at least 15 dB;
- panned instruments within ±1.5 dB;
- protected bass and kick within ±0.5 dB;
- zero latency;
- bit-exact Bypass;
- mono input never silenced;
- the pitch remover gives at least 3 dB on a mono sung note.

## Adding a preset or parameter

Same workflow as Bass Booster (see `obs-bass-booster/docs/DEVELOPMENT.md`):
- presets live in `params.cpp`;
- keys live in `param_fields.hpp`;
- strings live in `en-US.ini`;
- the counts asserted in `test_params.cpp` and the integration test must be updated.

Never rename a settings key.
