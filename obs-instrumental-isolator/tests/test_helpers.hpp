/*
Instrumental Isolator for OBS - test helpers
Copyright (C) 2026 Daren Wallace
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "dsp/isolator_engine.hpp"
#include "testmix.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace testutil {

using isolator::IsolatorEngine;
using isolator::IsolatorParams;
using testmix::kPi;

inline std::vector<float> sine(double freq, double amplitude, double fs, size_t frames, double phase = 0.0)
{
	std::vector<float> v(frames);
	for (size_t i = 0; i < frames; ++i)
		v[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * freq * static_cast<double>(i) / fs + phase));
	return v;
}

// Harmonic "voice": f0 with harmonics 1..n at 1/sqrt(k) amplitude.
inline std::vector<float> voice(double f0, int harmonics, double amplitude, double fs, size_t frames)
{
	std::vector<float> v(frames, 0.0f);
	for (size_t i = 0; i < frames; ++i) {
		double s = 0.0;
		for (int k = 1; k <= harmonics; ++k) {
			if (f0 * k >= fs * 0.45)
				break;
			s += std::sin(2.0 * kPi * f0 * k * static_cast<double>(i) / fs + 0.37 * k) /
			     std::sqrt(double(k));
		}
		v[i] = static_cast<float>(amplitude * s / std::sqrt(static_cast<double>(harmonics)));
	}
	return v;
}

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

// Process a stereo buffer in OBS-sized chunks.
inline void processStereo(IsolatorEngine &e, testmix::Stereo &s, size_t chunk = 1024)
{
	const size_t n = s.left.size();
	for (size_t off = 0; off < n; off += chunk) {
		const size_t len = std::min(chunk, n - off);
		float *ch[2] = {s.left.data() + off, s.right.data() + off};
		e.process(ch, 2, static_cast<int>(len));
	}
}

// Engine configured from a preset, with level-dependent stages optionally
// disabled so measurements are not skewed by makeup gain.
inline IsolatorParams presetParams(const char *id, bool makeup = false)
{
	IsolatorParams p = isolator::findPreset(id)->params;
	p.autoMakeup = makeup;
	return p;
}

inline testmix::Stereo runPreset(const IsolatorParams &p, const testmix::Stereo &in, double fs = 48000.0)
{
	IsolatorEngine e;
	e.prepare(fs, 2);
	e.setParams(p);
	e.snapToTargets();
	testmix::Stereo out = in;
	processStereo(e, out);
	return out;
}

} // namespace testutil
