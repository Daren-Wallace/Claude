/*
Instrumental Isolator for OBS - pitch tracker, comb, transient and limiter tests
Copyright (C) 2026 Daren Wallace
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "dsp/analysis.hpp"
#include "test_helpers.hpp"

using namespace isolator::dsp;
using namespace testutil;

TEST_CASE("Pitch tracker is accurate to 1 % across the voice range", "[analysis][pitch]")
{
	const double fs = GENERATE(44100.0, 48000.0);
	for (double f0 : {82.0, 110.0, 150.0, 220.0, 330.0, 440.0, 660.0, 880.0}) {
		PitchTracker t;
		t.prepare(fs);
		t.setRange(80.0, 1000.0);
		const auto v = voice(f0, 12, 0.3, fs, static_cast<size_t>(fs * 0.5));
		for (float x : v)
			t.process(x);
		INFO("fs=" << fs << " f0=" << f0 << " tracked=" << t.f0() << " conf=" << t.confidence());
		CHECK(t.voiced());
		CHECK(std::fabs(t.f0() - f0) / f0 < 0.01);
	}
}

TEST_CASE("Pitch tracker stays unvoiced on noise and silence", "[analysis][pitch]")
{
	PitchTracker t;
	t.prepare(48000.0);
	for (float x : noise(0.3, 48000, 11))
		t.process(x);
	CHECK_FALSE(t.voiced());
	PitchTracker s;
	s.prepare(48000.0);
	for (int i = 0; i < 48000; ++i)
		s.process(0.0);
	CHECK_FALSE(s.voiced());
	CHECK(s.confidence() == 0.0);
}

TEST_CASE("Pitch tracker follows a pitch change within tens of milliseconds", "[analysis][pitch]")
{
	PitchTracker t;
	t.prepare(48000.0);
	const auto a = voice(200.0, 10, 0.3, 48000.0, 24000);
	const auto b = voice(300.0, 10, 0.3, 48000.0, 24000);
	for (float x : a)
		t.process(x);
	CHECK(std::fabs(t.f0() - 200.0) < 2.0);
	size_t samplesToLock = 0;
	for (size_t i = 0; i < b.size(); ++i) {
		t.process(b[i]);
		if (samplesToLock == 0 && std::fabs(t.f0() - 300.0) < 3.0)
			samplesToLock = i + 1;
	}
	INFO("locked after " << static_cast<double>(samplesToLock) / 48.0 << " ms");
	CHECK(samplesToLock > 0);
	CHECK(samplesToLock < 48 * 60); // < 60 ms
}

TEST_CASE("Harmonic comb removes harmonics and leaves other frequencies", "[analysis][comb]")
{
	HarmonicComb comb;
	comb.prepare(48000.0);
	comb.setLimits(20, 100.0, 6000.0);
	comb.setTargetF0(200.0);
	for (int i = 0; i < 400; ++i)
		comb.refresh(false, 16); // let the f0 glide settle
	CHECK(comb.activeNotches() == 20);

	auto measure = [&](double f) {
		HarmonicComb c = comb;
		c.reset();
		const auto in = sine(f, 0.3, 48000.0, 48000);
		std::vector<float> out(in.size());
		for (size_t i = 0; i < in.size(); ++i)
			out[i] = static_cast<float>(c.process(in[i]));
		return toDb(rms(out, 24000) / rms(in, 24000));
	};
	CHECK(measure(400.0) < -30.0);  // 2nd harmonic
	CHECK(measure(1000.0) < -30.0); // 5th harmonic
	CHECK(measure(500.0) > -3.0);   // between harmonics
	CHECK(measure(310.0) > -3.0);
}

TEST_CASE("Harmonic comb respects its frequency window", "[analysis][comb]")
{
	HarmonicComb comb;
	comb.prepare(48000.0);
	comb.setLimits(40, 300.0, 2000.0);
	comb.setTargetF0(100.0);
	for (int i = 0; i < 400; ++i)
		comb.refresh(false, 16);
	CHECK(comb.activeNotches() == 18); // 300..2000 Hz -> harmonics 3..20
}

TEST_CASE("Transient detector fires on hits but not on steady spiky tones", "[analysis][transient]")
{
	TransientDetector d;
	d.prepare(48000.0);
	d.setAmount(0.5);
	// A harmonic-rich steady voice (high crest factor) must not trigger.
	for (float x : voice(220.0, 20, 0.3, 48000.0, 96000))
		d.process(x);
	CHECK(d.onsetCount() <= 1); // at most the initial onset

	// Four snare-like noise bursts over a quiet bed.
	TransientDetector h;
	h.prepare(48000.0);
	h.setAmount(0.5);
	const auto bed = noise(0.002, 96000, 4);
	const auto hit = noise(0.5, 96000, 8);
	double minFactor = 1.0;
	for (size_t i = 0; i < bed.size(); ++i) {
		const bool inHit = (i % 24000) < 2400;
		const double decay = std::exp(-static_cast<double>(i % 24000) / 600.0);
		minFactor = std::fmin(minFactor, h.process(inHit ? hit[i] * decay : bed[i]));
	}
	CHECK(h.onsetCount() == 4);
	CHECK(minFactor < 0.55);
}

TEST_CASE("Peak limiter never exceeds its ceiling", "[analysis][limiter]")
{
	PeakLimiter l;
	l.prepare(48000.0, -1.0, 150.0);
	const auto x = noise(4.0, 48000, 2);
	for (float s : x) {
		const double g = l.process(std::fabs(s));
		REQUIRE(std::fabs(s) * g <= l.getCeiling() + 1e-12);
	}
}
