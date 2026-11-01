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

// Second-order IIR ("biquad") sections using the formulas from Robert
// Bristow-Johnson's "Audio EQ Cookbook". Coefficients and state are kept in
// double precision: low-frequency filters at 48-96 kHz place their poles very
// close to the unit circle, where single precision becomes noisy and can
// even go unstable.

#pragma once

#include <cmath>

namespace isolator {
namespace dsp {

constexpr double kPi = 3.14159265358979323846;
constexpr double kButterworthQ = 0.70710678118654752440;

// Q values for the two cascaded sections of a 4th-order Butterworth filter.
constexpr double kButterworth4Q1 = 0.54119610014619698440;
constexpr double kButterworth4Q2 = 1.30656296487637652786;

inline double dbToGain(double db)
{
	return std::pow(10.0, db / 20.0);
}

inline double gainToDb(double gain)
{
	return 20.0 * std::log10(gain);
}

struct BiquadCoeffs {
	double b0 = 1.0;
	double b1 = 0.0;
	double b2 = 0.0;
	double a1 = 0.0;
	double a2 = 0.0;

	// Unity pass-through.
	static BiquadCoeffs identity() { return BiquadCoeffs{}; }

	// Keep the centre frequency safely inside (0, Nyquist).
	static double clampFrequency(double sampleRate, double freq)
	{
		const double maxFreq = sampleRate * 0.49;
		if (!(freq > 1.0))
			return 1.0;
		return freq > maxFreq ? maxFreq : freq;
	}

	static double clampQ(double q) { return (q > 0.05) ? (q < 40.0 ? q : 40.0) : 0.05; }

	static BiquadCoeffs normalize(double b0, double b1, double b2, double a0, double a1, double a2)
	{
		BiquadCoeffs c;
		const double inv = 1.0 / a0;
		c.b0 = b0 * inv;
		c.b1 = b1 * inv;
		c.b2 = b2 * inv;
		c.a1 = a1 * inv;
		c.a2 = a2 * inv;
		return c;
	}

	static BiquadCoeffs lowPass(double sampleRate, double freq, double q = kButterworthQ)
	{
		const double w0 = 2.0 * kPi * clampFrequency(sampleRate, freq) / sampleRate;
		const double cw = std::cos(w0);
		const double alpha = std::sin(w0) / (2.0 * clampQ(q));
		return normalize((1.0 - cw) * 0.5, 1.0 - cw, (1.0 - cw) * 0.5, 1.0 + alpha, -2.0 * cw, 1.0 - alpha);
	}

	static BiquadCoeffs highPass(double sampleRate, double freq, double q = kButterworthQ)
	{
		const double w0 = 2.0 * kPi * clampFrequency(sampleRate, freq) / sampleRate;
		const double cw = std::cos(w0);
		const double alpha = std::sin(w0) / (2.0 * clampQ(q));
		return normalize((1.0 + cw) * 0.5, -(1.0 + cw), (1.0 + cw) * 0.5, 1.0 + alpha, -2.0 * cw, 1.0 - alpha);
	}

	// Constant 0 dB peak gain band-pass.
	static BiquadCoeffs bandPass(double sampleRate, double freq, double q)
	{
		const double w0 = 2.0 * kPi * clampFrequency(sampleRate, freq) / sampleRate;
		const double cw = std::cos(w0);
		const double alpha = std::sin(w0) / (2.0 * clampQ(q));
		return normalize(alpha, 0.0, -alpha, 1.0 + alpha, -2.0 * cw, 1.0 - alpha);
	}

	static BiquadCoeffs peaking(double sampleRate, double freq, double q, double gainDb)
	{
		const double A = std::pow(10.0, gainDb / 40.0);
		const double w0 = 2.0 * kPi * clampFrequency(sampleRate, freq) / sampleRate;
		const double cw = std::cos(w0);
		const double alpha = std::sin(w0) / (2.0 * clampQ(q));
		return normalize(1.0 + alpha * A, -2.0 * cw, 1.0 - alpha * A, 1.0 + alpha / A, -2.0 * cw,
				 1.0 - alpha / A);
	}

	// Low shelf. `q` follows the cookbook's Q form; 0.7071 gives the classic
	// maximally-flat shelf with no overshoot, higher values add a resonant
	// bump just above the corner frequency.
	static BiquadCoeffs lowShelf(double sampleRate, double freq, double q, double gainDb)
	{
		const double A = std::pow(10.0, gainDb / 40.0);
		const double w0 = 2.0 * kPi * clampFrequency(sampleRate, freq) / sampleRate;
		const double cw = std::cos(w0);
		const double alpha = std::sin(w0) / (2.0 * clampQ(q));
		const double sqA2alpha = 2.0 * std::sqrt(A) * alpha;
		return normalize(A * ((A + 1.0) - (A - 1.0) * cw + sqA2alpha), 2.0 * A * ((A - 1.0) - (A + 1.0) * cw),
				 A * ((A + 1.0) - (A - 1.0) * cw - sqA2alpha), (A + 1.0) + (A - 1.0) * cw + sqA2alpha,
				 -2.0 * ((A - 1.0) + (A + 1.0) * cw), (A + 1.0) + (A - 1.0) * cw - sqA2alpha);
	}

	// Second-order all-pass: unity magnitude, phase rotation centred on
	// `freq`. With q = 1/sqrt(2) it equals the sum of a 4th-order
	// Linkwitz-Riley low-pass and high-pass at the same frequency, which
	// is what lets a crossover tree be phase-compensated exactly.
	static BiquadCoeffs allPass(double sampleRate, double freq, double q = kButterworthQ)
	{
		const double w0 = 2.0 * kPi * clampFrequency(sampleRate, freq) / sampleRate;
		const double cw = std::cos(w0);
		const double alpha = std::sin(w0) / (2.0 * clampQ(q));
		return normalize(1.0 - alpha, -2.0 * cw, 1.0 + alpha, 1.0 + alpha, -2.0 * cw, 1.0 - alpha);
	}

	// Band-reject (notch) with infinite attenuation at `freq`.
	static BiquadCoeffs notch(double sampleRate, double freq, double q)
	{
		const double w0 = 2.0 * kPi * clampFrequency(sampleRate, freq) / sampleRate;
		const double cw = std::cos(w0);
		const double alpha = std::sin(w0) / (2.0 * clampQ(q));
		return normalize(1.0, -2.0 * cw, 1.0, 1.0 + alpha, -2.0 * cw, 1.0 - alpha);
	}

	// Magnitude response (linear) at `freq`, evaluated analytically. Used by
	// tests and by the response-curve export tool.
	double magnitudeAt(double sampleRate, double freq) const
	{
		const double w = 2.0 * kPi * freq / sampleRate;
		const double c1 = std::cos(w), s1 = std::sin(w);
		const double c2 = std::cos(2.0 * w), s2 = std::sin(2.0 * w);
		const double nr = b0 + b1 * c1 + b2 * c2;
		const double ni = -(b1 * s1 + b2 * s2);
		const double dr = 1.0 + a1 * c1 + a2 * c2;
		const double di = -(a1 * s1 + a2 * s2);
		return std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
	}

	// True when both poles are strictly inside the unit circle.
	bool isStable() const { return std::fabs(a2) < 1.0 && std::fabs(a1) < 1.0 + a2; }
};

// Transposed Direct Form II: the best-behaved structure for floating point
// with time-varying coefficients.
class Biquad {
public:
	void setCoeffs(const BiquadCoeffs &c) { coeffs = c; }
	const BiquadCoeffs &getCoeffs() const { return coeffs; }

	void reset()
	{
		z1 = 0.0;
		z2 = 0.0;
	}

	inline double process(double x)
	{
		const double y = coeffs.b0 * x + z1;
		z1 = coeffs.b1 * x - coeffs.a1 * y + z2;
		z2 = coeffs.b2 * x - coeffs.a2 * y;
		return y;
	}

	// Flush tiny state values to zero so long stretches of silence never
	// fall into the (very slow) denormal range on any CPU.
	void flushDenormals()
	{
		if (std::fabs(z1) < 1e-30)
			z1 = 0.0;
		if (std::fabs(z2) < 1e-30)
			z2 = 0.0;
	}

	bool stateIsFinite() const { return std::isfinite(z1) && std::isfinite(z2); }

private:
	BiquadCoeffs coeffs;
	double z1 = 0.0;
	double z2 = 0.0;
};

// Two cascaded biquads forming a 4th-order Butterworth low- or high-pass
// (24 dB/octave).
class Butterworth4 {
public:
	enum class Type { LowPass, HighPass };

	void setup(Type type, double sampleRate, double freq)
	{
		if (type == Type::LowPass) {
			s1.setCoeffs(BiquadCoeffs::lowPass(sampleRate, freq, kButterworth4Q1));
			s2.setCoeffs(BiquadCoeffs::lowPass(sampleRate, freq, kButterworth4Q2));
		} else {
			s1.setCoeffs(BiquadCoeffs::highPass(sampleRate, freq, kButterworth4Q1));
			s2.setCoeffs(BiquadCoeffs::highPass(sampleRate, freq, kButterworth4Q2));
		}
	}

	void reset()
	{
		s1.reset();
		s2.reset();
	}

	inline double process(double x) { return s2.process(s1.process(x)); }

	void flushDenormals()
	{
		s1.flushDenormals();
		s2.flushDenormals();
	}

	bool stateIsFinite() const { return s1.stateIsFinite() && s2.stateIsFinite(); }

	double magnitudeAt(double sampleRate, double freq) const
	{
		return s1.getCoeffs().magnitudeAt(sampleRate, freq) * s2.getCoeffs().magnitudeAt(sampleRate, freq);
	}

private:
	Biquad s1;
	Biquad s2;
};

} // namespace dsp
} // namespace isolator
