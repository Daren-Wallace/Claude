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

#include "params.hpp"

#include <cmath>
#include <cstring>

namespace isolator {

IsolatorParams sanitize(const IsolatorParams &in)
{
	using namespace ranges;
	IsolatorParams p = in;
	if (p.outputMode != OutputMode::Instrumental && p.outputMode != OutputMode::Vocals)
		p.outputMode = OutputMode::Instrumental;
	p.strength = kStrength.clamp(in.strength);
	p.centreWidth = kCentreWidth.clamp(in.centreWidth);
	p.responseMs = kResponseMs.clamp(in.responseMs);
	p.protectLowHz = kProtectLowHz.clamp(in.protectLowHz);
	p.protectHighHz = kProtectHighHz.clamp(in.protectHighHz);
	p.transientProtect = kTransientProtect.clamp(in.transientProtect);
	p.pitchAmount = kPitchAmount.clamp(in.pitchAmount);
	p.pitchHarmonics = std::round(kPitchHarmonics.clamp(in.pitchHarmonics));
	p.voiceLowHz = kVoiceLowHz.clamp(in.voiceLowHz);
	p.voiceHighHz = kVoiceHighHz.clamp(in.voiceHighHz);
	if (p.voiceHighHz < p.voiceLowHz * kMinVoiceRangeRatio) {
		p.voiceHighHz = p.voiceLowHz * kMinVoiceRangeRatio;
		if (p.voiceHighHz > kVoiceHighHz.max) {
			p.voiceHighHz = kVoiceHighHz.max;
			p.voiceLowHz = p.voiceHighHz / kMinVoiceRangeRatio;
		}
	}
	p.stereoWidth = kStereoWidth.clamp(in.stereoWidth);
	p.outputGainDb = kOutputGainDb.clamp(in.outputGainDb);
	return p;
}

namespace {

struct B {
	IsolatorParams p;
	B &mode(OutputMode m)
	{
		p.outputMode = m;
		return *this;
	}
	B &removal(double strength, double width, double responseMs)
	{
		p.strength = strength;
		p.centreWidth = width;
		p.responseMs = responseMs;
		return *this;
	}
	B &protect(double lowHz, double highHz, double transient)
	{
		p.protectLowHz = lowHz;
		p.protectHighHz = highHz;
		p.transientProtect = transient;
		return *this;
	}
	B &pitch(bool on, double amount, double harmonics, double lowHz, double highHz)
	{
		p.pitchEnabled = on;
		p.pitchAmount = amount;
		p.pitchHarmonics = harmonics;
		p.voiceLowHz = lowHz;
		p.voiceHighHz = highHz;
		return *this;
	}
	B &output(double width, bool makeup, double gainDb)
	{
		p.stereoWidth = width;
		p.autoMakeup = makeup;
		p.outputGainDb = gainDb;
		return *this;
	}
};

// clang-format off
const Preset kPresets[] = {
	{"karaoke", "Preset.Karaoke", B{}.mode(OutputMode::Instrumental)
		.removal(90, 50, 20).protect(120, 8000, 50)
		.pitch(true, 50, 20, 80, 1000).output(100, true, 0).p},
	{"gentle", "Preset.Gentle", B{}.mode(OutputMode::Instrumental)
		.removal(60, 35, 30).protect(150, 7000, 60)
		.pitch(false, 50, 20, 80, 1000).output(100, true, 0).p},
	{"aggressive", "Preset.Aggressive", B{}.mode(OutputMode::Instrumental)
		.removal(100, 75, 12).protect(100, 10000, 30)
		.pitch(true, 85, 30, 80, 1000).output(110, true, 0).p},
	{"musicbed", "Preset.MusicBed", B{}.mode(OutputMode::Instrumental)
		.removal(80, 50, 25).protect(150, 6000, 70)
		.pitch(false, 50, 20, 80, 1000).output(100, true, -3).p},
	{"vocals", "Preset.Vocals", B{}.mode(OutputMode::Vocals)
		.removal(100, 50, 20).protect(120, 8000, 50)
		.pitch(true, 50, 20, 80, 1000).output(100, true, 0).p},
};
// clang-format on

bool near(double a, double b)
{
	return std::fabs(a - b) <= 1e-3;
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
	for (const Preset &p : kPresets)
		if (std::strcmp(p.id, id) == 0)
			return &p;
	return nullptr;
}

bool presetMatches(const IsolatorParams &a, const IsolatorParams &b)
{
	return a.outputMode == b.outputMode && near(a.strength, b.strength) && near(a.centreWidth, b.centreWidth) &&
	       near(a.responseMs, b.responseMs) && near(a.protectLowHz, b.protectLowHz) &&
	       near(a.protectHighHz, b.protectHighHz) && near(a.transientProtect, b.transientProtect) &&
	       a.pitchEnabled == b.pitchEnabled && near(a.pitchAmount, b.pitchAmount) &&
	       near(a.pitchHarmonics, b.pitchHarmonics) && near(a.voiceLowHz, b.voiceLowHz) &&
	       near(a.voiceHighHz, b.voiceHighHz) && near(a.stereoWidth, b.stereoWidth) &&
	       a.autoMakeup == b.autoMakeup && near(a.outputGainDb, b.outputGainDb);
}

} // namespace isolator
