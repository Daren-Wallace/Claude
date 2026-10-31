/*
Bass Booster for OBS - filter primitive tests
Copyright (C) 2026 Daren Wallace
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/biquad.hpp"
#include "dsp/utils.hpp"
#include "test_helpers.hpp"

using namespace bassbooster::dsp;
using Catch::Matchers::WithinAbs;

TEST_CASE("Butterworth low/high-pass are -3 dB at the cutoff", "[biquad]")
{
	const double fs = GENERATE(44100.0, 48000.0, 96000.0);
	const double fc = GENERATE(25.0, 100.0, 1000.0);
	const auto lp = BiquadCoeffs::lowPass(fs, fc);
	const auto hp = BiquadCoeffs::highPass(fs, fc);
	CHECK_THAT(gainToDb(lp.magnitudeAt(fs, fc)), WithinAbs(-3.0103, 0.01));
	CHECK_THAT(gainToDb(hp.magnitudeAt(fs, fc)), WithinAbs(-3.0103, 0.01));
	CHECK_THAT(gainToDb(lp.magnitudeAt(fs, 1.0)), WithinAbs(0.0, 0.01));
	CHECK(lp.isStable());
	CHECK(hp.isStable());
}

TEST_CASE("Peaking filter hits its gain at the centre frequency", "[biquad]")
{
	const double gain = GENERATE(-12.0, -3.0, 3.0, 12.0);
	const auto c = BiquadCoeffs::peaking(48000.0, 80.0, 1.0, gain);
	CHECK_THAT(gainToDb(c.magnitudeAt(48000.0, 80.0)), WithinAbs(gain, 0.01));
	CHECK_THAT(gainToDb(c.magnitudeAt(48000.0, 10000.0)), WithinAbs(0.0, 0.05));
}

TEST_CASE("Low shelf reaches full gain at low frequencies and unity above", "[biquad]")
{
	const double gain = GENERATE(-12.0, 6.0, 24.0);
	const auto c = BiquadCoeffs::lowShelf(48000.0, 120.0, kButterworthQ, gain);
	CHECK_THAT(gainToDb(c.magnitudeAt(48000.0, 5.0)), WithinAbs(gain, 0.05));
	CHECK_THAT(gainToDb(c.magnitudeAt(48000.0, 120.0)), WithinAbs(gain / 2.0, 0.05)); // half gain at corner
	CHECK_THAT(gainToDb(c.magnitudeAt(48000.0, 8000.0)), WithinAbs(0.0, 0.05));
}

TEST_CASE("4th-order Butterworth rolls off at 24 dB/octave", "[biquad]")
{
	Butterworth4 hp;
	hp.setup(Butterworth4::Type::HighPass, 48000.0, 100.0);
	CHECK_THAT(gainToDb(hp.magnitudeAt(48000.0, 100.0)), WithinAbs(-3.0103, 0.01));
	CHECK_THAT(gainToDb(hp.magnitudeAt(48000.0, 25.0)), WithinAbs(-48.2, 0.3)); // two octaves down
	CHECK_THAT(gainToDb(hp.magnitudeAt(48000.0, 1000.0)), WithinAbs(0.0, 0.01));
}

TEST_CASE("Every filter stays stable across the full parameter space", "[biquad]")
{
	for (double fs : {8000.0, 22050.0, 44100.0, 48000.0, 96000.0, 192000.0}) {
		for (double f = 1.0; f < fs; f *= 1.37) {
			for (double q : {0.05, 0.3, 0.707, 1.5, 4.0, 40.0}) {
				REQUIRE(BiquadCoeffs::lowPass(fs, f, q).isStable());
				REQUIRE(BiquadCoeffs::highPass(fs, f, q).isStable());
				REQUIRE(BiquadCoeffs::bandPass(fs, f, q).isStable());
				for (double g : {-24.0, 0.0, 24.0}) {
					REQUIRE(BiquadCoeffs::peaking(fs, f, q, g).isStable());
					REQUIRE(BiquadCoeffs::lowShelf(fs, f, q, g).isStable());
				}
			}
		}
	}
}

TEST_CASE("Out-of-range frequencies are clamped instead of producing NaN", "[biquad]")
{
	for (double f : {-100.0, 0.0, 1e9, std::nan("")}) {
		const auto c = BiquadCoeffs::lowShelf(48000.0, f, 0.7, 12.0);
		REQUIRE(std::isfinite(c.b0));
		REQUIRE(c.isStable());
	}
}

TEST_CASE("Measured biquad output matches the analytic response", "[biquad]")
{
	const double fs = 48000.0;
	Biquad bq;
	bq.setCoeffs(BiquadCoeffs::lowShelf(fs, 120.0, kButterworthQ, 9.0));
	const auto in = testutil::sine(60.0, 0.1, fs, 48000);
	std::vector<float> out(in.size());
	for (size_t i = 0; i < in.size(); ++i)
		out[i] = static_cast<float>(bq.process(in[i]));
	const double measured = testutil::toDb(testutil::rms(out, 24000) / testutil::rms(in, 24000));
	CHECK_THAT(measured, WithinAbs(gainToDb(bq.getCoeffs().magnitudeAt(fs, 60.0)), 0.02));
}

TEST_CASE("Smoother converges exponentially and snaps exactly to target", "[utils]")
{
	Smoother s;
	s.setTimeConstant(48000.0, 10.0);
	s.snap(0.0);
	s.setTarget(1.0);
	double v = 0.0;
	for (int i = 0; i < 480; ++i) // one time constant
		v = s.next();
	CHECK_THAT(v, WithinAbs(1.0 - std::exp(-1.0), 0.01));
	for (int i = 0; i < 48000; ++i)
		s.next();
	CHECK(s.settled());
	CHECK(s.value() == 1.0);
}

TEST_CASE("Linear ramp reaches its target in the configured time", "[utils]")
{
	LinearRamp r;
	r.setRampTime(48000.0, 20.0);
	r.snap(0.0);
	r.setTarget(1.0);
	int steps = 0;
	while (!r.settled()) {
		r.next();
		++steps;
	}
	CHECK(steps == 960);
	CHECK(r.value() == 1.0);
}

TEST_CASE("Envelope follower tracks peaks and decays", "[utils]")
{
	EnvelopeFollower e;
	e.setup(48000.0, 0.0, 50.0);
	CHECK(e.process(0.8) == 0.8); // instant attack
	double v = 0.0;
	for (int i = 0; i < 2400; ++i) // one release time constant
		v = e.process(0.0);
	CHECK_THAT(v, WithinAbs(0.8 * std::exp(-1.0), 0.01));
}
