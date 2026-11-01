/*
Instrumental Isolator for OBS
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

#include "isolator_engine.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace isolator {

using namespace dsp;

namespace {

// NLMS step size: ~1 s convergence for 256 taps. Pan positions in a mix are
// static, so slow, low-noise adaptation is the right trade-off.
constexpr double kPredictorStep = 0.003;

double smoothstep(double edge0, double edge1, double x)
{
	if (edge1 <= edge0)
		return x >= edge1 ? 1.0 : 0.0;
	double t = (x - edge0) / (edge1 - edge0);
	t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
	return t * t * (3.0 - 2.0 * t);
}

// Protection weight for a band spanning [lowerEdge, upperEdge]: a band is
// only processed once its *lower* edge clears protectLow and its *upper*
// edge stays below protectHigh, so crossover skirts never drag protected
// bass or treble into the removal. Soft one-sixth-octave transitions.
double protectionWeight(double lowerEdge, double upperEdge, double lowHz, double highHz)
{
	const double edge = std::pow(2.0, 1.0 / 6.0);
	const double low = smoothstep(lowHz / edge, lowHz * edge, lowerEdge);
	const double high = 1.0 - smoothstep(highHz / edge, highHz * edge, upperEdge);
	return low * high;
}

// Vocal share of the residual R in one band.
//
// After the predictor, R holds the vocal plus the mid part of decorrelated
// stereo content (pads, reverb, room), which puts equal power into M and S.
// Every panned instrument has |side| <= |mid| (its left and right gains share
// a sign), and its mid part is what the predictor captured in P, so the side
// power that panned instruments can account for is at most <P^2>. Whatever
// side power exceeds that must be decorrelated ambience:
//
//     Pd >= <S^2> - <P^2>                (a lower bound, immune to how many
//                                         instruments share the band or how
//                                         they are panned)
//     g   = (<R^2> - alpha * Pd) / <R^2>
//
// Centre Width sets alpha: narrow (2.0) protects ambience strongly, wide
// (0.5) removes more of what is near the centre.
double vocalShare(double pr, double ps, double pp, double width01)
{
	if (pr < 1e-14)
		return 0.0;
	double pd = ps - pp;
	pd = pd < 0.0 ? 0.0 : pd;
	const double alpha = 2.0 - 1.5 * width01;
	const double g = (pr - alpha * pd) / pr;
	return g < 0.0 ? 0.0 : (g > 1.0 ? 1.0 : g);
}

// Strength: 0..50 % scales the removal from none up to the estimate;
// 50..100 % keeps full scale and over-subtracts by up to 2x, trading some
// ambience for deeper vocal removal.
void strengthMapping(double strength01, double &scale, double &overSubtract)
{
	scale = strength01 >= 0.5 ? 1.0 : strength01 * 2.0;
	overSubtract = strength01 <= 0.5 ? 1.0 : 1.0 + (strength01 - 0.5) * 2.0;
}

} // namespace

IsolatorEngine::IsolatorEngine()
{
	prepare(48000.0, 2, -1);
	snapToTargets();
}

void IsolatorEngine::prepare(double sampleRate, int numChannels, int centreChannel)
{
	fs = (sampleRate > 1000.0 && std::isfinite(sampleRate)) ? sampleRate : 48000.0;
	channelCount = numChannels < 1 ? 1 : (numChannels > kMaxChannels ? kMaxChannels : numChannels);
	centreIndex = (centreChannel >= 2 && centreChannel < channelCount) ? centreChannel : -1;

	predictor.prepare(fs);
	predictor.setStepSize(kPredictorStep);
	layout = BandLayout::twoThirdOctave(fs);
	splitR.prepare(fs, layout);
	splitS.prepare(fs, layout);
	splitP.prepare(fs, layout);
	combVocal.prepare(fs, layout);
	apM.prepare(fs, layout);
	apS.prepare(fs, layout);

	pitch.prepare(fs);
	comb.prepare(fs);
	transients.prepare(fs);
	limiter.prepare(fs, -1.0, 150.0);

	for (Smoother *s : {&sStrength, &sWidth, &sOutGain, &sPitchAmount})
		s->setTimeConstant(fs, 15.0);
	sMakeup.setTimeConstant(fs, 200.0);
	pitchGate.setTimeConstant(fs, 15.0);
	modeFade.setRampTime(fs, kFadeMs);
	bypassFade.setRampTime(fs, kFadeMs);

	gainCoeff = onePoleCoeff(fs, 5.0);
	monoCoeff = onePoleCoeff(fs, 300.0);
	powerCoeff = onePoleCoeff(fs, 1500.0);

	dirty = true;
	setParams(params);
	snapToTargets();
	reset();
}

void IsolatorEngine::setParams(const IsolatorParams &newParams)
{
	params = sanitize(newParams);
	sStrength.setTarget(params.strength * 0.01);
	sWidth.setTarget(params.stereoWidth * 0.01);
	sOutGain.setTarget(dbToGain(params.outputGainDb));
	sPitchAmount.setTarget(params.pitchEnabled ? params.pitchAmount * 0.01 : 0.0);
	modeFade.setTarget(params.outputMode == OutputMode::Vocals ? 1.0 : 0.0);
	bypassFade.setTarget(params.bypass ? 0.0 : 1.0);
	dirty = true;
}

void IsolatorEngine::snapToTargets()
{
	for (Smoother *s : {&sStrength, &sWidth, &sOutGain, &sPitchAmount})
		s->snap(s->getTarget());
	modeFade.snap(modeFade.getTarget());
	bypassFade.snap(bypassFade.getTarget());
	configure();
}

void IsolatorEngine::reset()
{
	predictor.reset();
	splitR.reset();
	splitS.reset();
	splitP.reset();
	combVocal.reset();
	apM.reset();
	apS.reset();
	pitch.reset();
	comb.reset();
	transients.reset();
	limiter.reset();
	for (BandState &b : bands) {
		const double protect = b.protect;
		b = BandState{};
		b.protect = protect;
	}
	pitchGate.snap(0.0);
	sMakeup.snap(1.0);
	monoPm = monoPs = 0.0;
	monoFactor = 0.0;
	powerIn = powerOut = 0.0;
	transientFactor = 1.0;
	blockPos = 0;
	needsReset = false;
}

void IsolatorEngine::configure()
{
	statCoeff = onePoleCoeff(fs, params.responseMs);
	for (int b = 0; b < layout.numBands(); ++b)
		bands[b].protect = protectionWeight(layout.lowerEdge(b), layout.upperEdge(b, fs), params.protectLowHz,
						    params.protectHighHz);
	transients.setAmount(params.transientProtect * 0.01);
	pitch.setRange(params.voiceLowHz, params.voiceHighHz);
	comb.setLimits(static_cast<int>(params.pitchHarmonics), params.protectLowHz,
		       std::fmin(params.protectHighHz, 6000.0));
	dirty = false;
}

double IsolatorEngine::getBandGain(int band) const
{
	if (band < 0 || band >= layout.numBands())
		return 0.0;
	return bands[band].gain;
}

double IsolatorEngine::getMakeupDb() const
{
	return gainToDb(sMakeup.value());
}

void IsolatorEngine::updateBlockState()
{
	if (monoPm > 1e-12) {
		const double ratio = monoPs / monoPm;
		const double r = std::log10(ratio > 1e-12 ? ratio : 1e-12); // -4 (mono) .. -3
		monoFactor = 1.0 - smoothstep(-4.0, -3.0, r);
	}

	const double width01 = params.centreWidth * 0.01;
	const double keep = 1.0 - monoFactor;
	for (int b = 0; b < layout.numBands(); ++b) {
		BandState &s = bands[b];
		s.target = vocalShare(s.pr, s.ps, s.pp, width01) * keep;
	}

	if (pitch.voiced())
		comb.setTargetF0(pitch.f0());
	pitchGate.setTarget(pitch.voiced() ? 1.0 : 0.0);
	comb.refresh(false, kSubBlock);

	double makeup = 1.0;
	if (params.autoMakeup && powerOut > 1e-12 && powerIn > 1e-12) {
		makeup = std::sqrt(powerIn / powerOut);
		makeup = makeup < 1.0 ? 1.0 : (makeup > 2.0 ? 2.0 : makeup); // at most +6 dB
	}
	sMakeup.setTarget(makeup);
}

void IsolatorEngine::process(float *const *channels, int numChannels, int frames)
{
	if (!channels || frames <= 0 || numChannels <= 0)
		return;
	const int nch = numChannels < channelCount ? numChannels : channelCount;
	for (int ch = 0; ch < nch; ++ch)
		if (!channels[ch])
			return;

	// Fully bypassed and settled: leave the buffer bit-for-bit untouched.
	if (bypassFade.settled() && bypassFade.value() == 0.0) {
		needsReset = true;
		return;
	}
	if (needsReset)
		reset();
	if (dirty)
		configure();

	ScopedFlushDenormals ftz;
	// Block-rate state is updated on a fixed 16-sample grid that carries
	// across calls, so the output is identical whatever buffer size the
	// host uses.
	int offset = 0;
	while (offset < frames) {
		if (blockPos == 0)
			updateBlockState();
		const int n = std::min(frames - offset, kSubBlock - blockPos);
		processBlock(channels, nch, offset, n);
		offset += n;
		blockPos = (blockPos + n) % kSubBlock;
	}

	splitR.flushDenormals();
	splitS.flushDenormals();
	splitP.flushDenormals();
	combVocal.flushDenormals();
	apM.flushDenormals();
	apS.flushDenormals();
	pitch.flushDenormals();
	comb.flushDenormals();
	transients.flushDenormals();

	const bool finite = predictor.stateIsFinite() && splitR.stateIsFinite() && splitS.stateIsFinite() &&
			    splitP.stateIsFinite() && combVocal.stateIsFinite() && apM.stateIsFinite() &&
			    apS.stateIsFinite() && pitch.stateIsFinite() && comb.stateIsFinite() &&
			    transients.stateIsFinite();
	if (!finite) {
		++recoveries;
		reset();
	}
}

void IsolatorEngine::processBlock(float *const *channels, int nch, int offset, int frames)
{
	const int nb = layout.numBands();
	double rb[kMaxBands], sb[kMaxBands], pb[kMaxBands], yv[kMaxBands];
	const double statA = 1.0 - statCoeff;
	const double gainA = 1.0 - gainCoeff;
	const double monoA = 1.0 - monoCoeff;
	const double powA = 1.0 - powerCoeff;
	const bool limiterOn = params.autoMakeup;

	for (int i = offset; i < offset + frames; ++i) {
		const double strength = sStrength.next();
		const double width = sWidth.next();
		const double outGain = sOutGain.next();
		const double pitchAmount = sPitchAmount.next();
		const double makeup = sMakeup.next();
		const double gate = pitchGate.next();
		const double mode = modeFade.next();
		const double fade = bypassFade.next();

		const double inL = channels[0][i];
		const double inR = nch >= 2 ? channels[1][i] : inL;
		bool finite = std::isfinite(inL) && std::isfinite(inR);
		for (int ch = 2; ch < nch; ++ch)
			finite = finite && std::isfinite(channels[ch][i]);
		if (!finite)
			continue; // leave a corrupt frame untouched rather than poison state

		const double M = 0.5 * (inL + inR);
		const double S = 0.5 * (inL - inR);

		monoPm += (M * M - monoPm) * monoA;
		monoPs += (S * S - monoPs) * monoA;
		powerIn += (0.5 * (inL * inL + inR * inR) - powerIn) * powA;
		transientFactor = transients.process(M);

		// [A] Predict the panned instruments' part of M from S.
		const double P = predictor.process(M, S, monoFactor < 0.5);
		const double R = M - P;

		// [B] Per-band vocal share of the residual.
		splitR.process(R, rb);
		splitS.process(S, sb);
		splitP.process(P, pb);

		double scale, overSubtract;
		strengthMapping(strength, scale, overSubtract);
		const double drive = scale * transientFactor;
		for (int b = 0; b < nb; ++b) {
			BandState &s = bands[b];
			s.pr += (rb[b] * rb[b] - s.pr) * statA;
			s.ps += (sb[b] * sb[b] - s.ps) * statA;
			s.pp += (pb[b] * pb[b] - s.pp) * statA;
			double t = s.target * overSubtract;
			t = t > 1.0 ? 1.0 : t;
			s.gain += (t * s.protect * drive - s.gain) * gainA;
			yv[b] = s.gain * rb[b];
		}
		const double vocal = combVocal.process(yv);
		const double mAll = apM.process(M);
		const double sAll = apS.process(S);
		const double mRes = mAll - vocal;

		// [C] Pitch-tracked comb: tracks the extracted vocal (or the whole
		// signal for mono input) and notches its harmonics out of what is left.
		pitch.process(vocal + monoFactor * mAll);
		const double notched = comb.process(mRes);
		const double combRemoved = pitchAmount * gate * (mRes - notched);
		const double mInst = mRes - combRemoved;
		const double vocalOut = vocal + combRemoved;

		const double sw = sAll * width;
		const double instL = mInst + sw;
		const double instR = mInst - sw;
		double outL = instL + mode * (vocalOut - instL);
		double outR = instR + mode * (vocalOut - instR);

		powerOut += (0.5 * (outL * outL + outR * outR) - powerOut) * powA;

		const double g = makeup * outGain;
		outL *= g;
		outR *= g;

		// Surround: the dedicated centre channel carries dialogue/vocals.
		double extra[kMaxChannels] = {};
		double peak = std::fmax(std::fabs(outL), std::fabs(outR));
		for (int ch = 2; ch < nch; ++ch) {
			const double x = channels[ch][i];
			double inst = x, voc = 0.0;
			if (ch == centreIndex) {
				inst = x * (1.0 - strength);
				voc = x;
			}
			extra[ch] = (inst + mode * (voc - inst)) * g;
			peak = std::fmax(peak, std::fabs(extra[ch]));
		}

		const double lg = limiterOn ? limiter.process(peak) : 1.0;
		const double ceiling = limiter.getCeiling();
		auto finish = [&](double v) {
			v *= lg;
			if (limiterOn)
				v = v > ceiling ? ceiling : (v < -ceiling ? -ceiling : v);
			return v;
		};

		if (nch == 1) {
			const double out = finish(0.5 * (outL + outR));
			channels[0][i] = static_cast<float>(inL + fade * (out - inL));
		} else {
			const double oL = finish(outL);
			const double oR = finish(outR);
			channels[0][i] = static_cast<float>(inL + fade * (oL - inL));
			channels[1][i] = static_cast<float>(inR + fade * (oR - inR));
			for (int ch = 2; ch < nch; ++ch) {
				const double dry = channels[ch][i];
				channels[ch][i] = static_cast<float>(dry + fade * (finish(extra[ch]) - dry));
			}
		}
	}
}

} // namespace isolator
