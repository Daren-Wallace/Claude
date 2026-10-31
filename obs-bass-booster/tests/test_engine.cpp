/*
Bass Booster for OBS - full processing chain tests
Copyright (C) 2026 Daren Wallace
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/bass_engine.hpp"
#include "test_helpers.hpp"

#include <chrono>
#include <cstring>
#include <limits>
#include <random>

using namespace bassbooster;
using namespace testutil;
using Catch::Matchers::WithinAbs;

namespace {

BassParams shelfOnly(double gainDb, double freq = 120.0)
{
	BassParams p = linearOnly();
	p.punchEnabled = false;
	p.shelfGainDb = gainDb;
	p.shelfFreqHz = freq;
	return p;
}

} // namespace

// ------------------------------------------------------- frequency response

TEST_CASE("Bass is boosted while mids and highs are untouched", "[engine][response]")
{
	const BassParams p = shelfOnly(8.0, 120.0);
	CHECK_THAT(measureGainDb(p, 40.0), WithinAbs(7.6, 0.4));
	CHECK(measureGainDb(p, 60.0) > 6.5);
	CHECK_THAT(measureGainDb(p, 1000.0), WithinAbs(0.0, 0.2));
	CHECK_THAT(measureGainDb(p, 5000.0), WithinAbs(0.0, 0.1));
}

TEST_CASE("Measured response matches the analytic linear response", "[engine][response]")
{
	const double fs = GENERATE(44100.0, 48000.0, 96000.0);
	BassParams p = linearOnly();
	p.shelfGainDb = 10.0;
	p.shelfFreqHz = 150.0;
	p.punchEnabled = true;
	p.punchGainDb = 4.0;
	p.punchFreqHz = 70.0;
	p.rumbleFreqHz = 25.0;
	p.mix = 80.0;
	p.outputGainDb = -3.0;

	BassEngine reference;
	reference.prepare(fs, 1);
	reference.setParams(p);
	for (double f : {20.0, 35.0, 70.0, 150.0, 400.0, 2000.0, 10000.0}) {
		INFO("fs=" << fs << " f=" << f);
		CHECK_THAT(measureGainDb(p, f, fs), WithinAbs(reference.linearResponseDb(f), 0.15));
	}
}

TEST_CASE("Rumble filter removes infrasonic energy", "[engine][response]")
{
	BassParams p = shelfOnly(12.0, 120.0);
	p.rumbleFreqHz = 30.0;
	CHECK(measureGainDb(p, 7.5) < -30.0); // 2 octaves below the HPF, despite +12 dB shelf
	CHECK(measureGainDb(p, 60.0) > 9.0);
}

TEST_CASE("Negative shelf gain cuts bass", "[engine][response]")
{
	CHECK(measureGainDb(shelfOnly(-12.0, 150.0), 50.0) < -10.0);
}

TEST_CASE("Disabled punch section is exactly flat", "[engine][response]")
{
	BassParams on = shelfOnly(0.0);
	on.punchEnabled = true;
	on.punchGainDb = 12.0;
	on.punchFreqHz = 80.0;
	BassParams off = on;
	off.punchEnabled = false;
	CHECK(measureGainDb(on, 80.0) > 11.0);
	CHECK_THAT(measureGainDb(off, 80.0), WithinAbs(0.0, 0.05));
}

TEST_CASE("Input trim, output gain and mix behave as documented", "[engine][response]")
{
	BassParams p = shelfOnly(0.0);
	p.inputGainDb = 6.0;
	CHECK_THAT(measureGainDb(p, 1000.0), WithinAbs(6.0, 0.05));
	p = shelfOnly(0.0);
	p.outputGainDb = -6.0;
	CHECK_THAT(measureGainDb(p, 1000.0), WithinAbs(-6.0, 0.05));
	p = shelfOnly(12.0);
	p.mix = 50.0;
	const double half = measureGainDb(p, 40.0);
	CHECK(half > 5.0);
	CHECK(half < 10.0);
}

// ---------------------------------------------------------------- limiter

TEST_CASE("Limiter never lets a single sample exceed the ceiling", "[engine][limiter]")
{
	const double ceilingDb = GENERATE(-12.0, -6.0, -1.0, 0.0);
	BassParams p = findPreset("extreme")->params;
	p.limiterEnabled = true;
	p.limiterCeilingDb = ceilingDb;
	p.inputGainDb = 12.0;

	BassEngine engine;
	engine.prepare(48000.0, 2);
	engine.setParams(p);
	engine.snapToTargets();

	Buffer buf(2, 96000);
	buf.data[0] = sine(55.0, 0.9, 48000.0, 96000);
	buf.data[1] = noise(0.9, 96000, 7);
	processInChunks(engine, buf, 480);

	const double ceiling = std::pow(10.0, ceilingDb / 20.0);
	for (int ch = 0; ch < 2; ++ch)
		REQUIRE(peak(buf.data[ch]) <= ceiling + 1e-6);
	CHECK(engine.getLimiterGainReductionDb() < -1.0); // it really had to work
}

TEST_CASE("Limiter is transparent below the ceiling", "[engine][limiter]")
{
	BassParams p = shelfOnly(0.0);
	p.limiterEnabled = true;
	p.limiterCeilingDb = -1.0;
	CHECK_THAT(measureGainDb(p, 1000.0, 48000.0, 0.5), WithinAbs(0.0, 1e-4));
}

TEST_CASE("Limiter is channel-linked so the stereo image never shifts", "[engine][limiter]")
{
	BassParams p = shelfOnly(0.0);
	p.limiterEnabled = true;
	p.limiterCeilingDb = -6.0;
	BassEngine engine;
	engine.prepare(48000.0, 2);
	engine.setParams(p);
	engine.snapToTargets();
	Buffer buf(2, 48000);
	buf.data[0] = sine(200.0, 1.0, 48000.0, 48000); // loud left
	buf.data[1] = sine(200.0, 0.1, 48000.0, 48000); // quiet right
	processInChunks(engine, buf);
	const double ratio = rms(buf.data[0], 24000) / rms(buf.data[1], 24000);
	CHECK_THAT(ratio, WithinAbs(10.0, 0.05)); // left/right balance preserved
}

// -------------------------------------------------- transparency & bypass

TEST_CASE("Bypass leaves audio bit-for-bit untouched", "[engine][bypass]")
{
	BassParams p = findPreset("extreme")->params;
	p.bypass = true;
	BassEngine engine;
	engine.prepare(48000.0, 2);
	engine.setParams(p);
	engine.snapToTargets();

	Buffer buf(2, 10000);
	buf.data[0] = noise(0.8, 10000, 1);
	buf.data[1] = noise(0.8, 10000, 2);
	const Buffer original = buf;
	processInChunks(engine, buf, 1024);
	CHECK(std::memcmp(buf.data[0].data(), original.data[0].data(), 10000 * sizeof(float)) == 0);
	CHECK(std::memcmp(buf.data[1].data(), original.data[1].data(), 10000 * sizeof(float)) == 0);
}

TEST_CASE("Mix at 0% with the limiter off is bit-exact", "[engine][bypass]")
{
	BassParams p = findPreset("extreme")->params;
	p.mix = 0.0;
	p.outputGainDb = 0.0;
	p.limiterEnabled = false;
	BassEngine engine;
	engine.prepare(48000.0, 1);
	engine.setParams(p);
	engine.snapToTargets();
	Buffer buf(1, 5000);
	buf.data[0] = noise(0.5, 5000, 3);
	const Buffer original = buf;
	processInChunks(engine, buf, 512);
	CHECK(buf.data[0] == original.data[0]);
}

TEST_CASE("Toggling bypass crossfades without clicks", "[engine][bypass][smoothing]")
{
	BassParams p = shelfOnly(18.0, 150.0);
	BassEngine engine;
	engine.prepare(48000.0, 1);
	engine.setParams(p);
	engine.snapToTargets();

	const size_t frames = 48000;
	Buffer buf(1, frames);
	buf.data[0] = sine(80.0, 0.05, 48000.0, frames);
	auto ptrs = buf.pointers();
	for (size_t off = 0; off < frames; off += 480) {
		if (off == 24000) {
			p.bypass = true;
			engine.setParams(p);
		}
		float *ch = ptrs[0] + off;
		engine.process(&ch, 1, 480);
	}
	// The largest sample-to-sample step must stay close to that of the
	// steady-state (boosted) sine; a click would be far larger.
	double steady = 0.0, worst = 0.0;
	for (size_t i = 12001; i < frames; ++i) {
		const double d = std::fabs(buf.data[0][i] - buf.data[0][i - 1]);
		if (i < 24000)
			steady = std::fmax(steady, d);
		worst = std::fmax(worst, d);
	}
	CHECK(worst <= steady * 1.05);
}

TEST_CASE("Large parameter jumps are smoothed (no zipper noise)", "[engine][smoothing]")
{
	BassParams p = shelfOnly(0.0, 100.0);
	BassEngine engine;
	engine.prepare(48000.0, 1);
	engine.setParams(p);
	engine.snapToTargets();

	const size_t frames = 48000;
	Buffer buf(1, frames);
	buf.data[0] = sine(60.0, 0.03, 48000.0, frames);
	auto ptrs = buf.pointers();
	for (size_t off = 0; off < frames; off += 1024) {
		if (off >= 12288 && off < 13312) {
			p.shelfGainDb = 24.0; // jump 0 -> +24 dB in one update
			engine.setParams(p);
		}
		const size_t n = std::min<size_t>(1024, frames - off);
		float *ch = ptrs[0] + off;
		engine.process(&ch, 1, static_cast<int>(n));
	}
	double steadyAfter = 0.0, transition = 0.0;
	for (size_t i = 1; i < frames; ++i) {
		const double d = std::fabs(buf.data[0][i] - buf.data[0][i - 1]);
		if (i > 36000)
			steadyAfter = std::fmax(steadyAfter, d);
		else if (i > 12288)
			transition = std::fmax(transition, d);
	}
	CHECK(transition <= steadyAfter * 1.1);
	CHECK(steadyAfter > 0.0);
}

// ---------------------------------------------------------- robustness

TEST_CASE("Silence in gives exact silence out", "[engine][robustness]")
{
	BassEngine engine;
	engine.prepare(48000.0, 2);
	engine.setParams(findPreset("extreme")->params);
	engine.snapToTargets();
	Buffer buf(2, 48000);
	processInChunks(engine, buf);
	CHECK(peak(buf.data[0]) == 0.0);
	CHECK(peak(buf.data[1]) == 0.0);
}

TEST_CASE("Signal decays to exact zero after the input stops (no denormal tail)", "[engine][robustness]")
{
	BassEngine engine;
	engine.prepare(48000.0, 1);
	engine.setParams(findPreset("restore")->params);
	engine.snapToTargets();
	Buffer buf(1, 48000 * 12);
	auto tone = missingFundamental(110.0, 3, 8, 0.5, 48000.0, 48000);
	std::copy(tone.begin(), tone.end(), buf.data[0].begin());
	processInChunks(engine, buf);
	// After ~11 s of silence everything must have flushed to true zero.
	for (size_t i = buf.frames() - 4800; i < buf.frames(); ++i)
		REQUIRE(buf.data[0][i] == 0.0f);
}

TEST_CASE("Chunk size does not change the result", "[engine][robustness]")
{
	const auto run = [](size_t chunk) {
		BassEngine engine;
		engine.prepare(48000.0, 2);
		engine.setParams(findPreset("restore")->params);
		engine.snapToTargets();
		Buffer buf(2, 20000);
		buf.data[0] = missingFundamental(100.0, 3, 9, 0.4, 48000.0, 20000);
		buf.data[1] = noise(0.2, 20000, 9);
		processInChunks(engine, buf, chunk);
		return buf;
	};
	const Buffer reference = run(20000);
	for (size_t chunk : {1, 7, 31, 32, 33, 480, 1023, 1024}) {
		INFO("chunk " << chunk);
		const Buffer b = run(chunk);
		for (int ch = 0; ch < 2; ++ch) {
			double maxDiff = 0.0;
			for (size_t i = 0; i < b.frames(); ++i)
				maxDiff = std::fmax(maxDiff, std::fabs(b.data[ch][i] - reference.data[ch][i]));
			CHECK(maxDiff < 1e-6);
		}
	}
}

TEST_CASE("Zero-length, null and oversized-channel calls are safe", "[engine][robustness]")
{
	BassEngine engine;
	engine.prepare(48000.0, 2);
	engine.process(nullptr, 2, 1024);
	float data[16] = {0.5f, -0.5f};
	float *ptr = data;
	engine.process(&ptr, 1, 0);
	engine.process(&ptr, 0, 16);
	engine.process(&ptr, 1, -5);
	float *nullCh[2] = {data, nullptr};
	engine.process(nullCh, 2, 16);
	CHECK(data[0] == 0.5f);

	// More channels than the engine supports: extra channels untouched.
	engine.prepare(48000.0, 12);
	CHECK(engine.getNumChannels() == BassEngine::kMaxChannels);
	Buffer buf(12, 256);
	buf.data[10] = noise(0.5, 256, 4);
	const auto untouched = buf.data[10];
	auto ptrs = buf.pointers();
	engine.process(ptrs.data(), 12, 256);
	CHECK(buf.data[10] == untouched);
}

TEST_CASE("A NaN in the input never poisons the stream", "[engine][robustness]")
{
	BassEngine engine;
	engine.prepare(48000.0, 1);
	engine.setParams(findPreset("restore")->params);
	engine.snapToTargets();

	Buffer buf(1, 4800);
	buf.data[0] = sine(200.0, 0.5, 48000.0, 4800);
	buf.data[0][100] = std::numeric_limits<float>::quiet_NaN();
	buf.data[0][200] = std::numeric_limits<float>::infinity();
	processInChunks(engine, buf, 480);
	for (size_t i = 0; i < buf.frames(); ++i) {
		if (i == 100 || i == 200)
			continue;
		REQUIRE(std::isfinite(buf.data[0][i]));
	}

	Buffer next(1, 4800);
	next.data[0] = sine(200.0, 0.5, 48000.0, 4800);
	processInChunks(engine, next, 480);
	for (float v : next.data[0])
		REQUIRE(std::isfinite(v));
	CHECK(rms(next.data[0]) > 0.1);
}

TEST_CASE("Randomised stress: any parameters, rates and channel counts stay finite and bounded",
	  "[engine][robustness][stress]")
{
	std::mt19937 rng(20261031);
	std::uniform_real_distribution<double> u(0.0, 1.0);
	auto pick = [&](const ParamRange &r) {
		return r.min + (r.max - r.min) * u(rng);
	};

	for (int trial = 0; trial < 60; ++trial) {
		BassParams p;
		p.inputGainDb = pick(ranges::kInputGainDb);
		p.shelfFreqHz = pick(ranges::kShelfFreqHz);
		p.shelfGainDb = pick(ranges::kShelfGainDb);
		p.shelfQ = pick(ranges::kShelfQ);
		p.punchEnabled = u(rng) > 0.5;
		p.punchFreqHz = pick(ranges::kPunchFreqHz);
		p.punchGainDb = pick(ranges::kPunchGainDb);
		p.punchQ = pick(ranges::kPunchQ);
		p.harmonicsEnabled = u(rng) > 0.5;
		p.harmonicsAmount = pick(ranges::kHarmonicsAmount);
		p.harmonicsDrive = pick(ranges::kHarmonicsDrive);
		p.harmonicsLowHz = pick(ranges::kHarmonicsLowHz);
		p.harmonicsHighHz = pick(ranges::kHarmonicsHighHz);
		p.subEnabled = u(rng) > 0.5;
		p.subAmount = pick(ranges::kSubAmount);
		p.subFreqHz = pick(ranges::kSubFreqHz);
		p.subTightness = pick(ranges::kSubTightness);
		p.rumbleFreqHz = pick(ranges::kRumbleFreqHz);
		p.limiterEnabled = u(rng) > 0.3;
		p.limiterCeilingDb = pick(ranges::kLimiterCeilingDb);
		p.limiterReleaseMs = pick(ranges::kLimiterReleaseMs);
		p.mix = pick(ranges::kMix);
		p.outputGainDb = pick(ranges::kOutputGainDb);

		const double rates[] = {22050.0, 44100.0, 48000.0, 88200.0, 96000.0};
		const double fs = rates[trial % 5];
		const int channels = 1 + trial % BassEngine::kMaxChannels;

		BassEngine engine;
		engine.prepare(fs, channels);
		engine.setParams(p);

		Buffer buf(channels, 8192);
		for (int ch = 0; ch < channels; ++ch) {
			buf.data[ch] = noise(1.0, 8192, static_cast<uint32_t>(trial * 31 + ch + 1));
			auto tone = sine(40.0 + 30.0 * ch, 0.9, fs, 8192);
			for (size_t i = 0; i < 8192; ++i)
				buf.data[ch][i] = 0.5f * (buf.data[ch][i] + tone[i]);
		}
		processInChunks(engine, buf, 1024);

		// Change everything mid-stream, then keep going.
		p.shelfGainDb = pick(ranges::kShelfGainDb);
		p.subFreqHz = pick(ranges::kSubFreqHz);
		p.harmonicsLowHz = pick(ranges::kHarmonicsLowHz);
		engine.setParams(p);
		processInChunks(engine, buf, 333);

		const double ceiling = std::pow(10.0, sanitize(p).limiterCeilingDb / 20.0);
		for (int ch = 0; ch < channels; ++ch) {
			for (float v : buf.data[ch]) {
				REQUIRE(std::isfinite(v));
				if (p.limiterEnabled)
					REQUIRE(std::fabs(v) <= ceiling + 1e-6);
				else
					REQUIRE(std::fabs(v) < 1000.0f);
			}
		}
		REQUIRE(engine.getRecoveryCount() == 0);
	}
}

// ------------------------------------------------- channels & sample rate

TEST_CASE("Each channel is filtered independently", "[engine][channels]")
{
	const BassParams p = shelfOnly(9.0, 120.0);
	BassEngine multi;
	multi.prepare(48000.0, 6);
	multi.setParams(p);
	multi.snapToTargets();
	Buffer buf(6, 24000);
	for (int ch = 0; ch < 6; ++ch)
		buf.data[ch] = sine(50.0 + 200.0 * ch, 0.1, 48000.0, 24000);
	const Buffer input = buf;
	processInChunks(multi, buf);

	for (int ch = 0; ch < 6; ++ch) {
		BassEngine mono;
		mono.prepare(48000.0, 1);
		mono.setParams(p);
		mono.snapToTargets();
		Buffer single(1, 24000);
		single.data[0] = input.data[ch];
		processInChunks(mono, single);
		double maxDiff = 0.0;
		for (size_t i = 0; i < 24000; ++i)
			maxDiff = std::fmax(maxDiff, std::fabs(single.data[0][i] - buf.data[ch][i]));
		CHECK(maxDiff < 1e-7);
	}
}

TEST_CASE("Re-preparing at a new sample rate keeps the response correct", "[engine][channels]")
{
	const BassParams p = shelfOnly(10.0, 100.0);
	BassEngine engine;
	engine.prepare(44100.0, 2);
	engine.setParams(p);
	engine.prepare(96000.0, 2);
	CHECK(engine.getSampleRate() == 96000.0);
	CHECK(engine.getNumChannels() == 2);
	CHECK(engine.getParams().shelfGainDb == 10.0);
	CHECK_THAT(measureGainDb(p, 30.0, 96000.0), WithinAbs(engine.linearResponseDb(30.0), 0.15));
}

TEST_CASE("Invalid sample rates fall back to 48 kHz", "[engine][channels]")
{
	BassEngine engine;
	engine.prepare(0.0, 2);
	CHECK(engine.getSampleRate() == 48000.0);
	engine.prepare(std::nan(""), 0);
	CHECK(engine.getSampleRate() == 48000.0);
	CHECK(engine.getNumChannels() == 1);
}

// ------------------------------------------------------------ performance

TEST_CASE("CPU cost is a tiny fraction of real time", "[engine][benchmark]")
{
	BassEngine engine;
	engine.prepare(48000.0, 2);
	engine.setParams(findPreset("extreme")->params); // every stage active
	engine.snapToTargets();

	const size_t seconds = 20;
	Buffer buf(2, 48000);
	buf.data[0] = missingFundamental(100.0, 3, 9, 0.4, 48000.0, 48000);
	buf.data[1] = noise(0.3, 48000, 5);

	const auto start = std::chrono::steady_clock::now();
	for (size_t s = 0; s < seconds; ++s)
		processInChunks(engine, buf, 1024);
	const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	const double percentOfRealtime = 100.0 * elapsed / static_cast<double>(seconds);
	WARN("Stereo 48 kHz, every stage active: " << percentOfRealtime << " % of one core");
	CHECK(percentOfRealtime < 5.0); // generous for slow CI runners; typically ~0.3 %
}
