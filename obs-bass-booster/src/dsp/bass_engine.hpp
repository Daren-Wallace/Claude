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

// The complete Bass Booster processing chain. It has no dependency on
// libobs, so it is unit-tested directly and reused by the offline render tool.
//
//            ┌──────────── dry (untouched input) ──────────────────────────┐
//            │                                                             ▼
//  in ─► input trim ─┬─► [A] low-shelf ─► [B] punch ─►(+)─► [E] rumble HPF ─► mix ─► output gain ─► [F] limiter ─► bypass xfade ─► out
//                    │                                 ▲
//                    └─► mono sum ─┬─► [C] harmonic restorer ─┤
//                                  └─► [D] sub-octave gen. ───┘
//
// The shelf, punch and rumble filters run per channel. The two generators run
// once on the mono sum and their output is added to every channel, because
// bass is non-directional and this keeps the synthesised low end phase
// coherent across speakers. The limiter is channel-linked so the stereo
// image never shifts.
//
// Real-time guarantees: no allocation, locking or I/O inside process(); zero
// added latency; every parameter is smoothed; denormals are flushed; any
// non-finite value resets the engine and passes the input through unharmed.

#pragma once

#include "biquad.hpp"
#include "generators.hpp"
#include "params.hpp"
#include "utils.hpp"

namespace bassbooster {

class BassEngine {
public:
	static constexpr int kMaxChannels = 8;
	// Filter parameters are smoothed and their coefficients recomputed once
	// per sub-block of this many samples.
	static constexpr int kSubBlock = 32;
	static constexpr double kSmoothingMs = 15.0;
	static constexpr double kBypassFadeMs = 20.0;

	BassEngine();

	// Must be called before process() and whenever the sample rate or
	// channel count changes. Resets all state; parameters are kept.
	void prepare(double sampleRate, int numChannels);

	// Set new target parameters. Values are sanitised; the audio glides to
	// them over ~kSmoothingMs. Call from the audio thread (see plugin code
	// for the lock-free hand-off from the UI thread).
	void setParams(const BassParams &params);

	// Jump straight to the current targets without smoothing (used right
	// after creation so the first buffer is already correct).
	void snapToTargets();

	// Process planar float audio in place. `frames` may be any size,
	// including 0. Channels beyond kMaxChannels are left untouched.
	void process(float *const *channels, int numChannels, int frames);

	void reset();

	double getSampleRate() const { return fs; }
	int getNumChannels() const { return channelCount; }
	const BassParams &getParams() const { return params; }

	// Number of times a non-finite value forced a reset (diagnostics).
	unsigned long long getRecoveryCount() const { return recoveries; }

	// Most recent limiter gain in dB (0 = no gain reduction).
	double getLimiterGainReductionDb() const;

	// Analytic magnitude response of the linear part of the chain (input
	// trim, shelf, punch, rumble, output gain, mix with dry) at the current
	// *target* parameters, in dB. Generators and limiter are non-linear and
	// therefore excluded. Used for documentation plots and tests.
	double linearResponseDb(double freqHz) const;

private:
	void updateFilterCoefficients(bool force);
	void configureStaticSections();
	void processBlock(float *const *channels, int numChannels, int offset, int frames);

	double fs = 48000.0;
	int channelCount = 2;
	BassParams params;

	// Per-channel filters.
	dsp::Biquad shelf[kMaxChannels];
	dsp::Biquad punch[kMaxChannels];
	dsp::Butterworth4 rumble[kMaxChannels];

	// Shared generators and limiter.
	dsp::HarmonicRestorer harmonics;
	dsp::SubOctaveGenerator sub;
	dsp::PeakLimiter limiter;

	// Block-rate smoothers for filter parameters.
	dsp::Smoother sShelfFreq, sShelfGain, sShelfQ;
	dsp::Smoother sPunchFreq, sPunchGain, sPunchQ;

	// Sample-rate smoothers for gains.
	dsp::Smoother sInputGain, sOutputGain, sMix, sHarmonics, sSub;
	dsp::LinearRamp bypassFade; // 1 = processing, 0 = bypassed

	bool needsReset = false;
	bool staticDirty = true;
	unsigned long long recoveries = 0;

	// Scratch for one frame across channels.
	double frameWet[kMaxChannels] = {};
};

} // namespace bassbooster
