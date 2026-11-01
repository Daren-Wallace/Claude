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

// Causal analysis blocks. Every one of them looks only at *past* samples, so
// they steer the audio path without ever delaying it: the audio itself is
// never buffered.
//
//   PitchTracker      YIN pitch detector on a decimated copy of the signal
//   HarmonicComb      gliding bank of notches at f0, 2f0, 3f0...
//   TransientDetector fast/slow envelope onset detector for drum hits
//   PeakLimiter       zero-latency safety limiter (used with auto makeup)

#pragma once

#include "biquad.hpp"
#include "utils.hpp"

#include <cmath>

namespace isolator {
namespace dsp {

// ----------------------------------------------------------- PitchTracker --
//
// YIN (de Cheveigné & Kawahara, 2002) on the most recent ~25 ms of audio,
// re-evaluated every 8 ms. Runs at ~8 kHz after anti-alias filtering and
// decimation, which keeps the cost around 0.5 % of one core.
class PitchTracker {
public:
	static constexpr int kBufferSize = 2048; // decimated samples (power of two)
	static constexpr int kMaxLag = 512;
	// Confidence (1 - YIN aperiodicity) needed to call a frame voiced. The
	// tracker listens to the already-extracted centre signal, so it can be
	// more permissive than a general-purpose pitch detector.
	static constexpr double kVoicingThreshold = 0.7;

	void prepare(double sampleRate)
	{
		fs = sampleRate;
		decimation = static_cast<int>(std::lround(fs / 8000.0));
		if (decimation < 1)
			decimation = 1;
		fsd = fs / decimation;
		// Anti-alias for decimation, and focus on the voice's fundamental
		// region: upper partials of other content only lower YIN's confidence.
		antiAlias.setup(Butterworth4::Type::LowPass, fs, std::fmin(0.4 * fsd, 1500.0));
		window = static_cast<int>(0.025 * fsd);
		hop = static_cast<int>(0.008 * fsd);
		if (hop < 1)
			hop = 1;
		setRange(80.0, 1000.0);
		reset();
	}

	void setRange(double lowHz, double highHz)
	{
		minLag = static_cast<int>(std::floor(fsd / highHz));
		maxLag = static_cast<int>(std::ceil(fsd / lowHz));
		if (minLag < 2)
			minLag = 2;
		if (maxLag > kMaxLag - 2)
			maxLag = kMaxLag - 2;
		if (maxLag >= window)
			window = maxLag + 1;
		if (window + maxLag + 2 > kBufferSize)
			window = kBufferSize - maxLag - 2;
		rangeLow = lowHz;
		rangeHigh = highHz;
	}

	void reset()
	{
		antiAlias.reset();
		for (double &v : buffer)
			v = 0.0;
		writePos = 0;
		decimCounter = 0;
		hopCounter = 0;
		filled = 0;
		currentF0 = 0.0;
		currentConfidence = 0.0;
		isVoiced = false;
	}

	inline void process(double x)
	{
		const double y = antiAlias.process(x);
		if (++decimCounter < decimation)
			return;
		decimCounter = 0;
		buffer[writePos] = y;
		writePos = (writePos + 1) & (kBufferSize - 1);
		if (filled < kBufferSize)
			++filled;
		if (++hopCounter >= hop) {
			hopCounter = 0;
			analyse();
		}
	}

	double f0() const { return currentF0; }
	double confidence() const { return currentConfidence; }
	bool voiced() const { return isVoiced; }

	void flushDenormals() { antiAlias.flushDenormals(); }
	bool stateIsFinite() const { return antiAlias.stateIsFinite(); }

private:
	void analyse()
	{
		const int need = window + maxLag + 1;
		if (filled < need) {
			isVoiced = false;
			return;
		}
		// Copy the most recent `need` samples, oldest first.
		double *x = scratchX;
		const int start = (writePos - need + kBufferSize) & (kBufferSize - 1);
		double energy = 0.0;
		for (int i = 0; i < need; ++i) {
			x[i] = buffer[(start + i) & (kBufferSize - 1)];
			if (i < window)
				energy += x[i] * x[i];
		}
		const double rms = std::sqrt(energy / window);
		if (rms < 1e-4) { // below about -80 dBFS: nothing to track
			isVoiced = false;
			currentConfidence = 0.0;
			return;
		}

		// Difference function and its cumulative-mean normalisation.
		double *d = scratchD;
		d[0] = 1.0;
		double running = 0.0;
		for (int tau = 1; tau <= maxLag; ++tau) {
			double sum = 0.0;
			for (int j = 0; j < window; ++j) {
				const double diff = x[j] - x[j + tau];
				sum += diff * diff;
			}
			running += sum;
			d[tau] = running > 0.0 ? sum * tau / running : 1.0;
		}

		// First dip below the threshold, then walk to its local minimum.
		const double threshold = 0.2;
		int best = -1;
		for (int tau = minLag; tau <= maxLag; ++tau) {
			if (d[tau] < threshold) {
				while (tau + 1 <= maxLag && d[tau + 1] < d[tau])
					++tau;
				best = tau;
				break;
			}
		}
		if (best < 0) {
			best = minLag;
			for (int tau = minLag + 1; tau <= maxLag; ++tau)
				if (d[tau] < d[best])
					best = tau;
		}

		// Parabolic interpolation for sub-sample lag accuracy.
		double lag = best;
		if (best > minLag && best < maxLag) {
			const double a = d[best - 1], b = d[best], c = d[best + 1];
			const double den = a - 2.0 * b + c;
			if (std::fabs(den) > 1e-12)
				lag = best + 0.5 * (a - c) / den;
		}

		currentConfidence = 1.0 - d[best];
		const double f = fsd / lag;
		isVoiced = currentConfidence >= kVoicingThreshold && f >= rangeLow * 0.97 && f <= rangeHigh * 1.03;
		if (isVoiced)
			currentF0 = f;
	}

	double fs = 48000.0;
	double fsd = 12000.0;
	int decimation = 4;
	int window = 300;
	int hop = 60;
	int minLag = 12;
	int maxLag = 150;
	double rangeLow = 80.0;
	double rangeHigh = 1000.0;
	Butterworth4 antiAlias;
	double buffer[kBufferSize] = {};
	// Analysis scratch kept off the audio thread's stack.
	double scratchX[kBufferSize] = {};
	double scratchD[kMaxLag] = {};
	int writePos = 0;
	int decimCounter = 0;
	int hopCounter = 0;
	int filled = 0;
	double currentF0 = 0.0;
	double currentConfidence = 0.0;
	bool isVoiced = false;
};

// ----------------------------------------------------------- HarmonicComb --
//
// A cascade of notch filters at k * f0 (k = 1..N), limited to a frequency
// window. f0 glides smoothly between pitch estimates, and coefficients are
// refreshed once per sub-block, so pitch changes never click.
class HarmonicComb {
public:
	static constexpr int kMaxNotches = 40;

	void prepare(double sampleRate)
	{
		fs = sampleRate;
		f0.setTimeConstant(fs, 8.0);
		f0.snap(200.0);
		reset();
		refresh(true);
	}

	void setLimits(int maxHarmonics, double lowHz, double highHz)
	{
		harmonics = maxHarmonics < 1 ? 1 : (maxHarmonics > kMaxNotches ? kMaxNotches : maxHarmonics);
		lowLimit = lowHz;
		highLimit = std::fmin(highHz, fs * 0.45);
		dirty = true;
	}

	void setTargetF0(double hz)
	{
		if (hz > 0.0)
			f0.setTarget(hz);
	}

	void reset()
	{
		for (Biquad &n : notches)
			n.reset();
	}

	// Call once per sub-block of `n` samples.
	void refresh(bool force, int n = 16)
	{
		const double now = f0.advance(n);
		// Recompute while gliding, and once more when the glide lands exactly
		// on its target, so the notches always end up precisely in place.
		const bool moved = std::fabs(now - computedF0) >= 1e-4 * now;
		const bool landed = f0.settled() && now != computedF0;
		if (!force && !dirty && !moved && !landed)
			return;
		dirty = false;
		computedF0 = now;
		active = 0;
		for (int k = 1; k <= harmonics && active < kMaxNotches; ++k) {
			const double f = now * k;
			if (f > highLimit)
				break;
			if (f < lowLimit)
				continue;
			notches[active++].setCoeffs(BiquadCoeffs::notch(fs, f, 8.0));
		}
	}

	inline double process(double x)
	{
		for (int i = 0; i < active; ++i)
			x = notches[i].process(x);
		return x;
	}

	int activeNotches() const { return active; }
	double currentF0() const { return f0.value(); }

	void flushDenormals()
	{
		for (int i = 0; i < kMaxNotches; ++i)
			notches[i].flushDenormals();
	}

	bool stateIsFinite() const
	{
		for (int i = 0; i < kMaxNotches; ++i)
			if (!notches[i].stateIsFinite())
				return false;
		return true;
	}

private:
	double fs = 48000.0;
	Smoother f0;
	double computedF0 = 0.0; // f0 the current notch coefficients were built for
	int harmonics = 20;
	double lowLimit = 120.0;
	double highLimit = 8000.0;
	bool dirty = true;
	int active = 0;
	Biquad notches[kMaxNotches];
};

// ------------------------------------------------------ TransientDetector --
//
// Compares short-term (5 ms) and long-term (120 ms) *energy* of the
// high-passed mid signal. Steady tones, however spiky their waveform, keep
// the two equal; a sudden rise of more than ~6 dB marks a drum hit. The
// returned factor then dips to (1 - amount) and recovers over ~60 ms,
// briefly relaxing vocal removal so centre-panned kicks and snares keep
// their punch.
class TransientDetector {
public:
	void prepare(double sampleRate)
	{
		fs = sampleRate;
		hp.setCoeffs(BiquadCoeffs::highPass(fs, 150.0));
		fastCoeff = onePoleCoeff(fs, 5.0);
		slowCoeff = onePoleCoeff(fs, 120.0);
		recover = onePoleCoeff(fs, 60.0);
		reset();
	}

	void setAmount(double amount01) { amount = amount01 < 0.0 ? 0.0 : (amount01 > 1.0 ? 1.0 : amount01); }

	void reset()
	{
		hp.reset();
		fastPower = 0.0;
		slowPower = 0.0;
		factor = 1.0;
		armed = true;
		onsets = 0;
	}

	inline double process(double x)
	{
		const double h = hp.process(x);
		const double e = h * h;
		fastPower += (e - fastPower) * (1.0 - fastCoeff);
		slowPower += (e - slowPower) * (1.0 - slowCoeff);
		const bool rising = fastPower > 4.0 * slowPower && fastPower > 1e-7; // > +6 dB, above -70 dBFS
		if (rising && armed) {
			armed = false;
			++onsets;
		}
		if (!rising && fastPower < 2.0 * slowPower)
			armed = true; // re-arm once the hit has settled
		if (rising && amount > 0.0) {
			const double target = 1.0 - amount;
			if (target < factor)
				factor = target;
		}
		factor = 1.0 + (factor - 1.0) * recover;
		return factor;
	}

	unsigned long onsetCount() const { return onsets; }
	void flushDenormals()
	{
		hp.flushDenormals();
		if (fastPower < 1e-30)
			fastPower = 0.0;
		if (slowPower < 1e-30)
			slowPower = 0.0;
	}
	bool stateIsFinite() const
	{
		return hp.stateIsFinite() && std::isfinite(fastPower) && std::isfinite(slowPower);
	}

private:
	double fs = 48000.0;
	double amount = 0.5;
	double fastCoeff = 0.0, slowCoeff = 0.0, recover = 0.0;
	double fastPower = 0.0, slowPower = 0.0;
	double factor = 1.0;
	bool armed = true;
	unsigned long onsets = 0;
	Biquad hp;
};

// ------------------------------------------------------------ PeakLimiter --
// Zero-latency, channel-linked: instant attack, 20 ms hold, exponential
// release. Guarantees the output never exceeds the ceiling.
class PeakLimiter {
public:
	void prepare(double sampleRate, double ceilingDb, double releaseMs)
	{
		ceiling = dbToGain(ceilingDb);
		release = onePoleCoeff(sampleRate, releaseMs);
		holdSamples = static_cast<int>(0.020 * sampleRate);
		reset();
	}

	void reset()
	{
		gain = 1.0;
		holdCounter = 0;
	}

	inline double process(double peak)
	{
		const double desired = peak > ceiling ? ceiling / peak : 1.0;
		if (desired < gain) {
			gain = desired;
			holdCounter = holdSamples;
		} else if (holdCounter > 0) {
			--holdCounter;
		} else {
			gain = desired + (gain - desired) * release;
			if (gain > 0.999999 && desired >= 1.0)
				gain = 1.0;
		}
		return gain;
	}

	double getCeiling() const { return ceiling; }
	double currentGain() const { return gain; }

private:
	double ceiling = 1.0;
	double release = 0.0;
	double gain = 1.0;
	int holdSamples = 960;
	int holdCounter = 0;
};

} // namespace dsp
} // namespace isolator
