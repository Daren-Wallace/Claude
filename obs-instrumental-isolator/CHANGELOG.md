# Changelog

## 1.0.0 (2026-11-01)

First release.

- **Instrumental Isolator** audio filter for OBS Studio 30 and newer, on Windows, macOS (universal) and Linux.
- Zero-latency vocal removal using a 256-tap adaptive side predictor and a phase-compensated Linkwitz-Riley filterbank.
- Ambience protection, bass and treble protection, and energy-based drum-hit protection.
- Pitch-tracked harmonic vocal remover (YIN plus a gliding notch comb). Also helps on mono sources.
- Mono and dual-mono detection, so centre removal never silences mono audio.
- 5.1 / 7.1 support: the dedicated centre channel is removed in Instrumental mode.
- Vocals-only (acapella) output mode, auto level makeup with a zero-latency limiter, stereo width, and click-free bypass.
- Five presets plus Custom, with automatic switching to Custom when a control is edited.
- `isolator-render`: offline processing and a separation report on a ground-truth test mix.
- 47 unit test cases, an end-to-end libobs test with 150 checks, and CI on all three platforms including sanitizers.
