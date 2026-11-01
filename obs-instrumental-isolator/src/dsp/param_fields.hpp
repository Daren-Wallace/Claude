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

// Settings key <-> struct field table, shared by the OBS plugin (scene
// collection keys) and the render tool (--set key=value). Never rename a key.

#pragma once

#include "params.hpp"

#include <cstring>

namespace isolator {

struct FloatField {
	const char *key;
	const char *text; // locale key; tooltip is text + ".Tooltip"
	const ParamRange *range;
	double IsolatorParams::*member;
	const char *suffix;
};

struct BoolField {
	const char *key;
	bool IsolatorParams::*member;
	bool inPreset;
};

constexpr const char *kOutputModeKey = "output_mode";

// clang-format off
inline constexpr FloatField kFloatFields[] = {
	{"strength",          "Strength",         &ranges::kStrength,         &IsolatorParams::strength,         " %"},
	{"centre_width",      "CentreWidth",      &ranges::kCentreWidth,      &IsolatorParams::centreWidth,      " %"},
	{"response_ms",       "Response",         &ranges::kResponseMs,       &IsolatorParams::responseMs,       " ms"},
	{"protect_low_hz",    "ProtectLow",       &ranges::kProtectLowHz,     &IsolatorParams::protectLowHz,     " Hz"},
	{"protect_high_hz",   "ProtectHigh",      &ranges::kProtectHighHz,    &IsolatorParams::protectHighHz,    " Hz"},
	{"transient_protect", "TransientProtect", &ranges::kTransientProtect, &IsolatorParams::transientProtect, " %"},
	{"pitch_amount",      "PitchAmount",      &ranges::kPitchAmount,      &IsolatorParams::pitchAmount,      " %"},
	{"pitch_harmonics",   "PitchHarmonics",   &ranges::kPitchHarmonics,   &IsolatorParams::pitchHarmonics,   ""},
	{"voice_low_hz",      "VoiceLow",         &ranges::kVoiceLowHz,       &IsolatorParams::voiceLowHz,       " Hz"},
	{"voice_high_hz",     "VoiceHigh",        &ranges::kVoiceHighHz,      &IsolatorParams::voiceHighHz,      " Hz"},
	{"stereo_width",      "StereoWidth",      &ranges::kStereoWidth,      &IsolatorParams::stereoWidth,      " %"},
	{"output_gain_db",    "OutputGain",       &ranges::kOutputGainDb,     &IsolatorParams::outputGainDb,     " dB"},
};

inline constexpr BoolField kBoolFields[] = {
	{"bypass",        &IsolatorParams::bypass,       false},
	{"pitch_enabled", &IsolatorParams::pitchEnabled, true},
	{"auto_makeup",   &IsolatorParams::autoMakeup,   true},
};
// clang-format on

inline const FloatField *findFloatField(const char *key)
{
	for (const FloatField &f : kFloatFields)
		if (key && std::strcmp(f.key, key) == 0)
			return &f;
	return nullptr;
}

inline const BoolField *findBoolField(const char *key)
{
	for (const BoolField &f : kBoolFields)
		if (key && std::strcmp(f.key, key) == 0)
			return &f;
	return nullptr;
}

} // namespace isolator
