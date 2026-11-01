/*
Instrumental Isolator for OBS
Copyright (C) 2026 Daren Wallace

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

// SidePredictor: an adaptive FIR filter (normalised LMS) that predicts the
// mid signal M from the current and past samples of the side signal S.
//
// Why this separates vocals: every panned instrument appears in both M and S
// with a fixed, frequency-dependent ratio set by its pan position, so its
// part of M is *predictable* from S. A centre-panned vocal is identical in
// left and right, so it has no side component at all and cannot be predicted.
// The filter converges to the per-frequency Wiener solution
// H(f) = S_MS(f) / S_SS(f), whose resolution (fs / taps) is far finer than
// any band split, so instruments at different pans and different
// frequencies are handled at the same time.
//
//     prediction P = w * S   (instrument part of M)
//     residual   R = M - P   (vocal + decorrelated ambience)
//
// The filter is causal and uses the current input sample, so it adds zero
// latency. This is Widrow's adaptive noise canceller, with S as the reference.

#pragma once

#include <cmath>

namespace isolator {
namespace dsp {

class SidePredictor {
public:
	static constexpr int kMaxTaps = 1024;

	void prepare(double sampleRate)
	{
		taps = static_cast<int>(std::lround(256.0 * sampleRate / 48000.0));
		taps = taps < 32 ? 32 : (taps > kMaxTaps ? kMaxTaps : taps);
		taps = (taps + 3) & ~3; // multiple of 4 for the unrolled loop
		reset();
	}

	void setStepSize(double mu) { step = mu; }

	void reset()
	{
		for (int i = 0; i < kMaxTaps; ++i)
			w[i] = 0.0f;
		for (int i = 0; i < 2 * kMaxTaps; ++i)
			line[i] = 0.0f;
		pos = 0;
		energy = 0.0;
		leakCounter = 0;
	}

	// Feed one (M, S) pair; returns the prediction of M.
	inline double process(double m, double s, bool adapt = true)
	{
		// Mirrored ring buffer: x[0] is the newest sample, x[k] = S[n-k],
		// always contiguous so the inner loops vectorise.
		const double old = static_cast<double>(line[pos]);
		energy += s * s - old * old;
		if (energy < 0.0)
			energy = 0.0;
		line[pos] = static_cast<float>(s);
		line[pos + taps] = static_cast<float>(s);
		const float *x = &line[pos];

		// Four independent accumulators let the CPU pipeline the multiply-
		// adds (taps is always a multiple of 4).
		float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
		for (int k = 0; k < taps; k += 4) {
			a0 += w[k] * x[k];
			a1 += w[k + 1] * x[k + 1];
			a2 += w[k + 2] * x[k + 2];
			a3 += w[k + 3] * x[k + 3];
		}
		const double p = static_cast<double>((a0 + a1) + (a2 + a3));

		if (adapt) {
			const double e = m - p;
			const float g = static_cast<float>(step * e / (energy + 1e-10 * taps));
			for (int k = 0; k < taps; ++k)
				w[k] += g * x[k];
		}

		pos = (pos == 0) ? taps - 1 : pos - 1;

		// Gentle leakage keeps the weights bounded on pathological input.
		if (++leakCounter >= 64) {
			leakCounter = 0;
			for (int k = 0; k < taps; ++k)
				w[k] *= 0.99999f;
		}
		return p;
	}

	int numTaps() const { return taps; }

	bool stateIsFinite() const
	{
		if (!std::isfinite(energy))
			return false;
		for (int k = 0; k < taps; ++k)
			if (!std::isfinite(w[k]))
				return false;
		return true;
	}

private:
	int taps = 256;
	double step = 0.003;
	// Single precision is ample for the weights and delay line (the output
	// is a float stream) and halves the memory traffic of the inner loops.
	alignas(32) float w[kMaxTaps] = {};
	alignas(32) float line[2 * kMaxTaps] = {};
	int pos = 0;
	double energy = 0.0;
	int leakCounter = 0;
};

} // namespace dsp
} // namespace isolator
