/*
Bass Booster for OBS
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

// Bass *synthesis*. An equaliser can only amplify energy that is already in
// the signal; when the input has had its low end removed entirely (Bluetooth
// hands-free audio, VoIP, laptop microphones, an over-eager low-cut), these
// two generators create new low-frequency content from what survived.

#pragma once

#include "biquad.hpp"
#include "utils.hpp"

#include <cmath>

namespace bassbooster {
namespace dsp {

// HarmonicRestorer — regenerates a missing fundamental.
//
// A voice or instrument with fundamental f0 also carries harmonics at 2f0,
// 3f0, 4f0... When f0 is filtered away, those harmonics remain. Passing the
// surviving low-mid band through an even-order non-linearity (a full-wave
// rectifier) produces their *difference* frequencies: (n+1)f0 - n f0 = f0.
// In other words, the missing fundamental is rebuilt, at the right pitch,
// from the harmonics that are still present. This is the same idea used in
// low-frequency bandwidth extension for telephone speech.
//
// A second, odd-order (tanh) non-linearity, scaled by Drive, adds
// harmonic "weight" that makes bass more audible on small speakers.
//
//   x ─► band-pass [low..high] ─► rectifier + saturation ─► band-limit ─► out
//
// The saturation runs on an envelope-normalised signal, so its tone does not
// change with input level, and the output is scaled back by that envelope.
class HarmonicRestorer {
public:
	void prepare(double sampleRate)
	{
		fs = sampleRate;
		envelope.setup(fs, 0.0, 60.0);
		reset();
	}

	void setup(double lowHz, double highHz, double drive01)
	{
		bandHp.setup(Butterworth4::Type::HighPass, fs, lowHz);
		bandLp.setup(Butterworth4::Type::LowPass, fs, highHz);
		// Remove the rectifier's DC and sub-audio envelope thumps, but keep
		// regenerated fundamentals that sit well below the analysis band.
		double hp = lowHz * 0.25;
		hp = hp < 20.0 ? 20.0 : (hp > 60.0 ? 60.0 : hp);
		postHp.setup(Butterworth4::Type::HighPass, fs, hp);
		postLp.setup(Butterworth4::Type::LowPass, fs, highHz);
		drive = drive01 < 0.0 ? 0.0 : (drive01 > 1.0 ? 1.0 : drive01);
		k = 1.0 + 6.0 * drive;
		invTanhK = 1.0 / std::tanh(k);
	}

	void reset()
	{
		bandHp.reset();
		bandLp.reset();
		postHp.reset();
		postLp.reset();
		envelope.reset();
	}

	inline double process(double x)
	{
		const double band = bandLp.process(bandHp.process(x));
		const double env = envelope.process(band);

		double generated = 0.0;
		if (env > 1e-9) {
			const double n = band / env; // normalised to [-1, 1]
			const double even = std::fabs(band);
			const double odd = env * (std::tanh(k * n) * invTanhK - n);
			generated = (1.0 + 0.5 * drive) * even + 1.5 * drive * odd;
		}
		return postLp.process(postHp.process(generated));
	}

	void flushDenormals()
	{
		bandHp.flushDenormals();
		bandLp.flushDenormals();
		postHp.flushDenormals();
		postLp.flushDenormals();
	}

	bool stateIsFinite() const
	{
		return bandHp.stateIsFinite() && bandLp.stateIsFinite() && postHp.stateIsFinite() &&
		       postLp.stateIsFinite();
	}

private:
	double fs = 48000.0;
	double drive = 0.4;
	double k = 3.4;
	double invTanhK = 1.0;
	Butterworth4 bandHp;
	Butterworth4 bandLp;
	Butterworth4 postHp;
	Butterworth4 postLp;
	EnvelopeFollower envelope;
};

// SubOctaveGenerator — creates real bass one octave below the input.
//
// The classic analogue "subharmonic synthesiser" (dbx 120 family):
//   1. isolate a detection band just above the target frequency,
//   2. a Schmitt trigger (zero-crossing detector with hysteresis that tracks
//      the signal envelope, so noise cannot chatter it) toggles a flip-flop
//      on every positive-going crossing, giving a square wave at f/2,
//   3. the square wave is amplitude-modulated by the band's envelope, so the
//      new bass follows the dynamics of the source,
//   4. a steep low-pass turns the square into a smooth sine-like tone and
//      confines it below roughly 1.5x the target frequency.
// Tightness sets the envelope release: higher = shorter, punchier sub.
class SubOctaveGenerator {
public:
	void prepare(double sampleRate)
	{
		fs = sampleRate;
		reset();
	}

	void setup(double targetHz, double tightness01)
	{
		const double t = tightness01 < 0.0 ? 0.0 : (tightness01 > 1.0 ? 1.0 : tightness01);
		detHp.setCoeffs(BiquadCoeffs::highPass(fs, targetHz * 1.2));
		detLp.setup(Butterworth4::Type::LowPass, fs, targetHz * 3.0);
		outLp.setup(Butterworth4::Type::LowPass, fs, targetHz * 1.5);
		const double releaseMs = 250.0 - 220.0 * t; // 250 ms (loose) .. 30 ms (tight)
		envelope.setup(fs, 2.0, releaseMs);
	}

	void reset()
	{
		detHp.reset();
		detLp.reset();
		outLp.reset();
		envelope.reset();
		armed = false;
		polarity = 1.0;
	}

	inline double process(double x)
	{
		const double det = detLp.process(detHp.process(x));
		const double env = envelope.process(det);

		// Hysteresis at 20% of the envelope; the absolute floor (~-100 dBFS)
		// keeps the trigger from toggling on numerical noise in silence.
		const double threshold = 0.2 * env + 1e-5;
		if (det < -threshold) {
			armed = true;
		} else if (armed && det > threshold) {
			armed = false;
			polarity = -polarity; // toggles once per input cycle -> f/2
		}
		return outLp.process(polarity * env);
	}

	void flushDenormals()
	{
		detHp.flushDenormals();
		detLp.flushDenormals();
		outLp.flushDenormals();
	}

	bool stateIsFinite() const { return detHp.stateIsFinite() && detLp.stateIsFinite() && outLp.stateIsFinite(); }

private:
	double fs = 48000.0;
	Biquad detHp;
	Butterworth4 detLp;
	Butterworth4 outLp;
	EnvelopeFollower envelope;
	bool armed = false;
	double polarity = 1.0;
};

// Zero-latency, channel-linked peak limiter.
//
// Attack is instantaneous, so the output can never exceed the ceiling. A
// hold stage (20 ms, longer than one cycle of 50 Hz) stops the gain from
// recovering within a single bass waveform, which would otherwise cause
// audible distortion, before the exponential release takes over. No
// look-ahead is used, so audio/video sync in OBS is never affected.
class PeakLimiter {
public:
	void prepare(double sampleRate)
	{
		fs = sampleRate;
		holdSamples = static_cast<int>(0.020 * fs);
		reset();
	}

	void setup(bool isEnabled, double ceilingDb, double releaseMs)
	{
		enabled = isEnabled;
		ceiling = dbToGain(ceilingDb);
		release = onePoleCoeff(fs, releaseMs);
	}

	void reset()
	{
		gain = 1.0;
		holdCounter = 0;
	}

	// Returns the gain to apply for a frame whose loudest channel is `peak`.
	inline double process(double peak)
	{
		double desired = 1.0;
		if (enabled && peak > ceiling)
			desired = ceiling / peak;

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

	bool isEnabled() const { return enabled; }
	double getCeiling() const { return ceiling; }
	double currentGain() const { return gain; }

private:
	double fs = 48000.0;
	bool enabled = true;
	double ceiling = 1.0;
	double release = 0.0;
	double gain = 1.0;
	int holdSamples = 960;
	int holdCounter = 0;
};

} // namespace dsp
} // namespace bassbooster
