/*
Bass Booster for OBS - parameter and preset tests
Copyright (C) 2026 Daren Wallace
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <catch2/catch_test_macros.hpp>

#include "dsp/params.hpp"

#include <cmath>
#include <cstring>
#include <set>
#include <string>

using namespace bassbooster;

TEST_CASE("sanitize clamps every field into range", "[params]")
{
	BassParams p;
	p.inputGainDb = 1000;
	p.shelfFreqHz = -5;
	p.shelfGainDb = 99;
	p.shelfQ = 0;
	p.punchFreqHz = 1e6;
	p.punchGainDb = -99;
	p.punchQ = 100;
	p.harmonicsAmount = 500;
	p.harmonicsDrive = -1;
	p.subAmount = 101;
	p.subFreqHz = 1;
	p.subTightness = 1000;
	p.rumbleFreqHz = 0;
	p.limiterCeilingDb = 6;
	p.limiterReleaseMs = 0;
	p.mix = -50;
	p.outputGainDb = 100;

	const BassParams s = sanitize(p);
	CHECK(s.inputGainDb == ranges::kInputGainDb.max);
	CHECK(s.shelfFreqHz == ranges::kShelfFreqHz.min);
	CHECK(s.shelfGainDb == ranges::kShelfGainDb.max);
	CHECK(s.shelfQ == ranges::kShelfQ.min);
	CHECK(s.punchFreqHz == ranges::kPunchFreqHz.max);
	CHECK(s.punchGainDb == ranges::kPunchGainDb.min);
	CHECK(s.punchQ == ranges::kPunchQ.max);
	CHECK(s.harmonicsAmount == ranges::kHarmonicsAmount.max);
	CHECK(s.harmonicsDrive == ranges::kHarmonicsDrive.min);
	CHECK(s.subAmount == ranges::kSubAmount.max);
	CHECK(s.subFreqHz == ranges::kSubFreqHz.min);
	CHECK(s.subTightness == ranges::kSubTightness.max);
	CHECK(s.rumbleFreqHz == ranges::kRumbleFreqHz.min);
	CHECK(s.limiterCeilingDb == ranges::kLimiterCeilingDb.max);
	CHECK(s.limiterReleaseMs == ranges::kLimiterReleaseMs.min);
	CHECK(s.mix == ranges::kMix.min);
	CHECK(s.outputGainDb == ranges::kOutputGainDb.max);
}

TEST_CASE("sanitize replaces NaN with defaults", "[params]")
{
	BassParams p;
	p.shelfGainDb = std::nan("");
	p.mix = std::nan("");
	const BassParams s = sanitize(p);
	CHECK(s.shelfGainDb == ranges::kShelfGainDb.def);
	CHECK(s.mix == ranges::kMix.def);
}

TEST_CASE("sanitize keeps the harmonic band at least 1.5x wide", "[params]")
{
	BassParams p;
	p.harmonicsLowHz = 400;
	p.harmonicsHighHz = 200;
	BassParams s = sanitize(p);
	CHECK(s.harmonicsHighHz >= s.harmonicsLowHz * ranges::kMinHarmonicBandRatio - 1e-9);

	p.harmonicsLowHz = 500;
	p.harmonicsHighHz = 1500;
	s = sanitize(p);
	CHECK(s.harmonicsLowHz == 500);
	CHECK(s.harmonicsHighHz == 1500);

	// Upper edge pinned at its maximum: the low edge moves down instead.
	p.harmonicsLowHz = 500;
	p.harmonicsHighHz = 150;
	s = sanitize(p);
	CHECK(s.harmonicsHighHz <= ranges::kHarmonicsHighHz.max);
	CHECK(s.harmonicsHighHz >= s.harmonicsLowHz * ranges::kMinHarmonicBandRatio - 1e-9);
}

TEST_CASE("Range defaults are inside their ranges", "[params]")
{
	for (const ParamRange *r :
	     {&ranges::kInputGainDb, &ranges::kShelfFreqHz, &ranges::kShelfGainDb, &ranges::kShelfQ,
	      &ranges::kPunchFreqHz, &ranges::kPunchGainDb, &ranges::kPunchQ, &ranges::kHarmonicsAmount,
	      &ranges::kHarmonicsDrive, &ranges::kHarmonicsLowHz, &ranges::kHarmonicsHighHz, &ranges::kSubAmount,
	      &ranges::kSubFreqHz, &ranges::kSubTightness, &ranges::kRumbleFreqHz, &ranges::kLimiterCeilingDb,
	      &ranges::kLimiterReleaseMs, &ranges::kMix, &ranges::kOutputGainDb}) {
		REQUIRE(r->min < r->max);
		REQUIRE(r->def >= r->min);
		REQUIRE(r->def <= r->max);
		REQUIRE(r->step > 0);
	}
}

TEST_CASE("Factory presets are valid, unique and already in range", "[params][presets]")
{
	size_t count = 0;
	const Preset *list = presets(count);
	REQUIRE(count == 5);

	std::set<std::string> ids;
	for (size_t i = 0; i < count; ++i) {
		INFO("preset " << list[i].id);
		CHECK(ids.insert(list[i].id).second);
		CHECK(std::strcmp(list[i].id, kCustomPresetId) != 0);
		CHECK(std::strncmp(list[i].localeKey, "Preset.", 7) == 0);
		// A preset must survive sanitize unchanged, or the UI would
		// immediately flip it to "Custom".
		CHECK(presetMatches(sanitize(list[i].params), list[i].params));
		CHECK(findPreset(list[i].id) == &list[i]);
	}
	CHECK(findPreset(kDefaultPresetId) != nullptr);
	CHECK(findPreset(kCustomPresetId) == nullptr);
	CHECK(findPreset("does-not-exist") == nullptr);
	CHECK(findPreset(nullptr) == nullptr);
}

TEST_CASE("Presets are ordered from gentle to heavy", "[params][presets]")
{
	const BassParams &subtle = findPreset("subtle")->params;
	const BassParams &extreme = findPreset("extreme")->params;
	const BassParams &restore = findPreset("restore")->params;
	CHECK(subtle.shelfGainDb < extreme.shelfGainDb);
	CHECK_FALSE(subtle.harmonicsEnabled);
	CHECK_FALSE(subtle.subEnabled);
	CHECK(restore.harmonicsEnabled);
	CHECK(restore.subEnabled);
	CHECK(restore.limiterEnabled);
	CHECK(extreme.limiterEnabled);
}

TEST_CASE("presetMatches ignores bypass but detects any other change", "[params][presets]")
{
	const BassParams base = findPreset("voice")->params;
	BassParams p = base;
	p.bypass = true;
	CHECK(presetMatches(p, base));

	p = base;
	p.shelfGainDb += 0.1;
	CHECK_FALSE(presetMatches(p, base));

	p = base;
	p.subEnabled = !p.subEnabled;
	CHECK_FALSE(presetMatches(p, base));

	p = base;
	p.shelfGainDb += 1e-6; // float round-trip through OBS settings
	CHECK(presetMatches(p, base));
}
