/*
Instrumental Isolator for OBS - parameter, preset and settings-key tests
Copyright (C) 2026 Daren Wallace
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <catch2/catch_test_macros.hpp>

#include "dsp/param_fields.hpp"
#include "dsp/params.hpp"

#include <cmath>
#include <cstring>
#include <set>
#include <string>

using namespace isolator;

TEST_CASE("sanitize clamps every field and replaces NaN", "[params]")
{
	IsolatorParams p;
	p.strength = 500;
	p.centreWidth = -5;
	p.responseMs = 0;
	p.protectLowHz = 1e6;
	p.protectHighHz = 1;
	p.transientProtect = std::nan("");
	p.pitchAmount = 101;
	p.pitchHarmonics = 3.4;
	p.stereoWidth = 999;
	p.outputGainDb = -100;
	p.outputMode = static_cast<OutputMode>(7);
	const IsolatorParams s = sanitize(p);
	CHECK(s.strength == ranges::kStrength.max);
	CHECK(s.centreWidth == ranges::kCentreWidth.min);
	CHECK(s.responseMs == ranges::kResponseMs.min);
	CHECK(s.protectLowHz == ranges::kProtectLowHz.max);
	CHECK(s.protectHighHz == ranges::kProtectHighHz.min);
	CHECK(s.transientProtect == ranges::kTransientProtect.def);
	CHECK(s.pitchAmount == ranges::kPitchAmount.max);
	CHECK(s.pitchHarmonics == ranges::kPitchHarmonics.min);
	CHECK(s.stereoWidth == ranges::kStereoWidth.max);
	CHECK(s.outputGainDb == ranges::kOutputGainDb.min);
	CHECK(s.outputMode == OutputMode::Instrumental);
}

TEST_CASE("Harmonics count is rounded to a whole number", "[params]")
{
	IsolatorParams p;
	p.pitchHarmonics = 12.6;
	CHECK(sanitize(p).pitchHarmonics == 13.0);
}

TEST_CASE("Voice range always spans at least an octave", "[params]")
{
	IsolatorParams p;
	p.voiceLowHz = 300;
	p.voiceHighHz = 350;
	IsolatorParams s = sanitize(p);
	CHECK(s.voiceHighHz >= s.voiceLowHz * ranges::kMinVoiceRangeRatio - 1e-9);
	p.voiceLowHz = 400;
	p.voiceHighHz = 300;
	s = sanitize(p);
	CHECK(s.voiceHighHz <= ranges::kVoiceHighHz.max);
	CHECK(s.voiceHighHz >= s.voiceLowHz * ranges::kMinVoiceRangeRatio - 1e-9);
}

TEST_CASE("Range defaults sit inside their ranges", "[params]")
{
	for (const FloatField &f : kFloatFields) {
		INFO(f.key);
		REQUIRE(f.range->min < f.range->max);
		REQUIRE(f.range->def >= f.range->min);
		REQUIRE(f.range->def <= f.range->max);
		REQUIRE(f.range->step > 0);
	}
}

TEST_CASE("Factory presets are valid, unique and already sanitised", "[params][presets]")
{
	size_t count = 0;
	const Preset *list = presets(count);
	REQUIRE(count == 5);
	std::set<std::string> ids;
	for (size_t i = 0; i < count; ++i) {
		INFO(list[i].id);
		CHECK(ids.insert(list[i].id).second);
		CHECK(std::strcmp(list[i].id, kCustomPresetId) != 0);
		CHECK(std::strncmp(list[i].localeKey, "Preset.", 7) == 0);
		CHECK(presetMatches(sanitize(list[i].params), list[i].params));
		CHECK(findPreset(list[i].id) == &list[i]);
		CHECK_FALSE(list[i].params.bypass);
	}
	CHECK(findPreset(kDefaultPresetId) != nullptr);
	CHECK(findPreset(kCustomPresetId) == nullptr);
	CHECK(findPreset(nullptr) == nullptr);
	CHECK(findPreset("vocals")->params.outputMode == OutputMode::Vocals);
	CHECK(findPreset("karaoke")->params.outputMode == OutputMode::Instrumental);
	CHECK(findPreset("gentle")->params.strength < findPreset("aggressive")->params.strength);
}

TEST_CASE("presetMatches ignores bypass and detects every other change", "[params][presets]")
{
	const IsolatorParams base = findPreset("karaoke")->params;
	IsolatorParams p = base;
	p.bypass = true;
	CHECK(presetMatches(p, base));
	p = base;
	p.outputMode = OutputMode::Vocals;
	CHECK_FALSE(presetMatches(p, base));
	p = base;
	p.autoMakeup = !p.autoMakeup;
	CHECK_FALSE(presetMatches(p, base));
	for (const FloatField &f : kFloatFields) {
		p = base;
		p.*f.member += 1.0;
		INFO(f.key);
		CHECK_FALSE(presetMatches(p, base));
	}
}

TEST_CASE("Settings keys are unique and map to distinct fields", "[params][fields]")
{
	std::set<std::string> keys = {kOutputModeKey};
	const size_t nf = sizeof(kFloatFields) / sizeof(kFloatFields[0]);
	const size_t nb = sizeof(kBoolFields) / sizeof(kBoolFields[0]);
	for (size_t i = 0; i < nf; ++i) {
		CHECK(keys.insert(kFloatFields[i].key).second);
		CHECK(findFloatField(kFloatFields[i].key) == &kFloatFields[i]);
		for (size_t j = i + 1; j < nf; ++j)
			CHECK(kFloatFields[i].member != kFloatFields[j].member);
	}
	for (size_t i = 0; i < nb; ++i) {
		CHECK(keys.insert(kBoolFields[i].key).second);
		CHECK(findBoolField(kBoolFields[i].key) == &kBoolFields[i]);
		CHECK(kBoolFields[i].inPreset == (std::strcmp(kBoolFields[i].key, "bypass") != 0));
	}
	CHECK(nf == 12);
	CHECK(nb == 3);
	CHECK(findFloatField("nope") == nullptr);
	CHECK(findBoolField(nullptr) == nullptr);
}
