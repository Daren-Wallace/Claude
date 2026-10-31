/*
Bass Booster for OBS - end-to-end test inside a real libobs instance
Copyright (C) 2026 Daren Wallace
SPDX-License-Identifier: GPL-2.0-or-later

This loads the *compiled plugin module* exactly as OBS Studio does, then:
  * checks the filter registers under the right id and localised name,
  * walks the generated properties UI and checks every control,
  * checks defaults and the preset <-> Custom switching logic,
  * attaches the filter to a real OBS audio source, pushes audio through
    libobs's own audio pipeline and measures what comes out,
  * checks Bypass and OBS's own "disable filter" (eye icon) are transparent,
  * shuts libobs down and verifies there are zero memory leaks.

libobs applies audio filters synchronously inside obs_source_output_audio()
and then invokes audio capture callbacks with the filtered data, so the test
is deterministic and needs no video, GPU or timing loop.
*/

#include <obs-module.h>
#include <obs.h>
#include <util/bmem.h>
#include <util/platform.h>

#include "bass-filter.h"
#include "dsp/bass_engine.hpp"
#include "dsp/param_fields.hpp"
#include "dsp/params.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
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
constexpr double kPi = 3.14159265358979323846;

// ------------------------------------------------- minimal test source ----

const char *toneGetName(void *)
{
	return "Bass Booster test tone";
}
void *toneCreate(obs_data_t *, obs_source_t *source)
{
	return source;
}
void toneDestroy(void *) {}

void registerTestSource()
{
	obs_source_info info = {};
	info.id = "bass_booster_test_tone";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = toneGetName;
	info.create = toneCreate;
	info.destroy = toneDestroy;
	obs_register_source(&info);
}

// ------------------------------------------------------ audio capture ----

struct Capture {
	std::mutex mutex;
	std::vector<float> left, right;

	void clear()
	{
		std::lock_guard<std::mutex> lock(mutex);
		left.clear();
		right.clear();
	}
};

void captureCallback(void *param, obs_source_t *, const struct audio_data *audio, bool)
{
	auto *c = static_cast<Capture *>(param);
	std::lock_guard<std::mutex> lock(c->mutex);
	const auto *l = reinterpret_cast<const float *>(audio->data[0]);
	const auto *r = reinterpret_cast<const float *>(audio->data[1]);
	c->left.insert(c->left.end(), l, l + audio->frames);
	c->right.insert(c->right.end(), r, r + audio->frames);
}

// Push stereo planar float audio through the source, 1024 frames at a time.
void pushAudio(obs_source_t *source, const std::vector<float> &left, const std::vector<float> &right)
{
	static uint64_t ts = os_gettime_ns();
	const size_t chunk = 1024;
	for (size_t off = 0; off < left.size(); off += chunk) {
		const size_t n = std::min(chunk, left.size() - off);
		obs_source_audio a = {};
		a.data[0] = reinterpret_cast<const uint8_t *>(left.data() + off);
		a.data[1] = reinterpret_cast<const uint8_t *>(right.data() + off);
		a.frames = static_cast<uint32_t>(n);
		a.speakers = SPEAKERS_STEREO;
		a.format = AUDIO_FORMAT_FLOAT_PLANAR;
		a.samples_per_sec = kRate;
		a.timestamp = ts;
		ts += static_cast<uint64_t>(n) * 1000000000ULL / kRate;
		obs_source_output_audio(source, &a);
	}
}

std::vector<float> sine(double freq, double amp, size_t frames)
{
	std::vector<float> v(frames);
	for (size_t i = 0; i < frames; ++i)
		v[i] = static_cast<float>(amp * std::sin(2.0 * kPi * freq * static_cast<double>(i) / kRate));
	return v;
}

std::vector<float> missingFundamental(double f0, size_t frames)
{
	std::vector<float> v(frames, 0.0f);
	for (size_t i = 0; i < frames; ++i) {
		double s = 0.0;
		for (int n = 3; n <= 10; ++n)
			s += std::sin(2.0 * kPi * f0 * n * static_cast<double>(i) / kRate + 0.4 * n) / n;
		v[i] = static_cast<float>(0.25 * s);
	}
	return v;
}

double rms(const std::vector<float> &v, size_t start)
{
	double acc = 0.0;
	for (size_t i = start; i < v.size(); ++i)
		acc += static_cast<double>(v[i]) * v[i];
	return std::sqrt(acc / static_cast<double>(v.size() - start));
}

double toneAmplitude(const std::vector<float> &v, double freq, size_t start, size_t len)
{
	double re = 0.0, im = 0.0, wsum = 0.0;
	for (size_t i = 0; i < len; ++i) {
		const double w =
			0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(len - 1));
		const double ph = 2.0 * kPi * freq * static_cast<double>(i) / kRate;
		re += v[start + i] * w * std::cos(ph);
		im -= v[start + i] * w * std::sin(ph);
		wsum += w;
	}
	return 2.0 * std::sqrt(re * re + im * im) / wsum;
}

double db(double x)
{
	return 20.0 * std::log10(x > 1e-15 ? x : 1e-15);
}

// Measure steady-state gain of a sine through the filter chain.
double measureGainDb(obs_source_t *source, Capture &cap, double freq)
{
	cap.clear();
	const auto in = sine(freq, 0.05, kRate);
	pushAudio(source, in, in);
	std::lock_guard<std::mutex> lock(cap.mutex);
	if (cap.left.size() != in.size())
		return -999.0;
	return db(rms(cap.left, kRate / 2) / rms(in, kRate / 2));
}

// --------------------------------------------------- properties helpers ----

obs_property_t *findPropertyRecursive(obs_properties_t *props, const char *name)
{
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		if (std::strcmp(obs_property_name(p), name) == 0)
			return p;
		if (obs_property_get_type(p) == OBS_PROPERTY_GROUP) {
			if (obs_property_t *found = findPropertyRecursive(obs_property_group_content(p), name))
				return found;
		}
	}
	return nullptr;
}

void applyLinearSettings(obs_data_t *s, double shelfGain, double shelfFreq)
{
	obs_data_set_string(s, "preset", "custom");
	obs_data_set_bool(s, "bypass", false);
	obs_data_set_double(s, "input_gain_db", 0.0);
	obs_data_set_double(s, "shelf_gain_db", shelfGain);
	obs_data_set_double(s, "shelf_freq_hz", shelfFreq);
	obs_data_set_double(s, "shelf_q", 0.707);
	obs_data_set_bool(s, "punch_enabled", false);
	obs_data_set_bool(s, "harmonics_enabled", false);
	obs_data_set_bool(s, "sub_enabled", false);
	obs_data_set_bool(s, "limiter_enabled", false);
	obs_data_set_double(s, "rumble_freq_hz", 20.0);
	obs_data_set_double(s, "mix", 100.0);
	obs_data_set_double(s, "output_gain_db", 0.0);
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

	std::printf("[2] Loading plugin module %s\n", BASS_BOOSTER_MODULE_PATH);
	obs_module_t *module = nullptr;
	REQUIRE(obs_open_module(&module, BASS_BOOSTER_MODULE_PATH, BASS_BOOSTER_DATA_PATH) == MODULE_SUCCESS);
	REQUIRE(obs_init_module(module));
	CHECK(std::strcmp(obs_get_module_name(module), "Bass Booster") == 0);
	CHECK(std::strstr(obs_get_module_description(module), "bass") != nullptr);

	std::printf("[3] Checking filter registration\n");
	bool listed = false;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_filter_types(i, &id); ++i)
		listed = listed || std::strcmp(id, BASS_BOOSTER_FILTER_ID) == 0;
	CHECK(listed);
	CHECK(obs_get_source_output_flags(BASS_BOOSTER_FILTER_ID) & OBS_SOURCE_AUDIO);
	const char *displayName = obs_source_get_display_name(BASS_BOOSTER_FILTER_ID);
	REQUIRE(displayName != nullptr);
	CHECK(std::strcmp(displayName, "Bass Booster") == 0); // proves the locale file loaded

	std::printf("[4] Checking defaults\n");
	{
		obs_data_t *defaults = obs_get_source_defaults(BASS_BOOSTER_FILTER_ID);
		REQUIRE(defaults);
		CHECK(std::strcmp(obs_data_get_string(defaults, "preset"), bassbooster::kDefaultPresetId) == 0);
		const bassbooster::BassParams &def = bassbooster::findPreset(bassbooster::kDefaultPresetId)->params;
		for (const auto &f : bassbooster::kFloatFields)
			CHECK(std::fabs(obs_data_get_double(defaults, f.key) - def.*f.member) < 1e-9);
		CHECK(!obs_data_get_bool(defaults, "bypass"));
		obs_data_release(defaults);
	}

	std::printf("[5] Checking the properties UI\n");
	{
		obs_properties_t *props = obs_get_source_properties(BASS_BOOSTER_FILTER_ID);
		REQUIRE(props);
		for (const auto &f : bassbooster::kFloatFields) {
			obs_property_t *p = findPropertyRecursive(props, f.key);
			CHECK(p != nullptr);
			if (!p)
				continue;
			CHECK(obs_property_get_type(p) == OBS_PROPERTY_FLOAT);
			CHECK(obs_property_float_type(p) == OBS_NUMBER_SLIDER);
			CHECK(obs_property_float_min(p) == f.range->min);
			CHECK(obs_property_float_max(p) == f.range->max);
			CHECK(std::strcmp(obs_property_float_suffix(p), f.suffix) == 0);
			const char *desc = obs_property_description(p);
			const char *tip = obs_property_long_description(p);
			CHECK(desc && *desc && std::strcmp(desc, f.text) != 0); // localised, not the raw key
			CHECK(tip && std::strstr(tip, ".Tooltip") == nullptr);
		}
		for (const char *group : {"punch_enabled", "harmonics_enabled", "sub_enabled", "limiter_enabled"}) {
			obs_property_t *p = findPropertyRecursive(props, group);
			CHECK(p && obs_property_get_type(p) == OBS_PROPERTY_GROUP);
			CHECK(p && obs_property_group_type(p) == OBS_GROUP_CHECKABLE);
		}
		obs_property_t *preset = obs_properties_get(props, "preset");
		REQUIRE(preset);
		CHECK(obs_property_list_item_count(preset) == 6);
		CHECK(std::strcmp(obs_property_list_item_string(preset, 5), "custom") == 0);
		CHECK(obs_properties_get(props, "bypass") != nullptr);
		obs_property_t *help = obs_properties_get(props, "help");
		CHECK(help && obs_property_button_type(help) == OBS_BUTTON_URL);
		CHECK(help && std::strstr(obs_property_button_url(help), "USER_GUIDE") != nullptr);
		obs_properties_destroy(props);
	}

	std::printf("[6] Attaching the filter to a live audio source\n");
	obs_source_t *source = obs_source_create("bass_booster_test_tone", "tone", nullptr, nullptr);
	REQUIRE(source);
	obs_data_t *settings = obs_data_create();
	applyLinearSettings(settings, 8.0, 120.0);
	obs_source_t *filter = obs_source_create_private(BASS_BOOSTER_FILTER_ID, "Bass Booster", settings);
	REQUIRE(filter);
	obs_source_filter_add(source, filter);
	Capture cap;
	obs_source_add_audio_capture_callback(source, captureCallback, &cap);

	bassbooster::BassEngine reference;
	reference.prepare(kRate, 2);
	{
		bassbooster::BassParams p;
		p.shelfGainDb = 8.0;
		p.shelfFreqHz = 120.0;
		p.punchEnabled = false;
		p.harmonicsEnabled = false;
		p.subEnabled = false;
		p.limiterEnabled = false;
		p.rumbleFreqHz = 20.0;
		reference.setParams(p);
	}

	std::printf("[7] Measuring the frequency response through libobs\n");
	for (double f : {40.0, 80.0, 1000.0, 6000.0}) {
		const double measured = measureGainDb(source, cap, f);
		const double expected = reference.linearResponseDb(f);
		std::printf("    %6.0f Hz: measured %+6.2f dB, expected %+6.2f dB\n", f, measured, expected);
		CHECK(std::fabs(measured - expected) < 0.2);
	}

	std::printf("[8] Live settings update (obs_source_update)\n");
	applyLinearSettings(settings, 16.0, 120.0);
	obs_source_update(filter, settings);
	{
		const double g = measureGainDb(source, cap, 40.0);
		std::printf("    40 Hz after raising boost to +16 dB: %+6.2f dB\n", g);
		CHECK(g > 14.0);
	}

	std::printf("[9] Bypass and OBS's own filter toggle are transparent\n");
	{
		const auto in = sine(70.0, 0.3, kRate);
		obs_data_set_bool(settings, "bypass", true);
		obs_source_update(filter, settings);
		cap.clear();
		pushAudio(source, in, in);
		bool identical = cap.left.size() == in.size();
		for (size_t i = 4096; identical && i < in.size(); ++i) // after the 20 ms crossfade
			identical = cap.left[i] == in[i];
		CHECK(identical);

		obs_data_set_bool(settings, "bypass", false);
		obs_source_update(filter, settings);
		obs_source_set_enabled(filter, false); // the eye icon in the Filters dialog
		cap.clear();
		pushAudio(source, in, in);
		identical = cap.left.size() == in.size();
		for (size_t i = 0; identical && i < in.size(); ++i)
			identical = cap.left[i] == in[i];
		CHECK(identical);
		obs_source_set_enabled(filter, true);
	}

	std::printf("[10] Restore preset on audio with no bass at all\n");
	{
		// Exactly what the preset list writes when the user picks "Restore".
		const bassbooster::BassParams &p = bassbooster::findPreset("restore")->params;
		obs_data_t *restore = obs_data_create();
		obs_data_set_string(restore, "preset", "restore");
		obs_data_set_bool(restore, "bypass", false);
		for (const auto &f : bassbooster::kFloatFields)
			obs_data_set_double(restore, f.key, p.*f.member);
		for (const auto &b : bassbooster::kBoolFields)
			if (b.inPreset)
				obs_data_set_bool(restore, b.key, p.*b.member);
		obs_source_update(filter, restore);
		obs_data_release(restore);

		const auto in = missingFundamental(110.0, kRate * 2);
		cap.clear();
		pushAudio(source, in, in);
		const double before = toneAmplitude(in, 110.0, kRate, 32768);
		const double after = toneAmplitude(cap.left, 110.0, kRate, 32768);
		std::printf("    110 Hz fundamental: input %.1f dBFS -> output %.1f dBFS\n", db(before), db(after));
		CHECK(db(before) < -80.0);
		CHECK(db(after) > -35.0);
		double maxPeak = 0.0;
		for (float v : cap.left)
			maxPeak = std::fmax(maxPeak, std::fabs(static_cast<double>(v)));
		CHECK(maxPeak <= std::pow(10.0, -1.0 / 20.0) + 1e-6); // limiter ceiling -1 dBFS
	}

	std::printf("[11] Preset <-> Custom switching in the properties dialog\n");
	{
		obs_data_t *s = obs_source_get_settings(filter);
		obs_properties_t *props = obs_source_properties(filter);
		obs_property_t *preset = obs_properties_get(props, "preset");

		// Opening the dialog must not change anything (callbacks fire on load).
		obs_data_set_string(s, "preset", "restore");
		CHECK(!obs_property_modified(preset, s));
		CHECK(std::strcmp(obs_data_get_string(s, "preset"), "restore") == 0);

		// Picking a preset writes all of its values.
		obs_data_set_string(s, "preset", "voice");
		CHECK(obs_property_modified(preset, s));
		const bassbooster::BassParams &voice = bassbooster::findPreset("voice")->params;
		CHECK(obs_data_get_double(s, "shelf_freq_hz") == voice.shelfFreqHz);
		CHECK(obs_data_get_bool(s, "sub_enabled") == voice.subEnabled);

		// Touching a slider that still matches leaves the preset alone...
		obs_property_t *shelf = findPropertyRecursive(props, "shelf_gain_db");
		CHECK(!obs_property_modified(shelf, s));
		CHECK(std::strcmp(obs_data_get_string(s, "preset"), "voice") == 0);

		// ...but changing it switches the list to Custom.
		obs_data_set_double(s, "shelf_gain_db", voice.shelfGainDb + 2.0);
		CHECK(obs_property_modified(shelf, s));
		CHECK(std::strcmp(obs_data_get_string(s, "preset"), "custom") == 0);

		// So does ticking a checkable group.
		obs_data_set_string(s, "preset", "voice");
		obs_property_modified(preset, s);
		obs_data_set_bool(s, "sub_enabled", !voice.subEnabled);
		CHECK(obs_property_modified(findPropertyRecursive(props, "sub_enabled"), s));
		CHECK(std::strcmp(obs_data_get_string(s, "preset"), "custom") == 0);

		// Choosing Custom never overwrites the user's values.
		const double before = obs_data_get_double(s, "shelf_gain_db");
		CHECK(!obs_property_modified(preset, s));
		CHECK(obs_data_get_double(s, "shelf_gain_db") == before);

		obs_properties_destroy(props);
		obs_data_release(s);
	}

	std::printf("[12] Shutting down and checking for leaks\n");
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
	// libobs on Linux needs a display connection even for audio-only use.
	// Without one (e.g. a bare CI container without Xvfb), report SKIP.
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
