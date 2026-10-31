#!/usr/bin/env python3
"""Regenerate the documentation charts from the real DSP engine.

Usage (from obs-bass-booster/):
    python3 scripts/make_plots.py <path-to-bass-booster-render> [out-dir]

Every curve comes from bass-booster-render, which runs the exact engine used
inside OBS:
  * preset-response.png  - linear EQ response of each factory preset
  * restoration.png      - spectrum of a voice with its bass removed, before and
                           after the Restore preset, against the original voice
Requires numpy and matplotlib.
"""

import os
import struct
import subprocess
import sys
import tempfile

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

# Validated categorical palette (fixed order, never cycled).
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4"]
DASHES = ["-", (0, (6, 2)), (0, (2, 2)), (0, (8, 2, 2, 2)), (0, (1, 1))]
SURFACE = "#fcfcfb"
INK = "#1f1f1e"
MUTED = "#6b6a64"
GRID = "#e4e3de"

PRESETS = [
    ("subtle", "Subtle Warmth"),
    ("voice", "Voice Body"),
    ("music", "Music Boost"),
    ("restore", "Restore Missing Bass"),
    ("extreme", "Extreme"),
]


def style(ax, title, subtitle):
    ax.set_facecolor(SURFACE)
    ax.figure.set_facecolor(SURFACE)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)
    ax.tick_params(colors=MUTED, labelsize=9)
    ax.grid(True, which="major", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    ax.set_xscale("log")
    ticks = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]
    ax.set_xticks(ticks)
    ax.set_xticklabels(["20", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k"])
    ax.minorticks_off()
    ax.set_xlabel("Frequency (Hz)", color=MUTED, fontsize=9)
    ax.figure.text(0.07, 0.95, title, fontsize=14, color=INK, weight="bold", ha="left")
    ax.figure.text(0.07, 0.905, subtitle, fontsize=9.5, color=MUTED, ha="left")


def read_wav(path):
    """Minimal WAV reader for the float32 / PCM files bass-booster-render writes."""
    with open(path, "rb") as f:
        data = f.read()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE"
    pos, fmt, samples = 12, None, None
    while pos + 8 <= len(data):
        tag, size = data[pos : pos + 4], struct.unpack("<I", data[pos + 4 : pos + 8])[0]
        body = data[pos + 8 : pos + 8 + size]
        if tag == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif tag == b"data":
            samples = body
        pos += 8 + size + (size & 1)
    code, channels, rate, _, _, bits = fmt
    if code == 3:
        arr = np.frombuffer(samples, dtype="<f4")
    else:
        arr = np.frombuffer(samples, dtype="<i2").astype(np.float64) / 32768.0
    return rate, arr.reshape(-1, channels).mean(axis=1)


def spectrum_db(x, rate, nfft=16384):
    """Welch-averaged power spectrum in dBFS, smoothed to 1/12 octave."""
    win = np.hanning(nfft)
    hop = nfft // 2
    frames = [x[i : i + nfft] * win for i in range(0, len(x) - nfft, hop)]
    power = np.mean([np.abs(np.fft.rfft(fr)) ** 2 for fr in frames], axis=0)
    power /= np.sum(win) ** 2 / 4.0
    freqs = np.fft.rfftfreq(nfft, 1.0 / rate)
    centers = 20.0 * 2 ** (np.arange(0, 120) / 12.0)
    centers = centers[centers < rate / 2 * 0.9]
    out = []
    for c in centers:
        band = (freqs >= c * 2 ** (-1 / 24)) & (freqs < c * 2 ** (1 / 24))
        out.append(10 * np.log10(np.sum(power[band]) + 1e-20))
    return centers, np.array(out)


def run(render, *args):
    return subprocess.run([render, *args], check=True, capture_output=True, text=True).stdout


def plot_presets(render, out_dir):
    fig, ax = plt.subplots(figsize=(9, 5), dpi=150)
    fig.subplots_adjust(left=0.08, right=0.97, top=0.84, bottom=0.12)
    for i, (pid, name) in enumerate(PRESETS):
        csv = run(render, "--response", "--preset", pid)
        rows = [line.split(",") for line in csv.strip().splitlines()[1:]]
        f = np.array([float(r[0]) for r in rows])
        g = np.array([float(r[1]) for r in rows])
        ax.plot(f, g, color=SERIES[i], linestyle=DASHES[i], linewidth=2, label=name, solid_capstyle="round")
    ax.axhline(0, color=MUTED, linewidth=0.8)
    ax.set_xlim(10, 20000)
    ax.set_ylabel("Gain (dB)", color=MUTED, fontsize=9)
    style(ax, "Factory preset EQ curves",
          "Low-shelf + punch + rumble filter + output gain. The bass synthesis engines are not part of this curve.")
    # Five series: the legend plus a distinct dash pattern per curve carries
    # identity without relying on colour.
    ax.legend(loc="upper right", frameon=False, fontsize=8.5, labelcolor=INK, handlelength=3.2)
    path = os.path.join(out_dir, "preset-response.png")
    fig.savefig(path, facecolor=SURFACE)
    plt.close(fig)
    return path


def plot_restoration(render, out_dir):
    with tempfile.TemporaryDirectory() as tmp:
        full = os.path.join(tmp, "full.wav")
        phone = os.path.join(tmp, "phone.wav")
        restored = os.path.join(tmp, "restored.wav")
        run(render, "--generate", "full-voice", full)
        run(render, "--generate", "phone-voice", phone)
        run(render, "--quiet", "--preset", "restore", phone, restored)
        series = [
            ("Original voice (reference)", full),
            ("Bass removed (phone / Bluetooth)", phone),
            ("After Bass Booster — Restore preset", restored),
        ]
        fig, ax = plt.subplots(figsize=(9, 5), dpi=150)
        fig.subplots_adjust(left=0.08, right=0.97, top=0.84, bottom=0.12)
        for i, (name, path) in enumerate(series):
            rate, x = read_wav(path)
            f, db = spectrum_db(x, rate)
            ax.plot(f, db, color=SERIES[i], linestyle=DASHES[i], linewidth=2, label=name)
        ax.axvspan(20, 300, color=GRID, alpha=0.45, linewidth=0)
        ax.text(78, -112, "removed by the\nphone / Bluetooth link", fontsize=8.5, color=MUTED, ha="center")
        ax.set_xlim(20, 8000)
        ax.set_ylim(-120, -10)
        ax.set_ylabel("Level (dBFS per 1/12 octave)", color=MUTED, fontsize=9)
        style(ax, "Rebuilding bass that is completely gone",
              "A voice with everything below 300 Hz removed, processed by the plugin's own DSP engine.")
        ax.legend(loc="upper right", frameon=False, fontsize=8.5, labelcolor=INK)
        out = os.path.join(out_dir, "restoration.png")
        fig.savefig(out, facecolor=SURFACE)
        plt.close(fig)
        return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    render = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "docs", "images")
    os.makedirs(out_dir, exist_ok=True)
    print(plot_presets(render, out_dir))
    print(plot_restoration(render, out_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
