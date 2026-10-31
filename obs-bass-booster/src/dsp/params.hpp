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

// User-facing parameters, their legal ranges and the factory presets.
// The OBS properties UI, the settings defaults, the offline render tool and
// the tests all read from this one table, so nothing can drift apart.

#pragma once

#include <cstddef>

namespace bassbooster {

struct ParamRange {
	double min;
	double max;
	double def;
	double step;

	double clamp(double v) const
	{
		if (!(v == v)) // NaN
			return def;
		return v < min ? min : (v > max ? max : v);
	}
};

namespace ranges {
// clang-format off
constexpr ParamRange kInputGainDb      {-24.0,  24.0,    0.0, 0.1};
constexpr ParamRange kShelfFreqHz      { 20.0, 300.0,  120.0, 1.0};
constexpr ParamRange kShelfGainDb      {-12.0,  24.0,    8.0, 0.1};
constexpr ParamRange kShelfQ           {  0.3,   1.5,  0.707, 0.01};
constexpr ParamRange kPunchFreqHz      { 30.0, 250.0,   80.0, 1.0};
constexpr ParamRange kPunchGainDb      {-12.0,  12.0,    3.0, 0.1};
constexpr ParamRange kPunchQ           {  0.3,   4.0,    1.0, 0.01};
constexpr ParamRange kHarmonicsAmount  {  0.0, 100.0,   35.0, 1.0};
constexpr ParamRange kHarmonicsDrive   {  0.0, 100.0,   40.0, 1.0};
constexpr ParamRange kHarmonicsLowHz   { 40.0, 500.0,  120.0, 1.0};
constexpr ParamRange kHarmonicsHighHz  {150.0, 1500.0, 450.0, 1.0};
constexpr ParamRange kSubAmount        {  0.0, 100.0,   30.0, 1.0};
constexpr ParamRange kSubFreqHz        { 40.0, 200.0,  100.0, 1.0};
constexpr ParamRange kSubTightness     {  0.0, 100.0,   50.0, 1.0};
constexpr ParamRange kRumbleFreqHz     { 10.0,  60.0,   25.0, 1.0};
constexpr ParamRange kLimiterCeilingDb {-12.0,   0.0,   -1.0, 0.1};
constexpr ParamRange kLimiterReleaseMs { 10.0, 1000.0, 150.0, 1.0};
constexpr ParamRange kMix              {  0.0, 100.0,  100.0, 1.0};
constexpr ParamRange kOutputGainDb     {-24.0,  12.0,    0.0, 0.1};
// clang-format on

// The harmonic band must span at least this ratio (high / low) so its two
// filters never collapse onto each other.
constexpr double kMinHarmonicBandRatio = 1.5;
} // namespace ranges

struct BassParams {
	bool bypass = false;

	double inputGainDb = ranges::kInputGainDb.def;

	// [A] Low-shelf boost.
	double shelfFreqHz = ranges::kShelfFreqHz.def;
	double shelfGainDb = ranges::kShelfGainDb.def;
	double shelfQ = ranges::kShelfQ.def;

	// [B] Punch: peaking bell for low-end thump.
	bool punchEnabled = true;
	double punchFreqHz = ranges::kPunchFreqHz.def;
	double punchGainDb = ranges::kPunchGainDb.def;
	double punchQ = ranges::kPunchQ.def;

	// [C] Harmonic restoration (missing-fundamental regeneration).
	bool harmonicsEnabled = false;
	double harmonicsAmount = ranges::kHarmonicsAmount.def;
	double harmonicsDrive = ranges::kHarmonicsDrive.def;
	double harmonicsLowHz = ranges::kHarmonicsLowHz.def;
	double harmonicsHighHz = ranges::kHarmonicsHighHz.def;

	// [D] Sub-octave generator.
	bool subEnabled = false;
	double subAmount = ranges::kSubAmount.def;
	double subFreqHz = ranges::kSubFreqHz.def;
	double subTightness = ranges::kSubTightness.def;

	// [E] Rumble (infrasonic) high-pass, always active.
	double rumbleFreqHz = ranges::kRumbleFreqHz.def;

	// [F] Output peak limiter.
	bool limiterEnabled = true;
	double limiterCeilingDb = ranges::kLimiterCeilingDb.def;
	double limiterReleaseMs = ranges::kLimiterReleaseMs.def;

	// Output stage.
	double mix = ranges::kMix.def;
	double outputGainDb = ranges::kOutputGainDb.def;
};

// Clamp every field into range (NaN becomes the default) and enforce the
// harmonic band ordering. Always safe to feed arbitrary user input through.
BassParams sanitize(const BassParams &in);

// ---------------------------------------------------------------- presets --

struct Preset {
	const char *id;        // stable key stored in OBS settings
	const char *localeKey; // key into data/locale/*.ini
	BassParams params;
};

constexpr const char *kCustomPresetId = "custom";
constexpr const char *kDefaultPresetId = "restore";

// Factory presets in UI order (excluding "custom").
const Preset *presets(std::size_t &count);
const Preset *findPreset(const char *id);

// True when every preset-controlled field of `a` equals `b` (within slider
// resolution). The bypass flag is not part of a preset.
bool presetMatches(const BassParams &a, const BassParams &b);

} // namespace bassbooster
