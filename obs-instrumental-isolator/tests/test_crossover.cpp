/*
Instrumental Isolator for OBS - zero-latency filterbank tests
Copyright (C) 2026 Daren Wallace
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/crossover.hpp"
#include "test_helpers.hpp"

using namespace isolator::dsp;
using Catch::Matchers::WithinAbs;

TEST_CASE("Band layouts are monotonic and stay below Nyquist", "[crossover]")
{
	const double fs = GENERATE(22050.0, 44100.0, 48000.0, 96000.0);
	for (const BandLayout &l : {BandLayout::thirdOctave(fs), BandLayout::twoThirdOctave(fs)}) {
		REQUIRE(l.numCrossovers > 0);
		for (int k = 1; k < l.numCrossovers; ++k)
			REQUIRE(l.freqs[k] > l.freqs[k - 1]);
		REQUIRE(l.freqs[l.numCrossovers - 1] <= fs * 0.45);
		for (int b = 0; b < l.numBands(); ++b) {
			REQUIRE(l.lowerEdge(b) < l.upperEdge(b, fs));
			REQUIRE(l.bandCentre(b, fs) > 0.0);
		}
	}
	CHECK(BandLayout::twoThirdOctave(48000.0).numBands() == 11);
	CHECK(BandLayout::thirdOctave(48000.0).numBands() == 21);
}

TEST_CASE("All-pass sections have unit magnitude", "[crossover]")
{
	for (double f : {100.0, 1000.0, 6000.0}) {
		const auto c = BiquadCoeffs::allPass(48000.0, f);
		for (double probe : {20.0, 100.0, 1000.0, 10000.0})
			CHECK_THAT(c.magnitudeAt(48000.0, probe), WithinAbs(1.0, 1e-12));
	}
}

TEST_CASE("Linkwitz-Riley halves sum to an all-pass and are -6 dB at the crossover", "[crossover]")
{
	LR4Split split;
	split.setup(48000.0, 1000.0);
	// Steady-state amplitude of each output for a 1 kHz sine.
	const auto in = testutil::sine(1000.0, 0.5, 48000.0, 48000);
	std::vector<float> lo(in.size()), hi(in.size()), sum(in.size());
	for (size_t i = 0; i < in.size(); ++i) {
		double l, h;
		split.process(in[i], l, h);
		lo[i] = static_cast<float>(l);
		hi[i] = static_cast<float>(h);
		sum[i] = static_cast<float>(l + h);
	}
	CHECK_THAT(testutil::toDb(testutil::rms(lo, 24000) / testutil::rms(in, 24000)), WithinAbs(-6.02, 0.05));
	CHECK_THAT(testutil::toDb(testutil::rms(hi, 24000) / testutil::rms(in, 24000)), WithinAbs(-6.02, 0.05));
	CHECK_THAT(testutil::toDb(testutil::rms(sum, 24000) / testutil::rms(in, 24000)), WithinAbs(0.0, 0.01));
}

TEST_CASE("Splitter + combiner recombine to exactly the all-pass chain (flat, zero latency)", "[crossover]")
{
	const double fs = GENERATE(44100.0, 48000.0, 96000.0);
	for (const BandLayout &layout : {BandLayout::thirdOctave(fs), BandLayout::twoThirdOctave(fs)}) {
		BandSplitter split;
		BandCombiner comb;
		AllpassChain chain;
		split.prepare(fs, layout);
		comb.prepare(fs, layout);
		chain.prepare(fs, layout);
		const auto x = testutil::noise(0.8, 100000, 3);
		double bands[kMaxBands];
		double maxErr = 0.0;
		for (float s : x) {
			split.process(s, bands);
			maxErr = std::fmax(maxErr, std::fabs(comb.process(bands) - chain.process(s)));
		}
		INFO("fs=" << fs << " bands=" << layout.numBands());
		CHECK(maxErr < 1e-10);
	}
}

TEST_CASE("Horner combiner equals per-band phase compensation for any weights", "[crossover]")
{
	const double fs = 48000.0;
	const BandLayout layout = BandLayout::twoThirdOctave(fs);
	const int n = layout.numCrossovers;
	BandSplitter split;
	BandCombiner comb;
	split.prepare(fs, layout);
	comb.prepare(fs, layout);
	// Reference: each band through its own chain of compensating all-passes.
	std::vector<std::vector<Biquad>> comp(static_cast<size_t>(n + 1));
	for (int b = 0; b <= n; ++b)
		for (int k = b + 1; k < n; ++k) {
			Biquad ap;
			ap.setCoeffs(BiquadCoeffs::allPass(fs, layout.freqs[k]));
			comp[static_cast<size_t>(b)].push_back(ap);
		}
	const auto x = testutil::noise(0.5, 50000, 9);
	double bands[kMaxBands], weighted[kMaxBands];
	double maxErr = 0.0;
	for (size_t i = 0; i < x.size(); ++i) {
		split.process(x[i], bands);
		double reference = 0.0;
		for (int b = 0; b <= n; ++b) {
			const double w = 0.5 + 0.5 * std::sin(0.001 * static_cast<double>(i) * (b + 1)); // time-varying
			weighted[b] = w * bands[b];
			double y = weighted[b];
			for (Biquad &ap : comp[static_cast<size_t>(b)])
				y = ap.process(y);
			reference += y;
		}
		maxErr = std::fmax(maxErr, std::fabs(comb.process(weighted) - reference));
	}
	CHECK(maxErr < 1e-10);
}

TEST_CASE("Filterbank output starts on the very first sample (no buffering)", "[crossover][latency]")
{
	// Zero latency means the impulse response begins at n = 0. The all-pass
	// phase rotation spreads low frequencies by a few ms (as any EQ does),
	// but the response peak arrives within the first millisecond, where a
	// buffered (FFT) design could not produce any output at all.
	const BandLayout layout = BandLayout::twoThirdOctave(48000.0);
	BandSplitter split;
	BandCombiner comb;
	split.prepare(48000.0, layout);
	comb.prepare(48000.0, layout);
	double bands[kMaxBands];
	std::vector<double> h(4800);
	for (size_t i = 0; i < h.size(); ++i) {
		split.process(i == 0 ? 1.0 : 0.0, bands);
		h[i] = comb.process(bands);
	}
	size_t peakIndex = 0;
	for (size_t i = 1; i < h.size(); ++i)
		if (std::fabs(h[i]) > std::fabs(h[peakIndex]))
			peakIndex = i;
	CHECK(std::fabs(h[0]) > 1e-3);
	CHECK(peakIndex < 48); // < 1 ms
}
