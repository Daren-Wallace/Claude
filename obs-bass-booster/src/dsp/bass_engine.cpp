/*
Bass Booster for OBS
Copyright (C) 2026 Daren Wallace

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "bass_engine.hpp"

#include <cmath>
#include <initializer_list>

namespace bassbooster {

using namespace dsp;

namespace {

// Gain applied to the generators at 100 % "Amount". Calibrated so that 100 %
// generates roughly as much new bass as the band it was derived from.
constexpr double kHarmonicsFullScale = 2.0;
constexpr double kSubFullScale = 1.0;

double effectivePunchGain(const BassParams &p)
{
	return p.punchEnabled ? p.punchGainDb : 0.0;
}

double effectiveHarmonics(const BassParams &p)
{
	return p.harmonicsEnabled ? p.harmonicsAmount * 0.01 * kHarmonicsFullScale : 0.0;
}

double effectiveSub(const BassParams &p)
{
	return p.subEnabled ? p.subAmount * 0.01 * kSubFullScale : 0.0;
}

bool staticSectionsDiffer(const BassParams &a, const BassParams &b)
{
	return a.harmonicsLowHz != b.harmonicsLowHz || a.harmonicsHighHz != b.harmonicsHighHz ||
	       a.harmonicsDrive != b.harmonicsDrive || a.subFreqHz != b.subFreqHz || a.subTightness != b.subTightness ||
	       a.rumbleFreqHz != b.rumbleFreqHz || a.limiterEnabled != b.limiterEnabled ||
	       a.limiterCeilingDb != b.limiterCeilingDb || a.limiterReleaseMs != b.limiterReleaseMs;
}

} // namespace

BassEngine::BassEngine()
{
	prepare(48000.0, 2);
	snapToTargets();
}

void BassEngine::prepare(double sampleRate, int numChannels)
{
	fs = (sampleRate > 1000.0 && std::isfinite(sampleRate)) ? sampleRate : 48000.0;
	channelCount = numChannels < 1 ? 1 : (numChannels > kMaxChannels ? kMaxChannels : numChannels);

	for (Smoother *s : {&sShelfFreq, &sShelfGain, &sShelfQ, &sPunchFreq, &sPunchGain, &sPunchQ, &sInputGain,
			    &sOutputGain, &sMix, &sHarmonics, &sSub})
		s->setTimeConstant(fs, kSmoothingMs);
	bypassFade.setRampTime(fs, kBypassFadeMs);

	harmonics.prepare(fs);
	sub.prepare(fs);
	limiter.prepare(fs);
	staticDirty = true;

	setParams(params);
	snapToTargets();
	reset();
}

void BassEngine::setParams(const BassParams &newParams)
{
	const BassParams p = sanitize(newParams);
	if (staticSectionsDiffer(p, params))
		staticDirty = true;
	params = p;

	sShelfFreq.setTarget(p.shelfFreqHz);
	sShelfGain.setTarget(p.shelfGainDb);
	sShelfQ.setTarget(p.shelfQ);
	sPunchFreq.setTarget(p.punchFreqHz);
	sPunchGain.setTarget(effectivePunchGain(p));
	sPunchQ.setTarget(p.punchQ);

	sInputGain.setTarget(dbToGain(p.inputGainDb));
	sOutputGain.setTarget(dbToGain(p.outputGainDb));
	sMix.setTarget(p.mix * 0.01);
	sHarmonics.setTarget(effectiveHarmonics(p));
	sSub.setTarget(effectiveSub(p));
	bypassFade.setTarget(p.bypass ? 0.0 : 1.0);
}

void BassEngine::snapToTargets()
{
	for (Smoother *s : {&sShelfFreq, &sShelfGain, &sShelfQ, &sPunchFreq, &sPunchGain, &sPunchQ, &sInputGain,
			    &sOutputGain, &sMix, &sHarmonics, &sSub})
		s->snap(s->getTarget());
	bypassFade.snap(bypassFade.getTarget());
	updateFilterCoefficients(true);
	configureStaticSections();
}

void BassEngine::reset()
{
	for (int ch = 0; ch < kMaxChannels; ++ch) {
		shelf[ch].reset();
		punch[ch].reset();
		rumble[ch].reset();
	}
	harmonics.reset();
	sub.reset();
	limiter.reset();
	needsReset = false;
}

double BassEngine::getLimiterGainReductionDb() const
{
	return gainToDb(limiter.currentGain());
}

void BassEngine::configureStaticSections()
{
	harmonics.setup(params.harmonicsLowHz, params.harmonicsHighHz, params.harmonicsDrive * 0.01);
	sub.setup(params.subFreqHz, params.subTightness * 0.01);
	for (int ch = 0; ch < kMaxChannels; ++ch)
		rumble[ch].setup(Butterworth4::Type::HighPass, fs, params.rumbleFreqHz);
	limiter.setup(params.limiterEnabled, params.limiterCeilingDb, params.limiterReleaseMs);
	staticDirty = false;
}

void BassEngine::updateFilterCoefficients(bool force)
{
	const bool moving = !(sShelfFreq.settled() && sShelfGain.settled() && sShelfQ.settled() &&
			      sPunchFreq.settled() && sPunchGain.settled() && sPunchQ.settled());
	if (!moving && !force)
		return;

	const double shF = sShelfFreq.advance(kSubBlock);
	const double shG = sShelfGain.advance(kSubBlock);
	const double shQ = sShelfQ.advance(kSubBlock);
	const double puF = sPunchFreq.advance(kSubBlock);
	const double puG = sPunchGain.advance(kSubBlock);
	const double puQ = sPunchQ.advance(kSubBlock);

	const BiquadCoeffs shelfC = BiquadCoeffs::lowShelf(fs, shF, shQ, shG);
	const BiquadCoeffs punchC = BiquadCoeffs::peaking(fs, puF, puQ, puG);
	for (int ch = 0; ch < kMaxChannels; ++ch) {
		shelf[ch].setCoeffs(shelfC);
		punch[ch].setCoeffs(punchC);
	}
}

void BassEngine::process(float *const *channels, int numChannels, int frames)
{
	if (!channels || frames <= 0 || numChannels <= 0)
		return;

	const int nch = numChannels < channelCount ? numChannels : channelCount;
	for (int ch = 0; ch < nch; ++ch) {
		if (!channels[ch])
			return;
	}

	// Fully bypassed and settled: leave the buffer bit-for-bit untouched.
	if (bypassFade.settled() && bypassFade.value() == 0.0) {
		needsReset = true;
		return;
	}
	if (needsReset)
		reset();

	if (staticDirty)
		configureStaticSections();

	ScopedFlushDenormals ftz;

	for (int offset = 0; offset < frames; offset += kSubBlock) {
		const int n = (frames - offset) < kSubBlock ? (frames - offset) : kSubBlock;
		updateFilterCoefficients(false);
		processBlock(channels, nch, offset, n);
	}

	// Housekeeping once per buffer.
	bool finite = harmonics.stateIsFinite() && sub.stateIsFinite();
	for (int ch = 0; ch < nch; ++ch) {
		shelf[ch].flushDenormals();
		punch[ch].flushDenormals();
		rumble[ch].flushDenormals();
		finite = finite && shelf[ch].stateIsFinite() && punch[ch].stateIsFinite() && rumble[ch].stateIsFinite();
	}
	harmonics.flushDenormals();
	sub.flushDenormals();

	if (!finite) {
		// Should never happen with sanitised parameters, but a single NaN
		// from upstream must not silence a live stream forever.
		++recoveries;
		reset();
	}
}

void BassEngine::processBlock(float *const *channels, int nch, int offset, int frames)
{
	const double invChannels = 1.0 / static_cast<double>(nch);
	const double ceiling = limiter.getCeiling();
	const bool limiterOn = limiter.isEnabled();

	for (int i = offset; i < offset + frames; ++i) {
		const double inGain = sInputGain.next();
		const double outGain = sOutputGain.next();
		const double mix = sMix.next();
		const double harmAmt = sHarmonics.next();
		const double subAmt = sSub.next();
		const double fade = bypassFade.next();

		// Mono sum feeds the generators.
		double mono = 0.0;
		bool inputFinite = true;
		for (int ch = 0; ch < nch; ++ch) {
			const double x = channels[ch][i];
			inputFinite = inputFinite && std::isfinite(x);
			mono += x;
		}
		if (!inputFinite) {
			// Pass the corrupt frame through untouched rather than poisoning
			// every filter; the caller's own sanitising will deal with it.
			continue;
		}
		mono *= invChannels * inGain;

		// Generators always run (even at 0 %) so their envelopes and
		// trigger state are warm when the amount is raised.
		const double synth = harmAmt * harmonics.process(mono) + subAmt * sub.process(mono);

		double peak = 0.0;
		for (int ch = 0; ch < nch; ++ch) {
			const double dry = channels[ch][i];
			double y = dry * inGain;
			y = shelf[ch].process(y);
			y = punch[ch].process(y);
			y += synth;
			y = rumble[ch].process(y);
			double out = dry + mix * (y - dry);
			out *= outGain;
			frameWet[ch] = out;
			const double a = std::fabs(out);
			if (a > peak)
				peak = a;
		}

		const double g = limiter.process(peak);
		for (int ch = 0; ch < nch; ++ch) {
			double out = frameWet[ch] * g;
			if (limiterOn) {
				// Guard against floating point rounding at the ceiling.
				if (out > ceiling)
					out = ceiling;
				else if (out < -ceiling)
					out = -ceiling;
			}
			const double dry = channels[ch][i];
			channels[ch][i] = static_cast<float>(dry + fade * (out - dry));
		}
	}
}

double BassEngine::linearResponseDb(double freqHz) const
{
	const BiquadCoeffs shelfC =
		BiquadCoeffs::lowShelf(fs, sShelfFreq.getTarget(), sShelfQ.getTarget(), sShelfGain.getTarget());
	const BiquadCoeffs punchC =
		BiquadCoeffs::peaking(fs, sPunchFreq.getTarget(), sPunchQ.getTarget(), sPunchGain.getTarget());
	// Complex response is needed to mix with the dry path correctly; the
	// chain is minimum-phase, so evaluate each section as a complex number.
	struct Cx {
		double re, im;
	};
	auto mul = [](Cx a, Cx b) {
		return Cx{a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
	};
	auto section = [&](const BiquadCoeffs &c) {
		const double w = 2.0 * kPi * freqHz / fs;
		const Cx z1{std::cos(-w), std::sin(-w)};
		const Cx z2{std::cos(-2.0 * w), std::sin(-2.0 * w)};
		const Cx num{c.b0 + c.b1 * z1.re + c.b2 * z2.re, c.b1 * z1.im + c.b2 * z2.im};
		const Cx den{1.0 + c.a1 * z1.re + c.a2 * z2.re, c.a1 * z1.im + c.a2 * z2.im};
		const double d = den.re * den.re + den.im * den.im;
		return Cx{(num.re * den.re + num.im * den.im) / d, (num.im * den.re - num.re * den.im) / d};
	};

	Cx h = section(shelfC);
	h = mul(h, section(punchC));
	h = mul(h, section(BiquadCoeffs::highPass(fs, params.rumbleFreqHz, kButterworth4Q1)));
	h = mul(h, section(BiquadCoeffs::highPass(fs, params.rumbleFreqHz, kButterworth4Q2)));
	const double inGain = dbToGain(params.inputGainDb);
	h.re *= inGain;
	h.im *= inGain;

	const double mix = params.mix * 0.01;
	Cx total{(1.0 - mix) + mix * h.re, mix * h.im};
	const double outGain = dbToGain(params.outputGainDb);
	const double mag = std::sqrt(total.re * total.re + total.im * total.im) * outGain;
	return gainToDb(mag > 1e-12 ? mag : 1e-12);
}

} // namespace bassbooster
