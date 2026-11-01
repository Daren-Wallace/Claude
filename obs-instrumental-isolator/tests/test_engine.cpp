/*
Instrumental Isolator for OBS - full chain tests
Copyright (C) 2026 Daren Wallace
SPDX-License-Identifier: GPL-2.0-or-later

The separation tests use testmix, where every stem lives on its own
frequencies, so the level of the vocal and of each instrument after
processing can be measured exactly.
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "dsp/crossover.hpp"
#include "dsp/isolator_engine.hpp"
#include "test_helpers.hpp"

#include <chrono>
#include <cstring>
#include <limits>
#include <random>

using namespace isolator;
using namespace testutil;

namespace {

constexpr double kFs = 48000.0;

struct Separation {
	double vocal, guitar, keys, pad, bass, kick;
};

Separation measure(const IsolatorParams &p, const testmix::Mix &m)
{
	const testmix::Stereo out = runPreset(p, m.mix, m.sampleRate);
	const size_t len = 96000;
	const size_t start = out.left.size() - len; // after the predictor has converged
	Separation s;
	s.vocal = testmix::stemChangeDb(m.vocalStem, m.mix, out, m.sampleRate, start, len);
	s.guitar = testmix::stemChangeDb(m.guitarStem, m.mix, out, m.sampleRate, start, len);
	s.keys = testmix::stemChangeDb(m.keysStem, m.mix, out, m.sampleRate, start, len);
	s.pad = testmix::stemChangeDb(m.padStem, m.mix, out, m.sampleRate, start, len);
	s.bass = testmix::stemChangeDb(m.bassStem, m.mix, out, m.sampleRate, start, len);
	s.kick = testmix::stemChangeDb(m.kickStem, m.mix, out, m.sampleRate, start, len);
	return s;
}

const testmix::Mix &standardMix()
{
	static const testmix::Mix m = testmix::make(kFs, 6.0);
	return m;
}

} // namespace

// ------------------------------------------------------------- separation

TEST_CASE("Karaoke preset removes the vocal and keeps the instruments", "[engine][separation]")
{
	const Separation s = measure(presetParams("karaoke"), standardMix());
	INFO("vocal " << s.vocal << " guitar " << s.guitar << " keys " << s.keys << " pad " << s.pad << " bass "
		      << s.bass << " kick " << s.kick);
	CHECK(s.vocal <= -15.0);           // plan target: >= 15 dB suppression
	CHECK(std::fabs(s.guitar) <= 1.5); // panned instruments within 1.5 dB
	CHECK(std::fabs(s.keys) <= 1.5);
	CHECK(std::fabs(s.bass) <= 0.5); // protected centre bass
	CHECK(std::fabs(s.kick) <= 0.5); // protected centre kick
	CHECK(s.pad >= -6.0);            // decorrelated ambience keeps its side part
}

TEST_CASE("Presets order from gentle to aggressive", "[engine][separation]")
{
	const Separation gentle = measure(presetParams("gentle"), standardMix());
	const Separation karaoke = measure(presetParams("karaoke"), standardMix());
	const Separation aggressive = measure(presetParams("aggressive"), standardMix());
	CHECK(gentle.vocal > karaoke.vocal);
	CHECK(karaoke.vocal > aggressive.vocal);
	CHECK(aggressive.vocal <= -20.0);
}

TEST_CASE("Separation also works with vibrato and a louder vocal", "[engine][separation]")
{
	const double vocalGain = GENERATE(1.0, 2.0);
	const testmix::Mix m = testmix::make(kFs, 6.0, true, vocalGain);
	const Separation s = measure(presetParams("karaoke"), m);
	INFO("gain " << vocalGain << " vocal " << s.vocal << " guitar " << s.guitar << " keys " << s.keys);
	CHECK(s.vocal <= -15.0);
	CHECK(std::fabs(s.guitar) <= 2.5);
	CHECK(std::fabs(s.keys) <= 2.5);
}

TEST_CASE("Pitch-tracked remover adds suppression on a prominent vocal", "[engine][pitch]")
{
	const testmix::Mix m = testmix::make(kFs, 6.0, false, 2.0);
	IsolatorParams off = presetParams("karaoke");
	off.pitchEnabled = false;
	IsolatorParams on = presetParams("karaoke");
	on.pitchAmount = 100.0;
	const Separation a = measure(off, m);
	const Separation b = measure(on, m);
	INFO("without comb " << a.vocal << " dB, with comb " << b.vocal << " dB");
	CHECK(b.vocal < a.vocal - 1.5);
}

TEST_CASE("Vocals-only mode outputs the vocal and drops the instruments", "[engine][separation]")
{
	const Separation s = measure(presetParams("vocals"), standardMix());
	INFO("vocal " << s.vocal << " guitar " << s.guitar << " keys " << s.keys << " bass " << s.bass);
	CHECK(s.vocal >= -3.0);  // the vocal survives
	CHECK(s.guitar <= -6.0); // panned instruments drop well back (partials closer than
	CHECK(s.keys <= -6.0);   // the predictor's ~190 Hz resolution leak through)
	CHECK(s.bass <= -20.0);  // protected bass belongs to the instrumental
}

// --------------------------------------------------------- latency & phase

TEST_CASE("Zero latency: output responds on the same sample as the input", "[engine][latency]")
{
	IsolatorEngine e;
	e.prepare(kFs, 2);
	e.setParams(presetParams("karaoke"));
	e.snapToTargets();
	std::vector<float> l(4800, 0.0f), r(4800, 0.0f);
	l[0] = 0.5f; // a click in the left channel only (pure side + mid)
	float *ch[2] = {l.data(), r.data()};
	e.process(ch, 2, 4800);
	size_t peakIndex = 0;
	for (size_t i = 1; i < l.size(); ++i)
		if (std::fabs(l[i]) > std::fabs(l[peakIndex]))
			peakIndex = i;
	CHECK(std::fabs(l[0]) > 1e-3);
	CHECK(peakIndex < 48); // < 1 ms; any FFT/AI method would output nothing yet
}

TEST_CASE("Strength 0 is transparent: output equals an all-pass of the input", "[engine][transparency]")
{
	IsolatorParams p = presetParams("karaoke");
	p.strength = 0.0;
	p.pitchEnabled = false;
	p.stereoWidth = 100.0;
	p.outputGainDb = 0.0;
	IsolatorEngine e;
	e.prepare(kFs, 2);
	e.setParams(p);
	e.snapToTargets();

	const testmix::Mix &m = standardMix();
	testmix::Stereo out = m.mix;
	processStereo(e, out);

	const dsp::BandLayout layout = dsp::BandLayout::twoThirdOctave(kFs);
	dsp::AllpassChain apL, apR;
	apL.prepare(kFs, layout);
	apR.prepare(kFs, layout);
	double maxErr = 0.0;
	for (size_t i = 0; i < out.left.size(); ++i) {
		maxErr = std::fmax(maxErr, std::fabs(out.left[i] - apL.process(m.mix.left[i])));
		maxErr = std::fmax(maxErr, std::fabs(out.right[i] - apR.process(m.mix.right[i])));
	}
	CHECK(maxErr < 1e-5); // float rounding only: flat magnitude, nothing removed
}

// -------------------------------------------------------------- bypass

TEST_CASE("Bypass leaves audio bit-for-bit untouched", "[engine][bypass]")
{
	IsolatorParams p = presetParams("aggressive");
	p.bypass = true;
	IsolatorEngine e;
	e.prepare(kFs, 2);
	e.setParams(p);
	e.snapToTargets();
	testmix::Stereo s = standardMix().mix;
	const testmix::Stereo original = s;
	processStereo(e, s);
	CHECK(s.left == original.left);
	CHECK(s.right == original.right);
}

TEST_CASE("Toggling bypass or output mode never clicks", "[engine][bypass]")
{
	for (int which = 0; which < 2; ++which) {
		IsolatorParams p = presetParams("karaoke");
		IsolatorEngine e;
		e.prepare(kFs, 2);
		e.setParams(p);
		e.snapToTargets();
		testmix::Stereo s = testmix::make(kFs, 2.0).mix;
		const size_t n = s.left.size();
		for (size_t off = 0; off < n; off += 480) {
			if (off == 48000) {
				if (which == 0)
					p.bypass = true;
				else
					p.outputMode = OutputMode::Vocals;
				e.setParams(p);
			}
			float *ch[2] = {s.left.data() + off, s.right.data() + off};
			e.process(ch, 2, 480);
		}
		// The signal before and after the switch differ (vocal in or out),
		// so compare the 20 ms transition against the steeper of the two
		// steady states. A click would be far larger than either.
		double before = 0.0, after = 0.0, transition = 0.0;
		for (size_t i = 24001; i < n; ++i) {
			const double d = std::fabs(s.left[i] - s.left[i - 1]);
			if (i < 48000)
				before = std::fmax(before, d);
			else if (i < 48000 + 1440)
				transition = std::fmax(transition, d);
			else
				after = std::fmax(after, d);
		}
		INFO((which == 0 ? "bypass" : "mode")
		     << " before " << before << " after " << after << " transition " << transition);
		CHECK(transition <= std::fmax(before, after) * 1.2);
	}
}

// ------------------------------------------------------- mono & surround

TEST_CASE("Mono and dual-mono input are never silenced", "[engine][mono]")
{
	// A mono source has everything "in the centre": centre removal must
	// back off instead of deleting the whole signal.
	const auto music = voice(150.0, 15, 0.3, kFs, 144000);
	auto noiseBed = noise(0.05, 144000, 6);
	std::vector<float> x(music.size());
	for (size_t i = 0; i < x.size(); ++i)
		x[i] = music[i] + noiseBed[i];

	IsolatorParams p = presetParams("aggressive");
	p.pitchEnabled = false;

	IsolatorEngine stereo;
	stereo.prepare(kFs, 2);
	stereo.setParams(p);
	stereo.snapToTargets();
	testmix::Stereo dual(x.size());
	dual.left = x;
	dual.right = x;
	processStereo(stereo, dual);
	CHECK(toDb(rms(dual.left, 96000) / rms(x, 96000)) > -1.5);
	CHECK(stereo.getMonoFactor() > 0.99);

	IsolatorEngine mono;
	mono.prepare(kFs, 1);
	mono.setParams(p);
	mono.snapToTargets();
	std::vector<float> m = x;
	for (size_t off = 0; off < m.size(); off += 1024) {
		float *ch = m.data() + off;
		mono.process(&ch, 1, static_cast<int>(std::min<size_t>(1024, m.size() - off)));
	}
	CHECK(toDb(rms(m, 96000) / rms(x, 96000)) > -1.5);
}

TEST_CASE("On mono input the pitch remover still reduces a sung note", "[engine][mono][pitch]")
{
	const auto v = voice(196.0, 16, 0.3, kFs, 144000);
	IsolatorParams p = presetParams("karaoke");
	p.pitchAmount = 100.0;
	p.pitchHarmonics = 30.0;
	IsolatorEngine e;
	e.prepare(kFs, 1);
	e.setParams(p);
	e.snapToTargets();
	std::vector<float> m = v;
	for (size_t off = 0; off < m.size(); off += 1024) {
		float *ch = m.data() + off;
		e.process(&ch, 1, static_cast<int>(std::min<size_t>(1024, m.size() - off)));
	}
	const double change = toDb(rms(m, 96000) / rms(v, 96000));
	INFO("mono sung note change " << change << " dB, tracked f0 " << e.getTrackedF0());
	CHECK(change < -3.0); // plan: a measurable 3-6 dB on mono
	CHECK(std::fabs(e.getTrackedF0() - 196.0) < 3.0);
}

TEST_CASE("Surround: the centre channel is removed in instrumental mode", "[engine][surround]")
{
	// 5.1 layout: FL FR FC LFE RL RR
	IsolatorEngine e;
	e.prepare(kFs, 6, 2);
	e.setParams(presetParams("aggressive"));
	e.snapToTargets();
	std::vector<std::vector<float>> ch(6, std::vector<float>(48000, 0.0f));
	ch[2] = sine(300.0, 0.3, kFs, 48000); // dialogue/vocal in FC
	ch[3] = sine(50.0, 0.3, kFs, 48000);  // LFE must be untouched
	const auto lfe = ch[3];
	float *ptr[6];
	for (int c = 0; c < 6; ++c)
		ptr[c] = ch[static_cast<size_t>(c)].data();
	e.process(ptr, 6, 48000);
	CHECK(rms(ch[2], 24000) < 0.01);
	CHECK(ch[3] == lfe);
}

// ---------------------------------------------------------- robustness

TEST_CASE("Silence in gives silence out", "[engine][robustness]")
{
	IsolatorEngine e;
	e.prepare(kFs, 2);
	e.setParams(presetParams("aggressive", true));
	e.snapToTargets();
	testmix::Stereo s(48000);
	processStereo(e, s);
	CHECK(peak(s.left) == 0.0);
	CHECK(peak(s.right) == 0.0);
}

TEST_CASE("Auto makeup restores level without clipping", "[engine][makeup]")
{
	IsolatorEngine e;
	e.prepare(kFs, 2);
	e.setParams(presetParams("aggressive", true));
	e.snapToTargets();
	testmix::Mix m = testmix::make(kFs, 6.0, false, 3.0); // vocal-heavy: lots of level lost
	for (float &v : m.mix.left)
		v *= 2.5f;
	for (float &v : m.mix.right)
		v *= 2.5f;
	testmix::Stereo out = m.mix;
	processStereo(e, out);
	CHECK(e.getMakeupDb() > 1.0);
	CHECK(e.getMakeupDb() <= 6.03);
	const double ceiling = std::pow(10.0, -1.0 / 20.0);
	CHECK(peak(out.left) <= ceiling + 1e-6);
	CHECK(peak(out.right) <= ceiling + 1e-6);
}

TEST_CASE("Chunk size does not change the result", "[engine][robustness]")
{
	auto run = [](size_t chunk) {
		IsolatorEngine e;
		e.prepare(kFs, 2);
		e.setParams(presetParams("karaoke"));
		e.snapToTargets();
		testmix::Stereo s = testmix::make(kFs, 1.0).mix;
		processStereo(e, s, chunk);
		return s;
	};
	const testmix::Stereo reference = run(48000);
	for (size_t chunk : {1, 15, 16, 17, 480, 1024}) {
		const testmix::Stereo s = run(chunk);
		double maxDiff = 0.0;
		for (size_t i = 0; i < s.left.size(); ++i)
			maxDiff = std::fmax(maxDiff, std::fabs(s.left[i] - reference.left[i]));
		INFO("chunk " << chunk << " max diff " << maxDiff);
		CHECK(maxDiff == 0.0); // block updates run on a global grid: bit-identical
	}
}

TEST_CASE("A NaN in the input never poisons the stream", "[engine][robustness]")
{
	IsolatorEngine e;
	e.prepare(kFs, 2);
	e.setParams(presetParams("karaoke", true));
	e.snapToTargets();
	testmix::Stereo s = testmix::make(kFs, 0.5).mix;
	s.left[100] = std::numeric_limits<float>::quiet_NaN();
	s.right[200] = std::numeric_limits<float>::infinity();
	processStereo(e, s, 480);
	for (size_t i = 0; i < s.left.size(); ++i) {
		if (i == 100 || i == 200)
			continue;
		REQUIRE(std::isfinite(s.left[i]));
		REQUIRE(std::isfinite(s.right[i]));
	}
	CHECK(e.getRecoveryCount() == 0);
}

TEST_CASE("Randomised stress: any parameters, rates and layouts stay finite", "[engine][robustness][stress]")
{
	std::mt19937 rng(20261101);
	std::uniform_real_distribution<double> u(0.0, 1.0);
	auto pick = [&](const ParamRange &r) {
		return r.min + (r.max - r.min) * u(rng);
	};
	const double rates[] = {22050.0, 44100.0, 48000.0, 96000.0};
	for (int trial = 0; trial < 24; ++trial) {
		IsolatorParams p;
		p.outputMode = u(rng) > 0.7 ? OutputMode::Vocals : OutputMode::Instrumental;
		p.strength = pick(ranges::kStrength);
		p.centreWidth = pick(ranges::kCentreWidth);
		p.responseMs = pick(ranges::kResponseMs);
		p.protectLowHz = pick(ranges::kProtectLowHz);
		p.protectHighHz = pick(ranges::kProtectHighHz);
		p.transientProtect = pick(ranges::kTransientProtect);
		p.pitchEnabled = u(rng) > 0.5;
		p.pitchAmount = pick(ranges::kPitchAmount);
		p.pitchHarmonics = pick(ranges::kPitchHarmonics);
		p.voiceLowHz = pick(ranges::kVoiceLowHz);
		p.voiceHighHz = pick(ranges::kVoiceHighHz);
		p.stereoWidth = pick(ranges::kStereoWidth);
		p.autoMakeup = u(rng) > 0.5;
		p.outputGainDb = pick(ranges::kOutputGainDb);

		const double fs = rates[trial % 4];
		const int channels = 1 + trial % IsolatorEngine::kMaxChannels;
		IsolatorEngine e;
		e.prepare(fs, channels, channels >= 6 ? 2 : -1);
		e.setParams(p);
		std::vector<std::vector<float>> ch(static_cast<size_t>(channels));
		for (int c = 0; c < channels; ++c)
			ch[static_cast<size_t>(c)] = noise(0.9, 16384, static_cast<uint32_t>(trial * 13 + c + 1));
		std::vector<float *> ptr;
		for (auto &v : ch)
			ptr.push_back(v.data());
		for (int off = 0; off < 16384; off += 1024) {
			std::vector<float *> part;
			for (float *q : ptr)
				part.push_back(q + off);
			e.process(part.data(), channels, 1024);
		}
		for (auto &v : ch)
			for (float s : v)
				REQUIRE(std::isfinite(s));
		REQUIRE(e.getRecoveryCount() == 0);
	}
}

TEST_CASE("Zero-length, null and oversized calls are safe", "[engine][robustness]")
{
	IsolatorEngine e;
	e.prepare(kFs, 2);
	e.process(nullptr, 2, 1024);
	float data[32] = {0.5f};
	float *ptr = data;
	e.process(&ptr, 1, 0);
	e.process(&ptr, 0, 32);
	float *nullCh[2] = {data, nullptr};
	e.process(nullCh, 2, 32);
	CHECK(data[0] == 0.5f);
	e.prepare(0.0, 99);
	CHECK(e.getSampleRate() == 48000.0);
	CHECK(e.getNumChannels() == IsolatorEngine::kMaxChannels);
}

// ------------------------------------------------------------ performance

TEST_CASE("CPU cost is a small fraction of real time", "[engine][benchmark]")
{
	IsolatorEngine e;
	e.prepare(kFs, 2);
	e.setParams(presetParams("aggressive", true)); // every stage on
	e.snapToTargets();
	testmix::Stereo s = testmix::make(kFs, 2.0).mix;
	const int seconds = 10;
	const auto start = std::chrono::steady_clock::now();
	for (int i = 0; i < seconds / 2; ++i)
		processStereo(e, s);
	const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	const double percent = 100.0 * elapsed / seconds;
	WARN("Stereo 48 kHz, every stage active: " << percent << " % of one core");
	CHECK(percent < 15.0); // generous for slow CI runners; ~4 % on a desktop core
}
