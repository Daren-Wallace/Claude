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

// The complete zero-latency vocal-removal chain. No libobs dependency.
//
//  L,R ─► M = (L+R)/2,  S = (L-R)/2
//          │
//          ├─► [A] SidePredictor (adaptive FIR):  P = w*S  (panned instruments' part of M)
//          │                                      R = M - P (vocal + decorrelated ambience)
//          │
//          ├─► [B] band split of R, S, P (2/3-octave, zero-latency Linkwitz-Riley)
//          │       per band: ambience bound   Pd = <S²> - <P²>  (side power panning cannot explain)
//          │                 vocal share     g  = (<R²> - α·Pd) / <R²>
//          │                 removal         r  = g · strength · protect · transient · (1 - mono)
//          │       vocal = Σ comp(r_b · R_b)
//          │
//          ├─► [C] pitch-tracked harmonic comb on the remaining mid (tracks the extracted vocal)
//          ▼
//   Instrumental:  M' = AP(M) - vocal - comb part;   L = M' + width·AP(S),  R = M' - width·AP(S)
//   Vocals only:   L = R = vocal + comb part
//          ─► auto makeup ─► output gain ─► safety limiter ─► bypass crossfade
//
// Real-time guarantees: zero samples of latency (causal FIR + IIR only), no
// allocation or locking in process(), every control smoothed, denormals
// flushed, non-finite state resets the engine and passes audio through.

#pragma once

#include "analysis.hpp"
#include "biquad.hpp"
#include "crossover.hpp"
#include "params.hpp"
#include "predictor.hpp"
#include "utils.hpp"

namespace isolator {

class IsolatorEngine {
public:
	static constexpr int kMaxChannels = 8;
	static constexpr int kSubBlock = 16;
	static constexpr double kFadeMs = 20.0;

	IsolatorEngine();

	// centreChannel: index of a dedicated centre speaker (4.0/4.1/5.1/7.1),
	// or -1. Resets all state; parameters are kept.
	void prepare(double sampleRate, int numChannels, int centreChannel = -1);

	void setParams(const IsolatorParams &params);
	void snapToTargets();
	void reset();

	// Planar float, in place. Channels 0/1 are the front left/right pair; a
	// single-channel buffer is treated as identical left and right.
	void process(float *const *channels, int numChannels, int frames);

	const IsolatorParams &getParams() const { return params; }
	double getSampleRate() const { return fs; }
	int getNumChannels() const { return channelCount; }
	int getNumBands() const { return layout.numBands(); }
	double getBandCentreHz(int band) const { return layout.bandCentre(band, fs); }
	int getPredictorTaps() const { return predictor.numTaps(); }

	// Diagnostics.
	double getBandGain(int band) const; // current removal 0..1
	double getTrackedF0() const { return pitch.f0(); }
	double getPitchConfidence() const { return pitch.confidence(); }
	double getVoicing() const { return pitchGate.value(); }
	double getMonoFactor() const { return monoFactor; }
	double getMakeupDb() const;
	unsigned long getTransientCount() const { return transients.onsetCount(); }
	unsigned long long getRecoveryCount() const { return recoveries; }

private:
	void configure();
	void updateBlockState();
	void processBlock(float *const *channels, int nch, int offset, int frames);

	double fs = 48000.0;
	int channelCount = 2;
	int centreIndex = -1;
	IsolatorParams params;

	dsp::SidePredictor predictor;
	dsp::BandLayout layout;
	dsp::BandSplitter splitR, splitS, splitP;
	dsp::BandCombiner combVocal;
	dsp::AllpassChain apM, apS;

	struct BandState {
		double pr = 0.0, ps = 0.0, pp = 0.0; // smoothed band powers of R, S, P
		double gain = 0.0;                   // current removal (smoothed)
		double target = 0.0;                 // estimated vocal share
		double protect = 1.0;                // protection weight 0..1
	};
	BandState bands[dsp::kMaxBands];
	double statCoeff = 0.0;
	double gainCoeff = 0.0;

	dsp::PitchTracker pitch;
	dsp::HarmonicComb comb;
	dsp::TransientDetector transients;
	dsp::PeakLimiter limiter;

	dsp::Smoother sStrength, sWidth, sOutGain, sPitchAmount, sMakeup;
	dsp::Smoother pitchGate;
	dsp::LinearRamp modeFade;   // 0 = instrumental, 1 = vocals
	dsp::LinearRamp bypassFade; // 1 = processing, 0 = bypassed

	// Mono / dual-mono detection: with no side signal, "remove the centre"
	// would remove everything, so centre removal fades out.
	double monoPm = 0.0, monoPs = 0.0, monoCoeff = 0.0, monoFactor = 0.0;

	// Auto makeup: long-term input vs output power.
	double powerIn = 0.0, powerOut = 0.0, powerCoeff = 0.0;

	double transientFactor = 1.0;
	int blockPos = 0; // position within the current 16-sample update block
	bool needsReset = false;
	bool dirty = true;
	unsigned long long recoveries = 0;
};

} // namespace isolator
