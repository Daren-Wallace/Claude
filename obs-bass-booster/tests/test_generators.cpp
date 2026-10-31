/*
Bass Booster for OBS - bass synthesis tests
Copyright (C) 2026 Daren Wallace
SPDX-License-Identifier: GPL-2.0-or-later

These tests prove the headline claim: when the low end is *gone* from the
input (not merely quiet), the plugin creates real new bass at the right pitch,
which an equaliser alone can never do.
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "dsp/bass_engine.hpp"
#include "dsp/generators.hpp"
#include "test_helpers.hpp"

using namespace bassbooster;
using namespace testutil;

namespace {

constexpr double kFs = 48000.0;

BassParams generatorsOnly()
{
	BassParams p = linearOnly();
	p.shelfGainDb = 0.0;
	p.punchEnabled = false;
	p.rumbleFreqHz = 20.0;
	return p;
}

Buffer runEngine(const BassParams &p, const std::vector<float> &input)
{
	BassEngine engine;
	engine.prepare(kFs, 1);
	engine.setParams(p);
	engine.snapToTargets();
	Buffer buf(1, input.size());
	buf.data[0] = input;
	processInChunks(engine, buf);
	return buf;
}

} // namespace

TEST_CASE("Harmonic restoration rebuilds a missing fundamental at the right pitch", "[generators][harmonics]")
{
	const double f0 = GENERATE(90.0, 110.0, 140.0);
	// Harmonics 3..8 only: nothing at all below 3*f0, like telephone audio.
	const auto input = missingFundamental(f0, 3, 8, 0.4, kFs, 96000);

	BassParams p = generatorsOnly();
	p.harmonicsEnabled = true;
	p.harmonicsAmount = 100.0;
	p.harmonicsDrive = 0.0;
	p.harmonicsLowHz = 2.5 * f0;
	p.harmonicsHighHz = 9.0 * f0;

	const Buffer out = runEngine(p, input);
	const size_t start = 48000, len = 32768;
	const double before = toneAmplitude(input, f0, kFs, start, len);
	const double after = toneAmplitude(out.data[0], f0, kFs, start, len);
	INFO("f0=" << f0 << " before=" << toDb(before) << " dBFS after=" << toDb(after) << " dBFS");
	CHECK(toDb(before) < -90.0); // truly absent from the input
	CHECK(toDb(after) > -30.0);  // clearly audible in the output
	CHECK(toDb(after) - toDb(before) > 60.0);

	// And the regenerated tone is at f0, not some other frequency.
	CHECK(after > 3.0 * toneAmplitude(out.data[0], f0 * 0.75, kFs, start, len));
	CHECK(after > 3.0 * toneAmplitude(out.data[0], f0 * 1.5, kFs, start, len));
}

TEST_CASE("Harmonic restoration amount scales the regenerated bass", "[generators][harmonics]")
{
	const auto input = missingFundamental(100.0, 3, 8, 0.4, kFs, 96000);
	BassParams p = generatorsOnly();
	p.harmonicsEnabled = true;
	p.harmonicsLowHz = 250.0;
	p.harmonicsHighHz = 900.0;

	p.harmonicsAmount = 25.0;
	const double quarter = toneAmplitude(runEngine(p, input).data[0], 100.0, kFs, 48000, 32768);
	p.harmonicsAmount = 100.0;
	const double full = toneAmplitude(runEngine(p, input).data[0], 100.0, kFs, 48000, 32768);
	CHECK(full / quarter > 3.5);
	CHECK(full / quarter < 4.5);
}

TEST_CASE("Harmonic restoration level tracks the input level (no noise pumping)", "[generators][harmonics]")
{
	BassParams p = generatorsOnly();
	p.harmonicsEnabled = true;
	p.harmonicsAmount = 100.0;
	p.harmonicsDrive = 60.0;
	p.harmonicsLowHz = 250.0;
	p.harmonicsHighHz = 900.0;
	const double loud = toneAmplitude(runEngine(p, missingFundamental(100.0, 3, 8, 0.4, kFs, 96000)).data[0], 100.0,
					  kFs, 48000, 32768);
	const double quiet = toneAmplitude(runEngine(p, missingFundamental(100.0, 3, 8, 0.04, kFs, 96000)).data[0],
					   100.0, kFs, 48000, 32768);
	CHECK(toDb(loud) - toDb(quiet) > 18.0); // 20 dB less input -> ~20 dB less bass
	CHECK(toDb(loud) - toDb(quiet) < 22.0);
}

TEST_CASE("Drive adds harmonic weight", "[generators][harmonics]")
{
	const auto input = sine(300.0, 0.4, kFs, 96000);
	BassParams p = generatorsOnly();
	p.harmonicsEnabled = true;
	p.harmonicsAmount = 100.0;
	p.harmonicsLowHz = 200.0;
	p.harmonicsHighHz = 1400.0;

	p.harmonicsDrive = 0.0;
	const double clean = toneAmplitude(runEngine(p, input).data[0], 900.0, kFs, 48000, 32768);
	p.harmonicsDrive = 100.0;
	const double driven = toneAmplitude(runEngine(p, input).data[0], 900.0, kFs, 48000, 32768);
	CHECK(toDb(driven) - toDb(clean) > 20.0); // 3rd harmonic only appears with drive
}

TEST_CASE("Sub-octave generator creates real bass one octave down", "[generators][sub]")
{
	const double f = GENERATE(120.0, 160.0, 200.0);
	const auto input = sine(f, 0.4, kFs, 96000);

	BassParams p = generatorsOnly();
	p.subEnabled = true;
	p.subAmount = 100.0;
	p.subFreqHz = f * 0.8; // detection band = 0.96f .. 2.4f
	p.subTightness = 50.0;

	const Buffer out = runEngine(p, input);
	const size_t start = 48000, len = 32768;
	const double before = toneAmplitude(input, f / 2.0, kFs, start, len);
	const double after = toneAmplitude(out.data[0], f / 2.0, kFs, start, len);
	INFO("f=" << f << " sub before=" << toDb(before) << " after=" << toDb(after));
	CHECK(toDb(before) < -90.0);
	CHECK(toDb(after) > -20.0);
	// Clean octave: the third subharmonic is far quieter than f/2.
	CHECK(after > 10.0 * toneAmplitude(out.data[0], f / 3.0, kFs, start, len));
}

TEST_CASE("Sub-octave output follows the source dynamics and stops in silence", "[generators][sub]")
{
	BassParams p = generatorsOnly();
	p.subEnabled = true;
	p.subAmount = 100.0;
	p.subFreqHz = 100.0;
	p.subTightness = 100.0; // fastest release

	// 0.5 s tone burst then 0.5 s silence.
	std::vector<float> input(48000, 0.0f);
	const auto tone = sine(160.0, 0.4, kFs, 24000);
	std::copy(tone.begin(), tone.end(), input.begin());
	const Buffer out = runEngine(p, input);
	CHECK(rms(out.data[0], 12000, 24000) > 0.05); // sub present during the burst
	CHECK(rms(out.data[0], 36000, 48000) < 1e-4); // gone within ~0.25 s of silence
}

TEST_CASE("Sub-octave trigger ignores low-level noise", "[generators][sub]")
{
	dsp::SubOctaveGenerator sub;
	sub.prepare(kFs);
	sub.setup(100.0, 0.5);
	const auto hiss = noise(1e-6, 48000, 77); // -120 dBFS
	double maxOut = 0.0;
	for (float x : hiss)
		maxOut = std::fmax(maxOut, std::fabs(sub.process(x)));
	CHECK(maxOut < 1e-5);
}

TEST_CASE("Restore preset turns telephone-band audio into full-range audio", "[generators][presets]")
{
	// Voice-like tone at 120 Hz with nothing below 300 Hz.
	const auto input = missingFundamental(120.0, 3, 12, 0.3, kFs, 96000);
	const Buffer out = runEngine(findPreset("restore")->params, input);

	double lowBefore = 0.0, lowAfter = 0.0;
	for (double f : {60.0, 120.0, 240.0}) {
		lowBefore += toneAmplitude(input, f, kFs, 48000, 32768);
		lowAfter += toneAmplitude(out.data[0], f, kFs, 48000, 32768);
	}
	INFO("energy below 300 Hz before " << toDb(lowBefore) << " dBFS, after " << toDb(lowAfter) << " dBFS");
	CHECK(toDb(lowAfter) - toDb(lowBefore) > 40.0);
	CHECK(toDb(lowAfter) > -30.0);
	CHECK(peak(out.data[0], 48000) <= std::pow(10.0, -1.0 / 20.0) + 1e-6); // limiter holds
}

TEST_CASE("Generators are summed in mono and added equally to every channel", "[generators][channels]")
{
	BassParams p = generatorsOnly();
	p.subEnabled = true;
	p.subAmount = 100.0;
	p.subFreqHz = 100.0;
	BassEngine engine;
	engine.prepare(kFs, 2);
	engine.setParams(p);
	engine.snapToTargets();
	Buffer buf(2, 48000);
	buf.data[0] = sine(160.0, 0.4, kFs, 48000);
	// Right channel silent: it still receives the (mono) sub bass.
	processInChunks(engine, buf);
	CHECK(toneAmplitude(buf.data[1], 80.0, kFs, 16000, 16384) > 0.02);
	// Linearity check: (left with generators) - (left without generators)
	// must equal the right channel exactly, i.e. both channels received the
	// very same synthesised signal.
	BassParams noSynth = p;
	noSynth.subEnabled = false;
	BassEngine plain;
	plain.prepare(kFs, 2);
	plain.setParams(noSynth);
	plain.snapToTargets();
	Buffer ref(2, 48000);
	ref.data[0] = sine(160.0, 0.4, kFs, 48000);
	processInChunks(plain, ref);
	double maxDiff = 0.0;
	for (size_t i = 0; i < 48000; ++i)
		maxDiff = std::fmax(maxDiff, std::fabs((buf.data[0][i] - ref.data[0][i]) - buf.data[1][i]));
	CHECK(maxDiff < 1e-5);
}
