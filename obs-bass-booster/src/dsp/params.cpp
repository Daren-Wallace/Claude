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

#include "params.hpp"

#include <cmath>
#include <cstring>

namespace bassbooster {

BassParams sanitize(const BassParams &in)
{
	using namespace ranges;
	BassParams p = in;
	p.inputGainDb = kInputGainDb.clamp(in.inputGainDb);
	p.shelfFreqHz = kShelfFreqHz.clamp(in.shelfFreqHz);
	p.shelfGainDb = kShelfGainDb.clamp(in.shelfGainDb);
	p.shelfQ = kShelfQ.clamp(in.shelfQ);
	p.punchFreqHz = kPunchFreqHz.clamp(in.punchFreqHz);
	p.punchGainDb = kPunchGainDb.clamp(in.punchGainDb);
	p.punchQ = kPunchQ.clamp(in.punchQ);
	p.harmonicsAmount = kHarmonicsAmount.clamp(in.harmonicsAmount);
	p.harmonicsDrive = kHarmonicsDrive.clamp(in.harmonicsDrive);
	p.harmonicsLowHz = kHarmonicsLowHz.clamp(in.harmonicsLowHz);
	p.harmonicsHighHz = kHarmonicsHighHz.clamp(in.harmonicsHighHz);
	if (p.harmonicsHighHz < p.harmonicsLowHz * kMinHarmonicBandRatio) {
		p.harmonicsHighHz = p.harmonicsLowHz * kMinHarmonicBandRatio;
		if (p.harmonicsHighHz > kHarmonicsHighHz.max) {
			p.harmonicsHighHz = kHarmonicsHighHz.max;
			p.harmonicsLowHz = p.harmonicsHighHz / kMinHarmonicBandRatio;
		}
	}
	p.subAmount = kSubAmount.clamp(in.subAmount);
	p.subFreqHz = kSubFreqHz.clamp(in.subFreqHz);
	p.subTightness = kSubTightness.clamp(in.subTightness);
	p.rumbleFreqHz = kRumbleFreqHz.clamp(in.rumbleFreqHz);
	p.limiterCeilingDb = kLimiterCeilingDb.clamp(in.limiterCeilingDb);
	p.limiterReleaseMs = kLimiterReleaseMs.clamp(in.limiterReleaseMs);
	p.mix = kMix.clamp(in.mix);
	p.outputGainDb = kOutputGainDb.clamp(in.outputGainDb);
	return p;
}

namespace {

struct PresetBuilder {
	BassParams p;

	PresetBuilder &input(double db)
	{
		p.inputGainDb = db;
		return *this;
	}
	PresetBuilder &shelf(double hz, double db, double q)
	{
		p.shelfFreqHz = hz;
		p.shelfGainDb = db;
		p.shelfQ = q;
		return *this;
	}
	PresetBuilder &punch(bool on, double hz, double db, double q)
	{
		p.punchEnabled = on;
		p.punchFreqHz = hz;
		p.punchGainDb = db;
		p.punchQ = q;
		return *this;
	}
	PresetBuilder &harmonics(bool on, double amount, double drive, double lowHz, double highHz)
	{
		p.harmonicsEnabled = on;
		p.harmonicsAmount = amount;
		p.harmonicsDrive = drive;
		p.harmonicsLowHz = lowHz;
		p.harmonicsHighHz = highHz;
		return *this;
	}
	PresetBuilder &sub(bool on, double amount, double hz, double tightness)
	{
		p.subEnabled = on;
		p.subAmount = amount;
		p.subFreqHz = hz;
		p.subTightness = tightness;
		return *this;
	}
	PresetBuilder &rumble(double hz)
	{
		p.rumbleFreqHz = hz;
		return *this;
	}
	PresetBuilder &limiter(bool on, double ceilingDb, double releaseMs)
	{
		p.limiterEnabled = on;
		p.limiterCeilingDb = ceilingDb;
		p.limiterReleaseMs = releaseMs;
		return *this;
	}
	PresetBuilder &output(double mix, double db)
	{
		p.mix = mix;
		p.outputGainDb = db;
		return *this;
	}
};

// clang-format off
const Preset kPresets[] = {
	{"subtle", "Preset.Subtle", PresetBuilder{}
		.input(0).shelf(100, 4, 0.707).punch(false, 80, 0, 1.0)
		.harmonics(false, 0, 30, 120, 450).sub(false, 0, 100, 50)
		.rumble(25).limiter(true, -1, 150).output(100, 0).p},
	{"voice", "Preset.Voice", PresetBuilder{}
		.input(0).shelf(150, 6, 0.707).punch(true, 110, 3, 1.2)
		.harmonics(true, 25, 30, 120, 400).sub(false, 0, 100, 50)
		.rumble(40).limiter(true, -1, 150).output(100, 0).p},
	{"music", "Preset.Music", PresetBuilder{}
		.input(0).shelf(90, 8, 0.707).punch(true, 60, 4, 1.0)
		.harmonics(true, 20, 40, 80, 300).sub(true, 30, 80, 50)
		.rumble(25).limiter(true, -1, 150).output(100, 0).p},
	{"restore", "Preset.Restore", PresetBuilder{}
		.input(0).shelf(180, 10, 0.707).punch(true, 120, 4, 1.0)
		.harmonics(true, 60, 50, 250, 900).sub(true, 45, 120, 60)
		.rumble(30).limiter(true, -1, 150).output(100, -2).p},
	{"extreme", "Preset.Extreme", PresetBuilder{}
		.input(0).shelf(120, 16, 0.8).punch(true, 70, 6, 1.0)
		.harmonics(true, 75, 70, 150, 700).sub(true, 70, 90, 70)
		.rumble(25).limiter(true, -1, 200).output(100, -4).p},
};
// clang-format on

bool near(double a, double b, double tol)
{
	return std::fabs(a - b) <= tol;
}

} // namespace

const Preset *presets(std::size_t &count)
{
	count = sizeof(kPresets) / sizeof(kPresets[0]);
	return kPresets;
}

const Preset *findPreset(const char *id)
{
	if (!id)
		return nullptr;
	for (const Preset &p : kPresets) {
		if (std::strcmp(p.id, id) == 0)
			return &p;
	}
	return nullptr;
}

bool presetMatches(const BassParams &a, const BassParams &b)
{
	const double t = 1e-3;
	return near(a.inputGainDb, b.inputGainDb, t) && near(a.shelfFreqHz, b.shelfFreqHz, t) &&
	       near(a.shelfGainDb, b.shelfGainDb, t) && near(a.shelfQ, b.shelfQ, t) &&
	       a.punchEnabled == b.punchEnabled && near(a.punchFreqHz, b.punchFreqHz, t) &&
	       near(a.punchGainDb, b.punchGainDb, t) && near(a.punchQ, b.punchQ, t) &&
	       a.harmonicsEnabled == b.harmonicsEnabled && near(a.harmonicsAmount, b.harmonicsAmount, t) &&
	       near(a.harmonicsDrive, b.harmonicsDrive, t) && near(a.harmonicsLowHz, b.harmonicsLowHz, t) &&
	       near(a.harmonicsHighHz, b.harmonicsHighHz, t) && a.subEnabled == b.subEnabled &&
	       near(a.subAmount, b.subAmount, t) && near(a.subFreqHz, b.subFreqHz, t) &&
	       near(a.subTightness, b.subTightness, t) && near(a.rumbleFreqHz, b.rumbleFreqHz, t) &&
	       a.limiterEnabled == b.limiterEnabled && near(a.limiterCeilingDb, b.limiterCeilingDb, t) &&
	       near(a.limiterReleaseMs, b.limiterReleaseMs, t) && near(a.mix, b.mix, t) &&
	       near(a.outputGainDb, b.outputGainDb, t);
}

} // namespace bassbooster
