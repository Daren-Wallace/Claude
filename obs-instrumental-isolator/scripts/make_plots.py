#!/usr/bin/env python3
"""Regenerate the documentation chart from the real engine.

Usage (from obs-instrumental-isolator/):
    python3 scripts/make_plots.py <path-to-isolator-render> [out-dir]

Runs `isolator-render --report` for three presets on the ground-truth test
mix and plots how much each stem changes. Requires matplotlib.
"""

import os
import re
import subprocess
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

SERIES = ["#2a78d6", "#eb6834", "#1baf7a"]  # validated categorical slots 1-3
HATCH = ["", "//", ".."]                     # secondary, non-colour encoding
SURFACE, INK, MUTED, GRID = "#fcfcfb", "#1f1f1e", "#6b6a64", "#e4e3de"
PRESETS = [("gentle", "Gentle"), ("karaoke", "Karaoke (default)"), ("aggressive", "Aggressive")]
STEMS = [
    ("vocal", "Vocal\n(centre)"),
    ("guitar", "Guitar\n(panned left)"),
    ("keys", "Keys\n(panned right)"),
    ("pad", "Pad\n(wide stereo)"),
    ("bass", "Bass\n(centre)"),
    ("kick", "Kick\n(centre)"),
]


def report(render, preset):
    out = subprocess.run([render, "--report", "--preset", preset], check=True, capture_output=True, text=True).stdout
    steady = out.split("Vibrato vocal")[0]
    values = {}
    for line in steady.splitlines():
        m = re.match(r"\s+(\w+)\s+.+?\s+([+-]\d+\.\d) dB$", line)
        if m:
            values[m.group(1)] = float(m.group(2))
    return values


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    render = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "docs", "images")
    os.makedirs(out_dir, exist_ok=True)

    data = {pid: report(render, pid) for pid, _ in PRESETS}

    fig, ax = plt.subplots(figsize=(9, 5), dpi=150)
    fig.patch.set_facecolor(SURFACE)
    ax.set_facecolor(SURFACE)
    fig.subplots_adjust(left=0.09, right=0.98, top=0.82, bottom=0.17)
    width = 0.26
    for i, (pid, name) in enumerate(PRESETS):
        xs = [k + (i - 1) * (width + 0.02) for k in range(len(STEMS))]
        ys = [data[pid][key] for key, _ in STEMS]
        bars = ax.bar(xs, ys, width=width, color=SERIES[i], hatch=HATCH[i], edgecolor=SURFACE, linewidth=1.5,
                      label=name)
        # Selective direct labels: only the vocal group, which is the point.
        ax.annotate(f"{ys[0]:.0f} dB", xy=(xs[0], ys[0]), xytext=(0, -12), textcoords="offset points",
                    ha="center", fontsize=8.5, color=INK)
        del bars
    ax.axhline(0, color=MUTED, linewidth=0.9)
    ax.set_xticks(range(len(STEMS)))
    ax.set_xticklabels([label for _, label in STEMS], fontsize=9, color=INK)
    ax.set_ylabel("Level change after processing (dB)", color=MUTED, fontsize=9)
    ax.set_ylim(-28, 3)
    ax.grid(True, axis="y", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)
    ax.tick_params(colors=MUTED, labelsize=9)
    vocal = [data[pid]["vocal"] for pid, _ in PRESETS]
    title = f"The vocal drops by {-max(vocal):.0f}-{-min(vocal):.0f} dB; the instruments barely move"
    fig.text(0.09, 0.94, title, fontsize=14, color=INK, weight="bold")
    fig.text(0.09, 0.895,
             "Ground-truth test mix through the plugin's own engine, presets as shipped (auto level makeup on).",
             fontsize=9.5, color=MUTED)
    ax.legend(loc="lower right", frameon=False, fontsize=8.5, labelcolor=INK)
    path = os.path.join(out_dir, "separation.png")
    fig.savefig(path, facecolor=SURFACE)
    print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
