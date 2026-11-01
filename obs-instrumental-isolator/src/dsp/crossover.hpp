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

// Zero-latency, perfectly recombining crossover filterbank.
//
// The signal is split into bands by a chain of 4th-order Linkwitz-Riley
// crossovers ("split off the lowest band, pass the rest up"). Each LR4
// low-pass + high-pass pair sums to a 2nd-order all-pass, so the bands
// recombine to a flat magnitude as long as every band is given the phase
// rotation of the crossovers it did *not* pass through.
//
// Instead of filtering every band through its own chain of compensating
// all-passes (N^2/2 filters), the Combiner evaluates the weighted sum with a
// Horner scheme: acc = AP_k(acc) + y_k. Because the all-passes are linear and
// time-invariant this is exact, and costs only N-1 all-passes, even while
// the per-band weights change every sample.
//
// Everything is IIR: no buffering, no look-ahead, zero samples of latency.

#pragma once

#include "biquad.hpp"

#include <cmath>

namespace isolator {
namespace dsp {

constexpr int kMaxCrossovers = 24;
constexpr int kMaxBands = kMaxCrossovers + 1;

// The fixed crossover frequencies used by the isolator: third-octave steps
// from 100 Hz to ~8 kHz (20 crossovers, 21 bands). Band edges never move;
// the user's protection ranges are applied as per-band weights instead, so
// changing them never disturbs filter state.
struct BandLayout {
	int numCrossovers = 0;
	double freqs[kMaxCrossovers] = {};

	// Two-thirds-octave steps from 100 Hz to ~6.4 kHz (10 crossovers,
	// 11 bands): the resolution used for ambience protection, where the
	// fine frequency resolution comes from the adaptive predictor instead.
	static BandLayout twoThirdOctave(double sampleRate)
	{
		BandLayout l;
		for (int k = 0; k < 10; ++k) {
			const double f = 100.0 * std::pow(2.0, 2.0 * k / 3.0);
			if (f > sampleRate * 0.45)
				break;
			l.freqs[l.numCrossovers++] = f;
		}
		return l;
	}

	static BandLayout thirdOctave(double sampleRate)
	{
		BandLayout l;
		for (int k = 0; k < 20; ++k) {
			const double f = 100.0 * std::pow(2.0, k / 3.0);
			if (f > sampleRate * 0.45)
				break;
			l.freqs[l.numCrossovers++] = f;
		}
		return l;
	}

	int numBands() const { return numCrossovers + 1; }

	double lowerEdge(int b) const { return (b <= 0 || numCrossovers == 0) ? 0.0 : freqs[b - 1]; }
	double upperEdge(int b, double sampleRate) const { return (b >= numCrossovers) ? sampleRate * 0.5 : freqs[b]; }

	// Geometric centre of band b (edges at 0 and Nyquist use one octave).
	double bandCentre(int b, double sampleRate) const
	{
		if (numCrossovers == 0)
			return 1000.0;
		if (b <= 0)
			return freqs[0] / 2.0;
		if (b >= numCrossovers)
			return std::fmin(freqs[numCrossovers - 1] * 2.0, sampleRate * 0.45);
		return std::sqrt(freqs[b - 1] * freqs[b]);
	}
};

// One 4th-order Linkwitz-Riley crossover: two cascaded Butterworth
// sections for each output.
class LR4Split {
public:
	void setup(double sampleRate, double freq)
	{
		const BiquadCoeffs lp = BiquadCoeffs::lowPass(sampleRate, freq, kButterworthQ);
		const BiquadCoeffs hp = BiquadCoeffs::highPass(sampleRate, freq, kButterworthQ);
		lp1.setCoeffs(lp);
		lp2.setCoeffs(lp);
		hp1.setCoeffs(hp);
		hp2.setCoeffs(hp);
	}

	void reset()
	{
		lp1.reset();
		lp2.reset();
		hp1.reset();
		hp2.reset();
	}

	inline void process(double x, double &low, double &high)
	{
		low = lp2.process(lp1.process(x));
		high = hp2.process(hp1.process(x));
	}

	void flushDenormals()
	{
		lp1.flushDenormals();
		lp2.flushDenormals();
		hp1.flushDenormals();
		hp2.flushDenormals();
	}

	bool stateIsFinite() const
	{
		return lp1.stateIsFinite() && lp2.stateIsFinite() && hp1.stateIsFinite() && hp2.stateIsFinite();
	}

private:
	Biquad lp1, lp2, hp1, hp2;
};

// Splits one signal into layout.numBands() bands.
class BandSplitter {
public:
	void prepare(double sampleRate, const BandLayout &layout)
	{
		n = layout.numCrossovers;
		for (int k = 0; k < n; ++k)
			splits[k].setup(sampleRate, layout.freqs[k]);
		reset();
	}

	void reset()
	{
		for (int k = 0; k < kMaxCrossovers; ++k)
			splits[k].reset();
	}

	// bands[] must hold n + 1 values.
	inline void process(double x, double *bands)
	{
		double rest = x;
		for (int k = 0; k < n; ++k) {
			double high;
			splits[k].process(rest, bands[k], high);
			rest = high;
		}
		bands[n] = rest;
	}

	void flushDenormals()
	{
		for (int k = 0; k < n; ++k)
			splits[k].flushDenormals();
	}

	bool stateIsFinite() const
	{
		for (int k = 0; k < n; ++k)
			if (!splits[k].stateIsFinite())
				return false;
		return true;
	}

private:
	int n = 0;
	LR4Split splits[kMaxCrossovers];
};

// Recombines (weighted) bands with exact phase compensation.
class BandCombiner {
public:
	void prepare(double sampleRate, const BandLayout &layout)
	{
		n = layout.numCrossovers;
		for (int k = 0; k < n; ++k)
			ap[k].setCoeffs(BiquadCoeffs::allPass(sampleRate, layout.freqs[k], kButterworthQ));
		reset();
	}

	void reset()
	{
		for (int k = 0; k < kMaxCrossovers; ++k)
			ap[k].reset();
	}

	// y[] holds n + 1 band values. Returns the phase-aligned sum, which for
	// unweighted bands equals AP_0 * AP_1 * ... * AP_{n-1} applied to the
	// original signal: a flat-magnitude all-pass of the input.
	inline double process(const double *y)
	{
		if (n == 0)
			return y[0];
		double acc = y[0];
		for (int k = 1; k < n; ++k)
			acc = ap[k].process(acc) + y[k];
		return acc + y[n];
	}

	void flushDenormals()
	{
		for (int k = 0; k < n; ++k)
			ap[k].flushDenormals();
	}

	bool stateIsFinite() const
	{
		for (int k = 0; k < n; ++k)
			if (!ap[k].stateIsFinite())
				return false;
		return true;
	}

private:
	int n = 0;
	Biquad ap[kMaxCrossovers];
};

// The all-pass a signal must pass through to stay phase-aligned with the
// recombined output of a BandSplitter + BandCombiner on the same layout.
class AllpassChain {
public:
	void prepare(double sampleRate, const BandLayout &layout)
	{
		n = layout.numCrossovers;
		for (int k = 0; k < n; ++k)
			ap[k].setCoeffs(BiquadCoeffs::allPass(sampleRate, layout.freqs[k], kButterworthQ));
		reset();
	}

	void reset()
	{
		for (int k = 0; k < kMaxCrossovers; ++k)
			ap[k].reset();
	}

	inline double process(double x)
	{
		for (int k = 0; k < n; ++k)
			x = ap[k].process(x);
		return x;
	}

	void flushDenormals()
	{
		for (int k = 0; k < n; ++k)
			ap[k].flushDenormals();
	}

	bool stateIsFinite() const
	{
		for (int k = 0; k < n; ++k)
			if (!ap[k].stateIsFinite())
				return false;
		return true;
	}

private:
	int n = 0;
	Biquad ap[kMaxCrossovers];
};

} // namespace dsp
} // namespace isolator
