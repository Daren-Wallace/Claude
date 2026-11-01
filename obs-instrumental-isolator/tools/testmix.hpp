/*
Instrumental Isolator for OBS - synthetic test mix with known ground truth
Copyright (C) 2026 Daren Wallace
SPDX-License-Identifier: GPL-2.0-or-later

Every stem occupies its own set of frequencies, so after processing the level
of each stem can be measured exactly by evaluating the output at that stem's
partials. Used by the unit tests, the render tool's --report and the plots.

  vocal   f0 = 220 Hz, harmonics 1..20, vowel formants, dead centre
  guitar  310 Hz partials, panned left  (L 1.0 / R 0.25)
  keys    470 Hz partials, panned right (L 0.25 / R 1.0)
  pad     decorrelated stereo: 583 Hz partials left, 587 Hz partials right
  bass    82.4 Hz sine, centre
  kick    56 Hz decaying sine every 0.5 s, centre
*/

#pragma once

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace testmix {

constexpr double kPi = 3.14159265358979323846;

struct Stereo {
	std::vector<float> left, right;
	explicit Stereo(size_t n = 0) : left(n, 0.0f), right(n, 0.0f) {}
	void add(const Stereo &o)
	{
		for (size_t i = 0; i < left.size(); ++i) {
			left[i] += o.left[i];
			right[i] += o.right[i];
		}
	}
};

struct Stem {
	std::string name;
	std::vector<double> freqs; // partial frequencies used to measure it
	double panLeft = 1.0;      // channel gains (for reference)
	double panRight = 1.0;
	bool decorrelated = false; // pad: separate frequencies per channel
	std::vector<double> freqsRight;
};

inline double formant(double f)
{
	return 1.0 / (1.0 + std::pow((f - 700.0) / 300.0, 2)) + 0.7 / (1.0 + std::pow((f - 1200.0) / 350.0, 2)) +
	       0.4 / (1.0 + std::pow((f - 2600.0) / 500.0, 2)) + 0.3;
}

struct Mix {
	double sampleRate = 48000.0;
	Stereo mix;
	Stereo vocal, guitar, keys, pad, bass, kick;
	Stem vocalStem, guitarStem, keysStem, padStem, bassStem, kickStem;

	Stereo instrumental() const
	{
		Stereo s(mix.left.size());
		for (const Stereo *p : {&guitar, &keys, &pad, &bass, &kick})
			s.add(*p);
		return s;
	}
};

// `vibrato` adds +-1 % pitch modulation to the vocal (realistic but makes
// exact per-partial measurement fuzzier); `vocalGain` scales the vocal.
inline Mix make(double fs, double seconds, bool vibrato = false, double vocalGain = 1.0)
{
	Mix m;
	m.sampleRate = fs;
	const size_t n = static_cast<size_t>(fs * seconds);
	m.mix = Stereo(n);
	m.vocal = m.guitar = m.keys = m.pad = m.bass = m.kick = Stereo(n);

	m.vocalStem.name = "vocal";
	for (int k = 1; k <= 20; ++k)
		m.vocalStem.freqs.push_back(220.0 * k);
	m.guitarStem = {"guitar", {}, 1.0, 0.25, false, {}};
	for (int k = 1; k <= 12; ++k)
		m.guitarStem.freqs.push_back(310.0 * k);
	m.keysStem = {"keys", {}, 0.25, 1.0, false, {}};
	for (int k = 1; k <= 8; ++k)
		m.keysStem.freqs.push_back(470.0 * k);
	m.padStem = {"pad", {}, 1.0, 1.0, true, {}};
	for (int k = 1; k <= 6; ++k) {
		m.padStem.freqs.push_back(583.0 * k);
		m.padStem.freqsRight.push_back(587.0 * k);
	}
	m.bassStem = {"bass", {82.4}, 1.0, 1.0, false, {}};
	m.kickStem = {"kick", {56.0}, 1.0, 1.0, false, {}};

	double vphase = 0.0;
	for (size_t i = 0; i < n; ++i) {
		const double t = static_cast<double>(i) / fs;

		// Vocal (centre).
		const double f0 = 220.0 * (vibrato ? (1.0 + 0.01 * std::sin(2.0 * kPi * 5.5 * t)) : 1.0);
		vphase += 2.0 * kPi * f0 / fs;
		double v = 0.0;
		for (int k = 1; k <= 20; ++k)
			v += formant(220.0 * k) / std::sqrt(static_cast<double>(k)) * std::sin(k * vphase + 0.37 * k);
		v *= 0.035 * vocalGain;
		m.vocal.left[i] = m.vocal.right[i] = static_cast<float>(v);

		// Guitar (left) and keys (right).
		double g = 0.0;
		for (int k = 1; k <= 12; ++k)
			g += std::sin(2.0 * kPi * 310.0 * k * t + 0.9 * k) / k;
		g *= 0.08;
		m.guitar.left[i] = static_cast<float>(g * 1.0);
		m.guitar.right[i] = static_cast<float>(g * 0.25);

		double kb = 0.0;
		for (int k = 1; k <= 8; ++k)
			kb += std::sin(2.0 * kPi * 470.0 * k * t + 1.3 * k) / k;
		kb *= 0.07;
		m.keys.left[i] = static_cast<float>(kb * 0.25);
		m.keys.right[i] = static_cast<float>(kb * 1.0);

		// Pad: different partials per side, i.e. decorrelated stereo.
		double pl = 0.0, pr = 0.0;
		for (int k = 1; k <= 6; ++k) {
			pl += std::sin(2.0 * kPi * 583.0 * k * t + 0.5 * k) / k;
			pr += std::sin(2.0 * kPi * 587.0 * k * t + 2.1 * k) / k;
		}
		m.pad.left[i] = static_cast<float>(0.04 * pl);
		m.pad.right[i] = static_cast<float>(0.04 * pr);

		// Bass and kick (centre, below the default protection frequency).
		const double b = 0.12 * std::sin(2.0 * kPi * 82.4 * t);
		m.bass.left[i] = m.bass.right[i] = static_cast<float>(b);
		const double tk = std::fmod(t, 0.5);
		const double kk = 0.3 * std::exp(-tk * 12.0) * std::sin(2.0 * kPi * 56.0 * tk);
		m.kick.left[i] = m.kick.right[i] = static_cast<float>(kk);
	}

	for (const Stereo *p : {&m.vocal, &m.guitar, &m.keys, &m.pad, &m.bass, &m.kick})
		m.mix.add(*p);
	return m;
}

// Amplitude of the component at `freq` (Hann-windowed single-bin DFT).
inline double toneAmplitude(const std::vector<float> &v, double freq, double fs, size_t start, size_t len)
{
	double re = 0.0, im = 0.0, wsum = 0.0;
	for (size_t i = 0; i < len; ++i) {
		const double w =
			0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(len - 1));
		const double ph = 2.0 * kPi * freq * static_cast<double>(i) / fs;
		re += v[start + i] * w * std::cos(ph);
		im -= v[start + i] * w * std::sin(ph);
		wsum += w;
	}
	return 2.0 * std::sqrt(re * re + im * im) / wsum;
}

// Total power of a set of partials in one channel.
inline double partialPower(const std::vector<float> &v, const std::vector<double> &freqs, double fs, size_t start,
			   size_t len)
{
	double p = 0.0;
	for (double f : freqs) {
		if (f >= fs * 0.45)
			continue;
		const double a = toneAmplitude(v, f, fs, start, len);
		p += a * a;
	}
	return p;
}

inline double db(double power)
{
	return 10.0 * std::log10(power > 1e-30 ? power : 1e-30);
}

// Level change (dB) of a stem between input and output, measured in the
// given channel ("L", "R" or "LR" = both channels summed in power).
inline double stemChangeDb(const Stem &stem, const Stereo &in, const Stereo &out, double fs, size_t start, size_t len,
			   const std::string &channel = "LR")
{
	double pin = 0.0, pout = 0.0;
	const std::vector<double> &fr = stem.decorrelated ? stem.freqsRight : stem.freqs;
	if (channel != "R") {
		pin += partialPower(in.left, stem.freqs, fs, start, len);
		pout += partialPower(out.left, stem.freqs, fs, start, len);
	}
	if (channel != "L") {
		pin += partialPower(in.right, fr, fs, start, len);
		pout += partialPower(out.right, fr, fs, start, len);
	}
	return db(pout) - db(pin);
}

} // namespace testmix
