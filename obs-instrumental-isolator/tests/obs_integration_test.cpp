/*
Instrumental Isolator for OBS - end-to-end test inside a real libobs instance
Copyright (C) 2026 Daren Wallace
SPDX-License-Identifier: GPL-2.0-or-later

Loads the compiled plugin module exactly as OBS Studio does, checks its
registration and properties UI, pushes the ground-truth test mix through a
real OBS audio source with the filter attached, measures the separation that
comes out of libobs's own pipeline, exercises presets, Vocals-only mode,
Bypass and OBS's filter toggle, then verifies zero leaked allocations.

libobs runs audio filters synchronously inside obs_source_output_audio() and
then fires audio capture callbacks with the filtered buffer, so this is
deterministic and needs no GPU or timing loop.
*/

#include <obs-module.h>
#include <obs.h>
#include <util/bmem.h>
#include <util/platform.h>

#include "isolator-filter.h"
#include "dsp/param_fields.hpp"
#include "dsp/params.hpp"
#include "testmix.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                       \
	do {                                                                              \
		++g_checks;                                                               \
		if (!(cond)) {                                                            \
			++g_failures;                                                     \
			std::fprintf(stderr, "  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                         \
	} while (0)

#define REQUIRE(cond)                                                                       \
	do {                                                                                \
		++g_checks;                                                                 \
		if (!(cond)) {                                                              \
			std::fprintf(stderr, "  FATAL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return 1;                                                           \
		}                                                                           \
	} while (0)

constexpr uint32_t kRate = 48000;

const char *srcName(void *)
{
	return "Isolator test source";
}
void *srcCreate(obs_data_t *, obs_source_t *source)
{
	return source;
}
void srcDestroy(void *) {}

void registerTestSource()
{
	obs_source_info info = {};
	info.id = "isolator_test_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = srcName;
	info.create = srcCreate;
	info.destroy = srcDestroy;
	obs_register_source(&info);
}

struct Capture {
	std::mutex mutex;
	testmix::Stereo audio;
	void clear()
	{
		std::lock_guard<std::mutex> lock(mutex);
		audio.left.clear();
		audio.right.clear();
	}
};

void captureCallback(void *param, obs_source_t *, const struct audio_data *audio, bool)
{
	auto *c = static_cast<Capture *>(param);
	std::lock_guard<std::mutex> lock(c->mutex);
	const auto *l = reinterpret_cast<const float *>(audio->data[0]);
	const auto *r = reinterpret_cast<const float *>(audio->data[1]);
	c->audio.left.insert(c->audio.left.end(), l, l + audio->frames);
	c->audio.right.insert(c->audio.right.end(), r, r + audio->frames);
}

void pushAudio(obs_source_t *source, const testmix::Stereo &s)
{
	static uint64_t ts = os_gettime_ns();
	const size_t chunk = 1024;
	for (size_t off = 0; off < s.left.size(); off += chunk) {
		const size_t n = std::min(chunk, s.left.size() - off);
		obs_source_audio a = {};
		a.data[0] = reinterpret_cast<const uint8_t *>(s.left.data() + off);
		a.data[1] = reinterpret_cast<const uint8_t *>(s.right.data() + off);
		a.frames = static_cast<uint32_t>(n);
		a.speakers = SPEAKERS_STEREO;
		a.format = AUDIO_FORMAT_FLOAT_PLANAR;
		a.samples_per_sec = kRate;
		a.timestamp = ts;
		ts += static_cast<uint64_t>(n) * 1000000000ULL / kRate;
		obs_source_output_audio(source, &a);
	}
}

obs_property_t *findPropertyRecursive(obs_properties_t *props, const char *name)
{
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		if (std::strcmp(obs_property_name(p), name) == 0)
			return p;
		if (obs_property_get_type(p) == OBS_PROPERTY_GROUP)
			if (obs_property_t *found = findPropertyRecursive(obs_property_group_content(p), name))
				return found;
	}
	return nullptr;
}

void writePreset(obs_data_t *s, const char *id, bool makeup)
{
	const isolator::IsolatorParams &p = isolator::findPreset(id)->params;
	obs_data_set_string(s, "preset", id);
	obs_data_set_bool(s, "bypass", false);
	obs_data_set_int(s, isolator::kOutputModeKey, static_cast<long long>(p.outputMode));
	for (const auto &f : isolator::kFloatFields)
		obs_data_set_double(s, f.key, p.*f.member);
	for (const auto &b : isolator::kBoolFields)
		if (b.inPreset)
			obs_data_set_bool(s, b.key, p.*b.member);
	obs_data_set_bool(s, "auto_makeup", makeup);
}

struct Result {
	double vocal, guitar, keys, bass;
};

Result runMix(obs_source_t *source, Capture &cap, const testmix::Mix &m)
{
	cap.clear();
	pushAudio(source, m.mix);
	std::lock_guard<std::mutex> lock(cap.mutex);
	Result r{0, 0, 0, 0};
	if (cap.audio.left.size() != m.mix.left.size())
		return Result{999, 999, 999, 999};
	const size_t len = 96000, start = cap.audio.left.size() - len;
	r.vocal = testmix::stemChangeDb(m.vocalStem, m.mix, cap.audio, kRate, start, len);
	r.guitar = testmix::stemChangeDb(m.guitarStem, m.mix, cap.audio, kRate, start, len);
	r.keys = testmix::stemChangeDb(m.keysStem, m.mix, cap.audio, kRate, start, len);
	r.bass = testmix::stemChangeDb(m.bassStem, m.mix, cap.audio, kRate, start, len);
	return r;
}

int run()
{
	std::printf("[1] Starting libobs %s\n", obs_get_version_string());
	REQUIRE(obs_startup("en-US", nullptr, nullptr));
	obs_audio_info ai = {};
	ai.samples_per_sec = kRate;
	ai.speakers = SPEAKERS_STEREO;
	REQUIRE(obs_reset_audio(&ai));
	registerTestSource();

	std::printf("[2] Loading plugin module %s\n", ISOLATOR_MODULE_PATH);
	obs_module_t *module = nullptr;
	REQUIRE(obs_open_module(&module, ISOLATOR_MODULE_PATH, ISOLATOR_DATA_PATH) == MODULE_SUCCESS);
	REQUIRE(obs_init_module(module));
	CHECK(std::strcmp(obs_get_module_name(module), "Instrumental Isolator") == 0);
	CHECK(std::strstr(obs_get_module_description(module), "vocals") != nullptr);

	std::printf("[3] Checking filter registration\n");
	bool listed = false;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_filter_types(i, &id); ++i)
		listed = listed || std::strcmp(id, INSTRUMENTAL_ISOLATOR_FILTER_ID) == 0;
	CHECK(listed);
	CHECK(obs_get_source_output_flags(INSTRUMENTAL_ISOLATOR_FILTER_ID) & OBS_SOURCE_AUDIO);
	const char *displayName = obs_source_get_display_name(INSTRUMENTAL_ISOLATOR_FILTER_ID);
	REQUIRE(displayName != nullptr);
	CHECK(std::strcmp(displayName, "Instrumental Isolator (Vocal Remover)") == 0);

	std::printf("[4] Checking defaults\n");
	{
		obs_data_t *d = obs_get_source_defaults(INSTRUMENTAL_ISOLATOR_FILTER_ID);
		REQUIRE(d);
		CHECK(std::strcmp(obs_data_get_string(d, "preset"), isolator::kDefaultPresetId) == 0);
		const isolator::IsolatorParams &def = isolator::findPreset(isolator::kDefaultPresetId)->params;
		for (const auto &f : isolator::kFloatFields)
			CHECK(std::fabs(obs_data_get_double(d, f.key) - def.*f.member) < 1e-9);
		CHECK(obs_data_get_int(d, isolator::kOutputModeKey) == 0);
		CHECK(!obs_data_get_bool(d, "bypass"));
		obs_data_release(d);
	}

	std::printf("[5] Checking the properties UI\n");
	{
		// obs_module_text() falls back to the raw key, so ask the module's
		// locale directly whether each label and tooltip really exists.
		obs_properties_t *props = obs_get_source_properties(INSTRUMENTAL_ISOLATOR_FILTER_ID);
		REQUIRE(props);
		for (const auto &f : isolator::kFloatFields) {
			obs_property_t *p = findPropertyRecursive(props, f.key);
			CHECK(p != nullptr);
			if (!p)
				continue;
			CHECK(obs_property_get_type(p) == OBS_PROPERTY_FLOAT);
			CHECK(obs_property_float_min(p) == f.range->min);
			CHECK(obs_property_float_max(p) == f.range->max);
			CHECK(std::strcmp(obs_property_float_suffix(p), f.suffix) == 0);
			const char *desc = obs_property_description(p);
			const char *tip = obs_property_long_description(p);
			const char *translated = nullptr;
			CHECK(obs_module_get_locale_string(module, f.text, &translated)); // key exists in en-US.ini
			CHECK(desc && translated && std::strcmp(desc, translated) == 0);
			CHECK(tip && std::strstr(tip, ".Tooltip") == nullptr);
		}
		obs_property_t *pitch = findPropertyRecursive(props, "pitch_enabled");
		CHECK(pitch && obs_property_group_type(pitch) == OBS_GROUP_CHECKABLE);
		obs_property_t *makeup = findPropertyRecursive(props, "auto_makeup");
		CHECK(makeup && obs_property_get_type(makeup) == OBS_PROPERTY_BOOL);
		obs_property_t *mode = obs_properties_get(props, isolator::kOutputModeKey);
		CHECK(mode && obs_property_list_item_count(mode) == 2);
		obs_property_t *preset = obs_properties_get(props, "preset");
		REQUIRE(preset);
		CHECK(obs_property_list_item_count(preset) == 6);
		obs_property_t *help = obs_properties_get(props, "help");
		CHECK(help && obs_property_button_type(help) == OBS_BUTTON_URL);
		CHECK(help && std::strstr(obs_property_button_url(help), "obs-instrumental-isolator") != nullptr);
		obs_properties_destroy(props);
	}

	std::printf("[6] Attaching the filter to a live stereo source\n");
	obs_source_t *source = obs_source_create("isolator_test_source", "music", nullptr, nullptr);
	REQUIRE(source);
	obs_data_t *settings = obs_data_create();
	writePreset(settings, "karaoke", false);
	obs_source_t *filter = obs_source_create_private(INSTRUMENTAL_ISOLATOR_FILTER_ID, "Isolator", settings);
	REQUIRE(filter);
	obs_source_filter_add(source, filter);
	Capture cap;
	obs_source_add_audio_capture_callback(source, captureCallback, &cap);

	const testmix::Mix mix = testmix::make(kRate, 6.0);

	std::printf("[7] Separation measured through libobs (Karaoke preset)\n");
	{
		const Result r = runMix(source, cap, mix);
		std::printf("    vocal %+.1f dB | guitar %+.1f dB | keys %+.1f dB | bass %+.1f dB\n", r.vocal, r.guitar,
			    r.keys, r.bass);
		CHECK(r.vocal <= -15.0);
		CHECK(std::fabs(r.guitar) <= 1.5);
		CHECK(std::fabs(r.keys) <= 1.5);
		CHECK(std::fabs(r.bass) <= 0.5);
	}

	std::printf("[8] Live switch to Vocals-only (obs_source_update)\n");
	{
		writePreset(settings, "vocals", false);
		obs_source_update(filter, settings);
		const Result r = runMix(source, cap, mix);
		std::printf("    vocal %+.1f dB | guitar %+.1f dB | bass %+.1f dB\n", r.vocal, r.guitar, r.bass);
		CHECK(r.vocal >= -3.0);
		CHECK(r.guitar <= -6.0);
		CHECK(r.bass <= -20.0);
	}

	std::printf("[9] Bypass and OBS's own filter toggle are transparent\n");
	{
		obs_data_set_bool(settings, "bypass", true);
		obs_source_update(filter, settings);
		cap.clear();
		pushAudio(source, mix.mix);
		bool identical = cap.audio.left.size() == mix.mix.left.size();
		for (size_t i = 4096; identical && i < mix.mix.left.size(); ++i)
			identical = cap.audio.left[i] == mix.mix.left[i] && cap.audio.right[i] == mix.mix.right[i];
		CHECK(identical);

		obs_data_set_bool(settings, "bypass", false);
		obs_source_update(filter, settings);
		obs_source_set_enabled(filter, false);
		cap.clear();
		pushAudio(source, mix.mix);
		identical = cap.audio.left.size() == mix.mix.left.size();
		for (size_t i = 0; identical && i < mix.mix.left.size(); ++i)
			identical = cap.audio.left[i] == mix.mix.left[i];
		CHECK(identical);
		obs_source_set_enabled(filter, true);
	}

	std::printf("[10] Preset <-> Custom switching in the properties dialog\n");
	{
		writePreset(settings, "karaoke", true);
		obs_source_update(filter, settings);
		obs_data_t *s = obs_source_get_settings(filter);
		obs_properties_t *props = obs_source_properties(filter);
		obs_property_t *preset = obs_properties_get(props, "preset");

		CHECK(!obs_property_modified(preset, s)); // opening the dialog changes nothing
		obs_data_set_string(s, "preset", "aggressive");
		CHECK(obs_property_modified(preset, s));
		CHECK(obs_data_get_double(s, "strength") == isolator::findPreset("aggressive")->params.strength);

		obs_property_t *strength = findPropertyRecursive(props, "strength");
		CHECK(!obs_property_modified(strength, s));
		obs_data_set_double(s, "strength", 42.0);
		CHECK(obs_property_modified(strength, s));
		CHECK(std::strcmp(obs_data_get_string(s, "preset"), "custom") == 0);

		obs_data_set_string(s, "preset", "karaoke");
		obs_property_modified(preset, s);
		obs_data_set_int(s, isolator::kOutputModeKey, 1);
		CHECK(obs_property_modified(obs_properties_get(props, isolator::kOutputModeKey), s));
		CHECK(std::strcmp(obs_data_get_string(s, "preset"), "custom") == 0);

		obs_properties_destroy(props);
		obs_data_release(s);
	}

	std::printf("[11] Shutting down and checking for leaks\n");
	obs_source_remove_audio_capture_callback(source, captureCallback, &cap);
	obs_source_filter_remove(source, filter);
	obs_source_release(filter);
	obs_source_release(source);
	obs_data_release(settings);
	obs_shutdown();
	const long leaks = bnum_allocs();
	std::printf("    libobs allocations outstanding after shutdown: %ld\n", leaks);
	CHECK(leaks == 0);
	return 0;
}

} // namespace

int main()
{
	const char *display = std::getenv("DISPLAY");
	const char *wayland = std::getenv("WAYLAND_DISPLAY");
	if ((!display || !*display) && (!wayland || !*wayland)) {
		std::printf("SKIPPED: no X11/Wayland display (run under xvfb-run)\n");
		return 77;
	}
	const int fatal = run();
	std::printf("\n%d checks, %d failures%s\n", g_checks, g_failures, fatal ? " (aborted early)" : "");
	return (fatal || g_failures) ? 1 : 0;
}
