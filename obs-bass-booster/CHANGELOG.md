# Changelog

## 1.0.0 (2026-10-31)

First release.

- **Bass Booster** audio filter for OBS Studio 30 and newer, on Windows, macOS (universal) and Linux.
- Low-shelf bass boost and a switchable Punch bell EQ.
- **Harmonic Restoration**: rebuilds a missing fundamental from the surviving harmonics.
- **Sub-Octave Generator**: synthesises new bass one octave down.
- 24 dB/octave rumble filter, plus a zero-latency channel-linked peak limiter.
- Five factory presets plus Custom, with automatic switching to Custom when a control is edited.
- Click-free bypass and smoothing on every parameter. Zero added latency. Mono through 7.1, at any sample rate.
- `bass-booster-render`, an offline tool that runs the same engine on WAV files, with analysis and response export.
- 53 unit test cases, an end-to-end libobs test, and CI on all three platforms including sanitizers.
