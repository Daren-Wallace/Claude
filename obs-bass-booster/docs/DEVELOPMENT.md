# Bass Booster for OBS: Developer Notes

## Layout

```
obs-bass-booster/
├── CMakeLists.txt, CMakePresets.json, buildspec.json, cmake/   official obs-plugintemplate build system
├── src/
│   ├── plugin-main.c        module entry point (OBS_DECLARE_MODULE)
│   ├── bass-filter.cpp/.h   obs_source_info, properties UI, UI→audio thread hand-off
│   └── dsp/                 pure C++17, no libobs dependency
│       ├── biquad.hpp         RBJ cookbook biquads (TDF-II, double precision), 4th-order Butterworth
│       ├── utils.hpp          smoothers, envelope follower, FTZ/DAZ guard
│       ├── generators.hpp     HarmonicRestorer, SubOctaveGenerator, PeakLimiter
│       ├── bass_engine.*      the complete processing chain
│       ├── params.*           parameter struct, ranges, sanitize(), factory presets
│       └── param_fields.hpp   settings-key ↔ struct-field table (shared by plugin and tool)
├── data/locale/en-US.ini    UI strings and tooltips
├── tests/                   Catch2 unit tests + libobs end-to-end test
├── tools/                   bass-booster-render (offline engine) + WAV I/O
├── scripts/make_plots.py    regenerates docs/images/*.png from the engine
└── docs/                    user guide, these notes, images
```

## Signal flow

```
            ┌─────────────── dry (untouched input) ───────────────────────┐
            │                                                             ▼
 in ─► trim ┼─► low-shelf ─► punch ─►(+)─► rumble HPF ─► mix ─► out gain ─► limiter ─► bypass xfade ─► out
            │                         ▲
            └─► mono sum ─┬─► harmonic restorer ─┤
                          └─► sub-octave gen ────┘
```

- **Per channel:** the shelf, punch and rumble filters.
- **Once, on the mono sum:** both generators. Their output is added to every channel, so the synthesised bass is phase-coherent across speakers.
- **Channel-linked:** the limiter. It is the last gain stage, which is why it can *guarantee* the ceiling.
- **Mix** blends with the raw input, not the trimmed one. So Mix = 0 % with the limiter off is bit-exact.

## DSP details

| Block | Implementation |
|---|---|
| Biquads | RBJ Audio-EQ-Cookbook coefficients. Transposed Direct Form II with **double** state, because poles at 20 Hz / 96 kHz sit very close to the unit circle. Frequencies are clamped to (1 Hz, 0.49·fs) and Q to [0.05, 40], so every reachable coefficient set is stable (tested exhaustively). |
| Smoothing | Gains (trim, output, mix, generator amounts) use one-pole smoothers per sample (τ = 15 ms). Filter frequency, gain and Q are smoothed at block rate, with coefficients recomputed every 32 samples while moving. Disabling Punch or a generator glides its gain or amount to zero. |
| Bypass | A 20 ms linear crossfade. Once fully bypassed, `process()` returns immediately, leaving the buffer bit-identical, and the state is reset on re-engage. |
| Harmonic restorer | 4th-order Butterworth band-pass [low, high], then `|x|` (even order: difference tones rebuild the missing fundamental) plus envelope-normalised `tanh` odd harmonics (Drive). Then a 4th-order HP at clamp(low/4, 20, 60) Hz and a 4th-order LP at *high*. Full scale = 2× at 100 %. |
| Sub-octave | Detection band: 2nd-order HP at 1.2·f and 4th-order LP at 3·f. A Schmitt trigger with 20 % envelope hysteresis and a −100 dBFS absolute floor toggles a flip-flop, giving f/2. The square wave × envelope goes through a 4th-order LP at 1.5·f. Release runs from 250 ms down to 30 ms as Tightness increases. |
| Limiter | Instant attack on the max-abs across channels, 20 ms hold (longer than a 50 Hz cycle, to avoid modulating bass waveforms), exponential release, and a final clamp for float rounding. Zero look-ahead. |
| Robustness | FTZ/DAZ is enabled for the duration of `process()` (SSE MXCSR, or AArch64 FPCR). Filter states are flushed below 1e-30 once per buffer. A non-finite input frame is passed through untouched, and non-finite internal state triggers a reset (counted in `getRecoveryCount()`). |

## Threading

OBS calls `update()` on the UI thread and `filter_audio()` on the audio thread.
- `update()` converts settings to `BassParams`, stores them under a mutex and sets an atomic flag.
- The audio thread uses `try_lock()`, so it can never be blocked by the UI. If the lock is busy, it picks the change up on the next buffer, about 21 ms later.
- The sample rate and channel count are re-queried on every buffer, and the engine is re-prepared if they change.

## Presets and the UI

- Presets live in `src/dsp/params.cpp`. The UI never stores a preset's values separately.
- `presetModified` copies the chosen preset into settings, but only if they differ.
- Every slider and checkable group shares `paramModified`. It flips the list to *Custom* when the values stop matching the selected preset.

Both callbacks are idempotent. OBS fires them when the dialog opens, and they then change nothing. The end-to-end test asserts exactly this.

**To add a preset:**
1. Add a `PresetBuilder` entry to `kPresets` in `params.cpp`.
2. Add a `Preset.<Name>` string to `data/locale/en-US.ini`.
3. Update the preset count in `tests/test_params.cpp` and `tests/obs_integration_test.cpp`.
4. Run `scripts/make_plots.py`.

**To add a parameter:**
1. Add the field to `BassParams` and its range to `ranges::`.
2. Clamp it in `sanitize()` and compare it in `presetMatches()`.
3. Add a row to `param_fields.hpp`, plus locale strings `<Text>` and `<Text>.Tooltip`.
4. Add an `addSlider()` call in `bbProperties`.
5. The field-table tests will fail until the count is updated, which is deliberate.

**Never rename a settings key.** They are stored in users' scene collections.

## Building

| Target | Command |
|---|---|
| Linux plugin + tests | `cmake -S . -B build -G Ninja -DBASS_BOOSTER_ENABLE_TESTS=ON && cmake --build build && ctest --test-dir build` (needs `libobs-dev`, plus `xvfb` for the end-to-end test) |
| DSP only (no libobs) | `cmake -S . -B build-dsp -DBASS_BOOSTER_DSP_ONLY=ON -DBASS_BOOSTER_ENABLE_TESTS=ON` |
| Windows | `cmake --preset windows-x64 -DBASS_BOOSTER_ENABLE_TESTS=ON && cmake --build --preset windows-x64` |
| macOS | `cmake --preset macos -DBASS_BOOSTER_ENABLE_TESTS=ON && cmake --build --preset macos` |
| Render tool only | add `-DBASS_BOOSTER_BUILD_TOOLS=ON` |

The Windows and macOS presets download the OBS sources and prebuilt dependencies listed in `buildspec.json`.

## Tests

`ctest` runs two suites: `-L unit` (53 Catch2 test cases) and `-L integration` (the libobs end-to-end test, Linux only).

The end-to-end test (`tests/obs_integration_test.cpp`) loads the built module with `obs_open_module()`. It relies on one libobs behaviour: `obs_source_output_audio()` runs audio filters synchronously and then fires audio-capture callbacks with the filtered buffer. So the test is fully deterministic, with no timing loop and no GPU. libobs on Linux needs an X display, so the test runs under `xvfb-run` and reports SKIP (exit code 77) when no display exists.

CI (`.github/workflows/obs-bass-booster.yml`) runs these jobs:
- clang-format check
- Linux build with `-Werror`, all tests, `.deb` and portable tarball
- ASan/UBSan
- Windows MSVC build with `/WX` and unit tests, packaged as a zip
- macOS universal build with unit tests, packaged as a zip

Tagging `obs-bass-booster-vX.Y.Z` drafts a GitHub release containing every package and a `SHA256SUMS.txt`.

## Formatting

Use the template's `.clang-format` (clang-format 18 or newer):

```bash
clang-format -i src/*.c src/*.cpp src/*.h src/dsp/* tests/*.cpp tests/*.hpp tools/*.cpp tools/*.hpp
```
