/*
Bass Booster for OBS - test helpers
Copyright (C) 2026 Daren Wallace
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "dsp/bass_engine.hpp"
#include "dsp/biquad.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace testutil {

using bassbooster::BassEngine;
using bassbooster::BassParams;
using bassbooster::dsp::kPi;

// Planar multichannel buffer.
struct Buffer {
	int channels = 0;
	std::vector<std::vector<float>> data;

	Buffer(int ch, size_t frames) : channels(ch), data(static_cast<size_t>(ch), std::vector<float>(frames, 0.0f)) {}

	size_t frames() const { return data.empty() ? 0 : data[0].size(); }

	std::vector<float *> pointers()
	{
		std::vector<float *> p;
		for (auto &c : data)
			p.push_back(c.data());
		return p;
	}
};

inline std::vector<float> sine(double freq, double amplitude, double fs, size_t frames, double phase = 0.0)
{
	std::vector<float> v(frames);
	for (size_t i = 0; i < frames; ++i)
		v[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * freq * static_cast<double>(i) / fs + phase));
	return v;
}

// Sum of harmonics n*f0 for n in [firstHarmonic, lastHarmonic] with 1/n
// amplitudes: a "voice-like" tone whose fundamental is missing.
inline std::vector<float> missingFundamental(double f0, int firstHarmonic, int lastHarmonic, double amplitude,
					     double fs, size_t frames)
{
	std::vector<float> v(frames, 0.0f);
	double norm = 0.0;
	for (int n = firstHarmonic; n <= lastHarmonic; ++n)
		norm += 1.0 / n;
	for (size_t i = 0; i < frames; ++i) {
		double s = 0.0;
		for (int n = firstHarmonic; n <= lastHarmonic; ++n)
			s += std::sin(2.0 * kPi * f0 * n * static_cast<double>(i) / fs + 0.3 * n) / n;
		v[i] = static_cast<float>(amplitude * s / norm);
	}
	return v;
}

// Deterministic white noise (xorshift), uniform in [-amplitude, amplitude].
inline std::vector<float> noise(double amplitude, size_t frames, uint32_t seed = 12345)
{
	std::vector<float> v(frames);
	uint32_t s = seed ? seed : 1;
	for (size_t i = 0; i < frames; ++i) {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		v[i] = static_cast<float>(amplitude * (static_cast<double>(s) / 4294967295.0 * 2.0 - 1.0));
	}
	return v;
}

inline double rms(const std::vector<float> &v, size_t start = 0, size_t end = 0)
{
	if (end == 0 || end > v.size())
		end = v.size();
	double acc = 0.0;
	for (size_t i = start; i < end; ++i)
		acc += static_cast<double>(v[i]) * v[i];
	return std::sqrt(acc / static_cast<double>(end - start));
}

inline double peak(const std::vector<float> &v, size_t start = 0)
{
	double p = 0.0;
	for (size_t i = start; i < v.size(); ++i)
		p = std::fmax(p, std::fabs(static_cast<double>(v[i])));
	return p;
}

inline double toDb(double linear)
{
	return 20.0 * std::log10(linear > 1e-15 ? linear : 1e-15);
}

// Amplitude of the component at `freq` (Hann-windowed single-bin DFT,
// corrected for the window's coherent gain).
inline double toneAmplitude(const std::vector<float> &v, double freq, double fs, size_t start, size_t length)
{
	double re = 0.0, im = 0.0, wsum = 0.0;
	for (size_t i = 0; i < length; ++i) {
		const double w =
			0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(length - 1));
		const double x = v[start + i] * w;
		const double ph = 2.0 * kPi * freq * static_cast<double>(i) / fs;
		re += x * std::cos(ph);
		im -= x * std::sin(ph);
		wsum += w;
	}
	return 2.0 * std::sqrt(re * re + im * im) / wsum;
}

// Feed `buf` through `engine` in chunks of `chunk` frames (like OBS does).
inline void processInChunks(BassEngine &engine, Buffer &buf, size_t chunk = 1024)
{
	auto ptrs = buf.pointers();
	const size_t frames = buf.frames();
	for (size_t off = 0; off < frames; off += chunk) {
		const size_t n = (frames - off) < chunk ? (frames - off) : chunk;
		std::vector<float *> p(ptrs.size());
		for (size_t c = 0; c < ptrs.size(); ++c)
			p[c] = ptrs[c] + off;
		engine.process(p.data(), buf.channels, static_cast<int>(n));
	}
}

// Parameters with every non-linear / level-dependent stage disabled, so the
// engine is a pure linear filter.
inline BassParams linearOnly(BassParams p = BassParams{})
{
	p.harmonicsEnabled = false;
	p.subEnabled = false;
	p.limiterEnabled = false;
	return p;
}

// Steady-state gain (dB) of the engine for a sine at `freq`.
inline double measureGainDb(const BassParams &params, double freq, double fs = 48000.0, double amplitude = 0.05)
{
	BassEngine engine;
	engine.prepare(fs, 1);
	engine.setParams(params);
	engine.snapToTargets();
	const size_t frames = static_cast<size_t>(fs * 1.5);
	Buffer buf(1, frames);
	buf.data[0] = sine(freq, amplitude, fs, frames);
	const std::vector<float> in = buf.data[0];
	processInChunks(engine, buf);
	const size_t settle = static_cast<size_t>(fs * 0.75);
	return toDb(rms(buf.data[0], settle) / rms(in, settle));
}

} // namespace testutil
