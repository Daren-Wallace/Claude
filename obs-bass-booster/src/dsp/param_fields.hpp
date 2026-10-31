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

// Maps every BassParams field to its persistent settings key. The OBS plugin
// uses these keys in scene collections; the render tool accepts the same keys
// on its command line (--set shelf_gain_db=10), so one vocabulary covers both.
// Keys are stored in users' scene collections: never rename one.

#pragma once

#include "params.hpp"

#include <cstddef>
#include <cstring>

namespace bassbooster {

struct FloatField {
	const char *key;
	const char *text; // locale key; the tooltip is text + ".Tooltip"
	const ParamRange *range;
	double BassParams::*member;
	const char *suffix;
};

struct BoolField {
	const char *key;
	bool BassParams::*member;
	bool inPreset; // false for "bypass", which presets never touch
};

// clang-format off
inline constexpr FloatField kFloatFields[] = {
	{"input_gain_db",      "InputGain",       &ranges::kInputGainDb,      &BassParams::inputGainDb,      " dB"},
	{"shelf_gain_db",      "ShelfGain",       &ranges::kShelfGainDb,      &BassParams::shelfGainDb,      " dB"},
	{"shelf_freq_hz",      "ShelfFreq",       &ranges::kShelfFreqHz,      &BassParams::shelfFreqHz,      " Hz"},
	{"shelf_q",            "ShelfQ",          &ranges::kShelfQ,           &BassParams::shelfQ,           ""},
	{"punch_gain_db",      "PunchGain",       &ranges::kPunchGainDb,      &BassParams::punchGainDb,      " dB"},
	{"punch_freq_hz",      "PunchFreq",       &ranges::kPunchFreqHz,      &BassParams::punchFreqHz,      " Hz"},
	{"punch_q",            "PunchQ",          &ranges::kPunchQ,           &BassParams::punchQ,           ""},
	{"harmonics_amount",   "HarmonicsAmount", &ranges::kHarmonicsAmount,  &BassParams::harmonicsAmount,  " %"},
	{"harmonics_drive",    "HarmonicsDrive",  &ranges::kHarmonicsDrive,   &BassParams::harmonicsDrive,   " %"},
	{"harmonics_low_hz",   "HarmonicsLow",    &ranges::kHarmonicsLowHz,   &BassParams::harmonicsLowHz,   " Hz"},
	{"harmonics_high_hz",  "HarmonicsHigh",   &ranges::kHarmonicsHighHz,  &BassParams::harmonicsHighHz,  " Hz"},
	{"sub_amount",         "SubAmount",       &ranges::kSubAmount,        &BassParams::subAmount,        " %"},
	{"sub_freq_hz",        "SubFreq",         &ranges::kSubFreqHz,        &BassParams::subFreqHz,        " Hz"},
	{"sub_tightness",      "SubTightness",    &ranges::kSubTightness,     &BassParams::subTightness,     " %"},
	{"rumble_freq_hz",     "RumbleFreq",      &ranges::kRumbleFreqHz,     &BassParams::rumbleFreqHz,     " Hz"},
	{"limiter_ceiling_db", "LimiterCeiling",  &ranges::kLimiterCeilingDb, &BassParams::limiterCeilingDb, " dB"},
	{"limiter_release_ms", "LimiterRelease",  &ranges::kLimiterReleaseMs, &BassParams::limiterReleaseMs, " ms"},
	{"mix",                "Mix",             &ranges::kMix,              &BassParams::mix,              " %"},
	{"output_gain_db",     "OutputGain",      &ranges::kOutputGainDb,     &BassParams::outputGainDb,     " dB"},
};

inline constexpr BoolField kBoolFields[] = {
	{"bypass",            &BassParams::bypass,           false},
	{"punch_enabled",     &BassParams::punchEnabled,     true},
	{"harmonics_enabled", &BassParams::harmonicsEnabled, true},
	{"sub_enabled",       &BassParams::subEnabled,       true},
	{"limiter_enabled",   &BassParams::limiterEnabled,   true},
};
// clang-format on

inline const FloatField *findFloatField(const char *key)
{
	for (const FloatField &f : kFloatFields) {
		if (key && std::strcmp(f.key, key) == 0)
			return &f;
	}
	return nullptr;
}

inline const BoolField *findBoolField(const char *key)
{
	for (const BoolField &f : kBoolFields) {
		if (key && std::strcmp(f.key, key) == 0)
			return &f;
	}
	return nullptr;
}

} // namespace bassbooster
