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

// User-facing parameters, their ranges and the factory presets. The OBS UI,
// defaults, render tool and tests all read this single table.

#pragma once

#include <cstddef>

namespace isolator {

struct ParamRange {
	double min;
	double max;
	double def;
	double step;

	double clamp(double v) const
	{
		if (!(v == v))
			return def;
		return v < min ? min : (v > max ? max : v);
	}
};

namespace ranges {
// clang-format off
constexpr ParamRange kStrength          {  0.0,   100.0,   85.0, 1.0};
constexpr ParamRange kCentreWidth       {  0.0,   100.0,   50.0, 1.0};
constexpr ParamRange kResponseMs        {  5.0,   100.0,   20.0, 1.0};
constexpr ParamRange kProtectLowHz      { 40.0,   300.0,  120.0, 1.0};
constexpr ParamRange kProtectHighHz     {4000.0, 16000.0, 8000.0, 100.0};
constexpr ParamRange kTransientProtect  {  0.0,   100.0,   50.0, 1.0};
constexpr ParamRange kPitchAmount       {  0.0,   100.0,   50.0, 1.0};
constexpr ParamRange kPitchHarmonics    {  4.0,    40.0,   20.0, 1.0};
constexpr ParamRange kVoiceLowHz        { 60.0,   400.0,   80.0, 1.0};
constexpr ParamRange kVoiceHighHz       {300.0,  1200.0, 1000.0, 1.0};
constexpr ParamRange kStereoWidth       {  0.0,   200.0,  100.0, 1.0};
constexpr ParamRange kOutputGainDb      {-24.0,    12.0,    0.0, 0.1};
// clang-format on

// The voice range must span at least this ratio (high / low).
constexpr double kMinVoiceRangeRatio = 2.0;
} // namespace ranges

enum class OutputMode : int {
	Instrumental = 0, // vocals removed (default)
	Vocals = 1,       // only the extracted vocal
};

struct IsolatorParams {
	bool bypass = false;
	OutputMode outputMode = OutputMode::Instrumental;

	// [A] Centre-steered sub-band removal.
	double strength = ranges::kStrength.def;
	double centreWidth = ranges::kCentreWidth.def;
	double responseMs = ranges::kResponseMs.def;

	// [C] + [D] Protection.
	double protectLowHz = ranges::kProtectLowHz.def;
	double protectHighHz = ranges::kProtectHighHz.def;
	double transientProtect = ranges::kTransientProtect.def;

	// [B] Pitch-tracked harmonic remover.
	bool pitchEnabled = true;
	double pitchAmount = ranges::kPitchAmount.def;
	double pitchHarmonics = ranges::kPitchHarmonics.def;
	double voiceLowHz = ranges::kVoiceLowHz.def;
	double voiceHighHz = ranges::kVoiceHighHz.def;

	// Output.
	double stereoWidth = ranges::kStereoWidth.def;
	bool autoMakeup = true;
	double outputGainDb = ranges::kOutputGainDb.def;
};

IsolatorParams sanitize(const IsolatorParams &in);

struct Preset {
	const char *id;
	const char *localeKey;
	IsolatorParams params;
};

constexpr const char *kCustomPresetId = "custom";
constexpr const char *kDefaultPresetId = "karaoke";

const Preset *presets(std::size_t &count);
const Preset *findPreset(const char *id);

// True when every preset-controlled field matches (bypass is not part of a
// preset).
bool presetMatches(const IsolatorParams &a, const IsolatorParams &b);

} // namespace isolator
