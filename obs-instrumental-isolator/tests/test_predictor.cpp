/*
Instrumental Isolator for OBS - adaptive side predictor tests
Copyright (C) 2026 Daren Wallace
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <catch2/catch_test_macros.hpp>

#include "dsp/predictor.hpp"
#include "test_helpers.hpp"

using namespace isolator::dsp;
using namespace testutil;

namespace {

// Run (M, S) through a fresh predictor; return the residual M - P.
std::vector<float> residual(const std::vector<float> &M, const std::vector<float> &S)
{
	SidePredictor p;
	p.prepare(48000.0);
	p.setStepSize(0.003);
	std::vector<float> r(M.size());
	for (size_t i = 0; i < M.size(); ++i)
		r[i] = static_cast<float>(M[i] - p.process(M[i], S[i]));
	return r;
}

} // namespace

TEST_CASE("Tap count scales with sample rate and is a multiple of 4", "[predictor]")
{
	for (double fs : {22050.0, 44100.0, 48000.0, 96000.0, 192000.0}) {
		SidePredictor p;
		p.prepare(fs);
		CHECK(p.numTaps() % 4 == 0);
		CHECK(p.numTaps() >= 32);
		CHECK(p.numTaps() <= SidePredictor::kMaxTaps);
	}
	SidePredictor p;
	p.prepare(48000.0);
	CHECK(p.numTaps() == 256);
}

TEST_CASE("A panned instrument is predicted out of the mid signal", "[predictor]")
{
	// Guitar-like tone panned left: L = g, R = 0.3 g.
	const auto g = voice(310.0, 10, 0.3, 48000.0, 96000);
	std::vector<float> M(g.size()), S(g.size());
	for (size_t i = 0; i < g.size(); ++i) {
		M[i] = 0.5f * (g[i] + 0.3f * g[i]);
		S[i] = 0.5f * (g[i] - 0.3f * g[i]);
	}
	const auto r = residual(M, S);
	CHECK(toDb(rms(r, 72000) / rms(M, 72000)) < -30.0);
}

TEST_CASE("Two instruments panned to opposite sides are both predicted", "[predictor]")
{
	// This is what defeats a per-band single-coefficient approach. The two
	// spectra interleave but stay further apart than the filter's resolution
	// (fs / taps = 187 Hz); partials closer than that are a known limit.
	const auto a = voice(310.0, 4, 0.3, 48000.0, 96000); // left:  310..1240 Hz
	const auto b = voice(523.0, 3, 0.3, 48000.0, 96000); // right: 523, 1046, 1569 Hz
	std::vector<float> M(a.size()), S(a.size());
	for (size_t i = 0; i < a.size(); ++i) {
		const float L = a[i] + 0.25f * b[i];
		const float R = 0.25f * a[i] + b[i];
		M[i] = 0.5f * (L + R);
		S[i] = 0.5f * (L - R);
	}
	const auto r = residual(M, S);
	CHECK(toDb(rms(r, 72000) / rms(M, 72000)) < -20.0);
}

TEST_CASE("A centre-panned vocal cannot be predicted and stays in the residual", "[predictor]")
{
	const auto v = voice(220.0, 20, 0.3, 48000.0, 96000);
	const auto g = voice(310.0, 10, 0.3, 48000.0, 96000);
	std::vector<float> M(v.size()), S(v.size());
	for (size_t i = 0; i < v.size(); ++i) {
		const float L = v[i] + g[i];
		const float R = v[i] + 0.3f * g[i];
		M[i] = 0.5f * (L + R);
		S[i] = 0.5f * (L - R);
	}
	const auto r = residual(M, S);
	// Residual ~= vocal: compare against the vocal itself.
	std::vector<float> diff(v.size());
	for (size_t i = 0; i < v.size(); ++i)
		diff[i] = r[i] - v[i];
	CHECK(toDb(rms(diff, 72000) / rms(v, 72000)) < -20.0);
}

TEST_CASE("Predictor is stable on silence, mono and extreme input", "[predictor]")
{
	SidePredictor p;
	p.prepare(48000.0);
	p.setStepSize(0.003);
	for (int i = 0; i < 48000; ++i)
		REQUIRE(p.process(0.0, 0.0) == 0.0); // silence
	const auto x = noise(1.0, 48000, 5);
	for (float s : x)
		REQUIRE(std::isfinite(p.process(s, 0.0))); // mono: no side at all
	for (float s : x)
		REQUIRE(std::isfinite(p.process(1000.0 * s, -1000.0 * s))); // absurdly loud
	CHECK(p.stateIsFinite());
}
